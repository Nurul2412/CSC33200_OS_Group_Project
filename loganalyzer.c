/*
 * Operating System I - Group Project (Custom Shell)
 */
#include <stdio.h>      
#include <string.h>     
#include <unistd.h>     
#include <fcntl.h>      
#include <signal.h>     
#include <sys/stat.h>   
#include <sys/mman.h>   

/*
 * This gets flipped to 1 when you hit Ctrl+C. The main loop keeps checking
 * it, so instead of the program just dying, it stops and still shows you
 * whatever it counted up to that point.
 */
static volatile sig_atomic_t stop_requested = 0;

/* Ctrl+C lands here. All we do is flip the flag and let main handle the rest. */
static void handle_sigint(int sig)
{
    stop_requested = 1;
}

/* Quick help message so you know what options you can use. */
static void usage(char *prog)
{
    printf("Usage: %s [-k keyword] [-t] [-v] [-h] <logfile>\n", prog);
    printf("  -k keyword  count lines containing keyword\n");
    printf("  -t          count log levels (ERROR, WARN, INFO, DEBUG)\n");
    printf("  -v          verbose (show file size)\n");
    printf("  -h          show this help\n");
}

/*
 * Looks for a word inside one line.
 * The line doesn't end with '\0' since it's just a spot inside the mapped
 * file, so we pass its length in ourselves. Then we slide along the line
 * and compare letter by letter. If every letter lines up, we found it.
 * Gives back 1 if it's there, 0 if it's not.
 */
static int line_contains(char *start, long len, char *word)
{
    long wlen = strlen(word);
    for (long i = 0; i + wlen <= len; i++) {
        long j = 0;
        while (j < wlen && start[i + j] == word[j])
            j++;
        if (j == wlen)
            return 1;
    }
    return 0;
}

int loganalyzer_main(int argc, char *argv[])
{
    char *keyword = NULL;
    int show_levels = 0, verbose = 0, opt;

    /* The shell might run this command more than once, so start fresh:
       optind = 1 makes getopt start reading options from the beginning
       again, and the Ctrl+C flag gets cleared from last time. */
    optind = 1;
    stop_requested = 0;

    /*
     * First, figure out which options were typed in.
     * getopt hands us one option at a time. "k:tvh" is the list of options
     * we accept, and the colon after k means -k needs a word after it
     * (getopt drops that word into optarg for us). Anything we don't
     * recognize just shows the help and quits.
     */
    while ((opt = getopt(argc, argv, "k:tvh")) != -1) {
        switch (opt) {
        case 'k': keyword = optarg; break;
        case 't': show_levels = 1; break;
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Whatever's left after the options should be the log file name.
       If there's nothing there, the user forgot to give us a file. */
    if (optind >= argc) {
        printf("Error: no log file given\n");
        usage(argv[0]);
        return 1;
    }
    char *path = argv[optind];

    /* Open the log file, read-only since we never change it.
       If it fails (like the file doesn't exist), perror says why. */
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror("Error opening log file");
        return 1;
    }

    /* We need to know how big the file is before we can map it,
       and fstat gives us that in st_size. */
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return 1;
    }
    long size = st.st_size;

    /* You can't map an empty file, so just say so and stop here. */
    if (size == 0) {
        printf("File is empty.\n");
        close(fd);
        return 0;
    }

    /*
     * This is the main trick: map the whole file into memory.
     *   NULL        - we don't care where it goes, let the OS choose
     *   size        - grab the entire file
     *   PROT_READ   - we're only reading
     *   MAP_PRIVATE - nobody else sees our copy
     *   fd, 0       - this file, from the very start
     * Now the file is basically just a big char array called data,
     * so we can loop through it without calling read() over and over.
     */
    char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("Error mapping log file");
        close(fd);
        return 1;
    }

    /* We can close the file now. The mapping still works without it. */
    close(fd);

    /* Now walk through the file line by line and count stuff. */
    char *levels[] = { "ERROR", "WARN", "INFO", "DEBUG" };
    long level_count[4] = { 0, 0, 0, 0 };
    long lines = 0, words = 0, keyword_hits = 0, empty_lines = 0;
    long longest = 0;

    /* Hook up our Ctrl+C handler right before the long part, so we can
       stop nicely in the middle of a big file. */
    signal(SIGINT, handle_sigint);

    long pos = 0;   /* where we are in the file right now */
    while (pos < size && !stop_requested) {
        char *line = data + pos;

        /* Keep moving forward until we hit a newline (or the end of
           the file). That tells us how long this line is. */
        long len = 0;
        while (pos + len < size && line[len] != '\n')
            len++;

        lines++;
        if (len == 0) empty_lines++;
        if (len > longest) longest = len;

        /* Counting words: every time we go from a space into a letter,
           that's the start of a new word. */
        int in_word = 0;
        for (long i = 0; i < len; i++) {
            char c = line[i];
            if (c == ' ' || c == '\t' || c == '\r') in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }

        /* If they used -k, check if this line has their keyword. */
        if (keyword && line_contains(line, len, keyword))
            keyword_hits++;

        /* If they used -t, figure out the log level. We stop at the
           first one we find so a line never gets counted twice. */
        if (show_levels)
            for (int l = 0; l < 4; l++)
                if (line_contains(line, len, levels[l])) {
                    level_count[l]++;
                    break;
                }

        /* Skip over this line and the newline to get to the next one. */
        pos += len + 1;
    }

    /* Done counting, time to show the results. */
    if (stop_requested)
        printf("\n[Interrupted by Ctrl+C - partial results]\n");

    printf("===== loganalyzer report: %s =====\n", path);
    printf("Total lines      : %ld\n", lines);
    printf("Empty lines      : %ld\n", empty_lines);
    printf("Total words      : %ld\n", words);
    printf("Longest line     : %ld chars\n", longest);
    if (keyword)
        printf("Lines with \"%s\" : %ld\n", keyword, keyword_hits);
    if (show_levels) {
        printf("--- Log levels ---\n");
        for (int l = 0; l < 4; l++)
            printf("  %-6s : %ld\n", levels[l], level_count[l]);
    }
    if (verbose)
        printf("File size        : %ld bytes\n", size);

    /* Clean up by unmapping the file. We return 130 if Ctrl+C stopped us,
       since that's the usual exit code for that, otherwise 0. */
    munmap(data, size);

    /* Put Ctrl+C back to normal so it doesn't mess with the rest of
       the shell after we're done. */
    signal(SIGINT, SIG_DFL);
    return stop_requested ? 130 : 0;
}