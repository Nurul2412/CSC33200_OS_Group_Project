/*
 * Operating System I - Group Project (Custom Shell)
 * filecrypt command
 * Tausif Tusar
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>

/*
 * Every encrypted file starts with a 12 byte header:
 *   4 bytes "FCR1"     so we know it's one of our files
 *   8 bytes key check  a number made from the key, so a wrong key gets
 *                      caught before we write out garbage
 */
#define MAGIC      "FCR1"
#define MAGIC_LEN  4
#define HEADER_LEN 12
#define KEY_MAX    256

/*
 * The key lives here. It has a fixed spot in memory so we can lock it
 * with mlock() (so it never gets swapped to disk) and wipe it when we're
 * done.
 */
static char crypt_key[KEY_MAX];

/*
 * This gets flipped to 1 when you hit Ctrl+C. The main loop keeps checking
 * it, so instead of leaving half a file behind, we stop, delete the
 * partial output, and wipe the key.
 */
static volatile sig_atomic_t crypt_stop_requested = 0;

/* Ctrl+C lands here. All we do is flip the flag and let main handle the rest. */
static void crypt_handle_sigint(int sig)
{
    (void)sig;
    crypt_stop_requested = 1;
}

/* Quick help message so you know what options you can use. */
static void crypt_usage(char *prog)
{
    printf("Usage: %s -e|-d [-k key] [-o output] [-v] [-h] <file>\n", prog);
    printf("  -e          encrypt the file\n");
    printf("  -d          decrypt the file\n");
    printf("  -k key      key to use (if left out, you'll be asked for it)\n");
    printf("  -o output   output file (default: adds or removes .enc)\n");
    printf("  -v          verbose (show sizes, time and speed)\n");
    printf("  -h          show this help\n");
}

/*
 * Turns the key text into one big number (this is called FNV-1a hashing).
 * Start with a fixed number, then for each letter, mix it in and multiply
 * by a big prime. The same key always gives the same number.
 */
static uint64_t crypt_hash(char *key, int len, uint64_t start)
{
    uint64_t h = start;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)key[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/*
 * A tiny random number generator (called xorshift). Shift and XOR the
 * number a few times and you get a new random-looking number. The same
 * starting number always gives the same sequence, which is exactly what
 * we need to decrypt later.
 */
static uint64_t crypt_next(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/*
 * Asks for the key without showing it on screen.
 * We turn off echo on the terminal so nothing appears while you type, then
 * read one letter at a time with read(). We don't use fgets() because it
 * would leave a copy of the key in its own buffer that we can't wipe.
 * Ctrl+C is caught here too, so echo always gets turned back on. We use
 * sigaction without SA_RESTART so that Ctrl+C makes read() stop waiting.
 * Gives back the key length, or -1 if something went wrong.
 */
static int crypt_read_key(void)
{
    struct termios old_term, quiet;
    struct sigaction action, old_action;
    int is_tty = isatty(STDIN_FILENO);
    int len = 0;
    char c;

    if (is_tty) {
        fprintf(stderr, "Enter key: ");
        if (tcgetattr(STDIN_FILENO, &old_term) == -1) {
            perror("tcgetattr");
            return -1;
        }
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = crypt_handle_sigint;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_action) == -1) {
        perror("sigaction");
        return -1;
    }

    if (is_tty) {
        quiet = old_term;
        quiet.c_lflag &= ~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == -1) {
            perror("tcsetattr");
            sigaction(SIGINT, &old_action, NULL);
            return -1;
        }
    }

    while (len < KEY_MAX - 1 && !crypt_stop_requested) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == -1 && errno == EINTR)
            continue;
        if (n <= 0 || c == '\n')
            break;
        crypt_key[len++] = c;
    }
    crypt_key[len] = '\0';
    c = 0;

    /* Turn echo back on so the shell works normally again. */
    if (is_tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
        fprintf(stderr, "\n");
    }

    /* Give Ctrl+C back to the shell. */
    sigaction(SIGINT, &old_action, NULL);
    return len;
}

/*
 * Cleans up before we leave: unmaps whatever files are mapped and wipes
 * the key. We use explicit_bzero instead of memset because the compiler
 * is allowed to skip a memset it thinks is pointless, but it never skips
 * this one.
 */
static void crypt_cleanup(unsigned char *in, long in_size,
                          unsigned char *out, long out_size)
{
    if (in)
        munmap(in, in_size);
    if (out)
        munmap(out, out_size);
    explicit_bzero(crypt_key, sizeof(crypt_key));
    munlock(crypt_key, sizeof(crypt_key));
}

int filecrypt_main(int argc, char *argv[])
{
    char *key_arg = NULL, *outpath = NULL;
    int encrypt = -1, verbose = 0, opt;

    /* The shell might run this command more than once, so start fresh:
       optind = 1 makes getopt start reading options from the beginning
       again, and the Ctrl+C flag gets cleared from last time. */
    optind = 1;
    crypt_stop_requested = 0;

    /*
     * First, figure out which options were typed in.
     * "edk:o:vh" is the list of options we accept. The colons after k and
     * o mean they need a value after them (getopt drops it into optarg).
     * Anything we don't recognize just shows the help and quits.
     */
    while ((opt = getopt(argc, argv, "edk:o:vh")) != -1) {
        switch (opt) {
        case 'e': encrypt = 1; break;
        case 'd': encrypt = 0; break;
        case 'k': key_arg = optarg; break;
        case 'o': outpath = optarg; break;
        case 'v': verbose = 1; break;
        case 'h': crypt_usage(argv[0]); return 0;
        default:  crypt_usage(argv[0]); return 1;
        }
    }

    /* They have to pick encrypt or decrypt, and give us exactly one file. */
    if (encrypt == -1) {
        printf("Error: pick -e to encrypt or -d to decrypt\n");
        crypt_usage(argv[0]);
        return 1;
    }
    if (optind != argc - 1) {
        printf("Error: give exactly one file\n");
        crypt_usage(argv[0]);
        return 1;
    }
    char *inpath = argv[optind];

    /* Lock the key's memory so it never gets written to swap. Some systems
       limit how much you can lock, so if it fails we just keep going. */
    int locked = (mlock(crypt_key, sizeof(crypt_key)) == 0);

    /* Get the key, either from -k or by asking for it. */
    int key_len;
    if (key_arg) {
        key_len = strlen(key_arg);
        if (key_len >= KEY_MAX) {
            printf("Error: key is too long (max %d chars)\n", KEY_MAX - 1);
            crypt_cleanup(NULL, 0, NULL, 0);
            return 1;
        }
        memcpy(crypt_key, key_arg, key_len);
        /* Wipe the copy that was typed on the command line too. */
        explicit_bzero(key_arg, key_len);
    } else {
        key_len = crypt_read_key();
        if (crypt_stop_requested) {
            printf("[Interrupted by Ctrl+C]\n");
            crypt_cleanup(NULL, 0, NULL, 0);
            return 130;
        }
    }
    if (key_len <= 0) {
        printf("Error: no key given\n");
        crypt_cleanup(NULL, 0, NULL, 0);
        return 1;
    }

    /* If they didn't give an output name, make one up:
       encrypt: notes.txt     -> notes.txt.enc
       decrypt: notes.txt.enc -> notes.txt (or notes.txt.dec otherwise) */
    char default_out[4096];
    if (!outpath) {
        int n = strlen(inpath);
        if (encrypt)
            snprintf(default_out, sizeof(default_out), "%s.enc", inpath);
        else if (n > 4 && strcmp(inpath + n - 4, ".enc") == 0)
            snprintf(default_out, sizeof(default_out), "%.*s", n - 4, inpath);
        else
            snprintf(default_out, sizeof(default_out), "%s.dec", inpath);
        outpath = default_out;
    }

    /* Open the input file, read-only since we never change it. */
    int fd = open(inpath, O_RDONLY);
    if (fd == -1) {
        perror("Error opening input file");
        crypt_cleanup(NULL, 0, NULL, 0);
        return 1;
    }

    /* fstat tells us the file size and makes sure it's a normal file. */
    struct stat in_st, out_st;
    if (fstat(fd, &in_st) == -1 || !S_ISREG(in_st.st_mode)) {
        printf("Error: %s is not a regular file\n", inpath);
        close(fd);
        crypt_cleanup(NULL, 0, NULL, 0);
        return 1;
    }
    long in_size = in_st.st_size;

    /* Don't let the output be the same file as the input, or we'd wipe
       the input before reading it. */
    if (stat(outpath, &out_st) == 0 &&
        out_st.st_dev == in_st.st_dev && out_st.st_ino == in_st.st_ino) {
        printf("Error: output file can't be the same as the input file\n");
        close(fd);
        crypt_cleanup(NULL, 0, NULL, 0);
        return 1;
    }

    /*
     * Map the whole input file into memory, just like loganalyzer.
     * Now the file is basically a big array called in. You can't map an
     * empty file, but that's fine since there's nothing to read anyway.
     */
    unsigned char *in = NULL;
    if (in_size > 0) {
        in = mmap(NULL, in_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (in == MAP_FAILED) {
            perror("Error mapping input file");
            close(fd);
            crypt_cleanup(NULL, 0, NULL, 0);
            return 1;
        }
    }

    /* We can close the file now. The mapping still works without it. */
    close(fd);

    /* Turn the key into two numbers: one to check the key later, and one
       to start the random byte stream. */
    uint64_t check = crypt_hash(crypt_key, key_len, 0x84222325cbf29ce4ULL);
    uint64_t state = crypt_hash(crypt_key, key_len, 0xcbf29ce484222325ULL);
    if (state == 0)
        state = 1;      /* the generator gets stuck if it starts at 0 */

    /* When decrypting, check the header first, so a wrong key never
       creates or overwrites any file. */
    long out_size;
    if (encrypt) {
        out_size = in_size + HEADER_LEN;
    } else {
        uint64_t stored;
        if (in_size < HEADER_LEN || memcmp(in, MAGIC, MAGIC_LEN) != 0) {
            printf("Error: %s is not a filecrypt file\n", inpath);
            crypt_cleanup(in, in_size, NULL, 0);
            return 1;
        }
        memcpy(&stored, in + MAGIC_LEN, sizeof(stored));
        if (stored != check) {
            printf("Error: wrong key\n");
            crypt_cleanup(in, in_size, NULL, 0);
            return 1;
        }
        out_size = in_size - HEADER_LEN;
    }

    /* Create the output file. 0600 means only you can read or write it. */
    int out_fd = open(outpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (out_fd == -1) {
        perror("Error creating output file");
        crypt_cleanup(in, in_size, NULL, 0);
        return 1;
    }

    /*
     * A new file starts at size 0, and you can't map past the end of a
     * file, so we grow it to the final size first with ftruncate.
     * Then we map it with MAP_SHARED, which means anything we write into
     * the array goes straight into the real file.
     */
    unsigned char *out = NULL;
    if (ftruncate(out_fd, out_size) == -1) {
        perror("ftruncate");
        close(out_fd);
        unlink(outpath);
        crypt_cleanup(in, in_size, NULL, 0);
        return 1;
    }
    if (out_size > 0) {
        out = mmap(NULL, out_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   out_fd, 0);
        if (out == MAP_FAILED) {
            perror("Error mapping output file");
            close(out_fd);
            unlink(outpath);
            crypt_cleanup(in, in_size, NULL, 0);
            return 1;
        }
    }
    close(out_fd);

    /* Point at where the real data starts in each file.
       Encrypt: the input has no header, the output does.
       Decrypt: the input has a header, the output doesn't. */
    unsigned char *src = encrypt ? in : in + HEADER_LEN;
    unsigned char *dst = encrypt ? out + HEADER_LEN : out;
    long data_size = encrypt ? in_size : out_size;

    if (encrypt) {
        memcpy(out, MAGIC, MAGIC_LEN);
        memcpy(out + MAGIC_LEN, &check, sizeof(check));
    }

    /* Hook up our Ctrl+C handler right before the long part. We save the
       shell's old handler so we can put it back when we're done. */
    void (*old_handler)(int) = signal(SIGINT, crypt_handle_sigint);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /*
     * The main loop. Each byte gets XORed with the next random byte.
     * XOR undoes itself, so doing this again with the same key gives you
     * the original file back. We only check for Ctrl+C every 64 KB so the
     * loop stays fast.
     */
    for (long i = 0; i < data_size; i++) {
        if ((i & 0xFFFF) == 0 && crypt_stop_requested)
            break;
        dst[i] = src[i] ^ (unsigned char)(crypt_next(&state) >> 56);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    signal(SIGINT, old_handler);
    state = 0;
    check = 0;

    /* If Ctrl+C stopped us, delete the half-done file. We return 130
       since that's the usual exit code for Ctrl+C. */
    if (crypt_stop_requested) {
        printf("\n[Interrupted by Ctrl+C - removing partial output]\n");
        crypt_cleanup(in, in_size, out, out_size);
        unlink(outpath);
        return 130;
    }

    /* msync makes sure everything actually got written to disk. */
    if (out && msync(out, out_size, MS_SYNC) == -1) {
        perror("msync");
        crypt_cleanup(in, in_size, out, out_size);
        unlink(outpath);
        return 1;
    }

    /* Done, time to show the results. */
    printf("%s %s -> %s (%ld bytes)\n",
           encrypt ? "Encrypted" : "Decrypted", inpath, outpath, data_size);

    if (verbose) {
        double secs = (end.tv_sec - start.tv_sec) +
                      (end.tv_nsec - start.tv_nsec) / 1e9;
        printf("Input size   : %ld bytes\n", in_size);
        printf("Output size  : %ld bytes\n", out_size);
        printf("Time         : %.6f seconds\n", secs);
        if (secs > 0)
            printf("Speed        : %.2f MB/s\n", data_size / secs / 1e6);
        printf("Key locked   : %s\n", locked ? "yes" : "no");
    }

    crypt_cleanup(in, in_size, out, out_size);
    return 0;
}

