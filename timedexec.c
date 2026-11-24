/* timedexec - runs a command and kills it if it goes over a CPU time,
   memory, or wall-clock limit.
   usage: timedexec [-c SECONDS] [-m MEGABYTES] [-t SECONDS] [-q] [-v] COMMAND [ARGS...] */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

/* exit codes, same idea as GNU timeout so scripts can tell what happened */
#define EXIT_LIMIT 124
#define EXIT_INTERNAL 125
#define EXIT_NOEXEC 126
#define EXIT_NOTFOUND 127

/* how often the parent checks the child: 10 ms in nanoseconds */
#define POLL_NS 10000000L

/* remembers why we killed the child, R_NONE means we didn't */
enum reason { R_NONE, R_CPU, R_MEM, R_WALL };

/* child's pid, global because the signal handler needs to see it.
   sig_atomic_t + volatile so reads/writes are safe inside a handler */
static volatile sig_atomic_t g_child = 0;

/* if timedexec gets Ctrl+C / SIGTERM etc, pass it on to the child
   so we don't leave an orphan running. kill() is safe to call in a handler */
static void forward_signal(int sig)
{
    pid_t c = g_child;
    if (c > 0)
        kill(c, sig);
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [-c SECONDS] [-m MEGABYTES] [-t SECONDS] [-q] [-v] COMMAND [ARGS...]\n"
            "  -c SECONDS    CPU time limit (user + system)\n"
            "  -m MEGABYTES  resident memory limit\n"
            "  -t SECONDS    wall-clock time limit\n"
            "  -q            quiet, suppress the summary\n"
            "  -v            verbose, print limits and PID\n"
            "  -h            show this help\n"
            "At least one of -c, -m, -t is required.\n"
            "Exit: 124 limit exceeded, 125 timedexec error, 126 not executable,\n"
            "      127 not found, 128+N killed by signal N, else the command's status.\n",
            prog);
}

/* turns a string into a positive number, rejects junk like "abc", "5x",
   "inf", "nan", 0 and negatives. this is our argument error handling */
static int parse_positive(const char *s, double *out)
{
    char *end;
    double v;

    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v) || v <= 0)
        return -1;
    *out = v;
    return 0;
}

/* CLOCK_MONOTONIC never jumps backwards if the system clock changes,
   so it's the right one for measuring elapsed time */
static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* reads the child's live CPU time and memory from /proc.
   returns -1 if it fails (like if the child just exited) */
static int sample(pid_t pid, double *cpu, double *rss_kb)
{
    char path[64];
    char buf[1024];
    FILE *f;
    char *p;
    unsigned long ut, st, pages;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* the process name is in () and can contain spaces or even ')',
       so find the LAST ')' and parse from there.
       skip fields 3-13, then grab field 14 (utime) and 15 (stime) */
    p = strrchr(buf, ')');
    if (!p)
        return -1;
    if (sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2)
        return -1;

    /* utime/stime are in clock ticks, divide by ticks per second (usually 100) */
    *cpu = (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);

    /* statm field 2 = resident pages = RAM actually being used right now */
    snprintf(path, sizeof path, "/proc/%d/statm", (int)pid);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%*u %lu", &pages) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *rss_kb = (double)pages * ((double)sysconf(_SC_PAGESIZE) / 1024.0);
    return 0;
}

/* only runs in the child: set kernel limits as a backup, then become the command */
static void child_exec(char **cmd, double cpu_limit, double mem_mb)
{
    struct rlimit rl;
    rlim_t secs;

    if (cpu_limit > 0) {
        /* RLIMIT_CPU only takes whole seconds so round up.
           soft limit -> kernel sends SIGXCPU, hard limit (1s later) -> SIGKILL.
           this is the safety net in case our polling is late */
        secs = (rlim_t)cpu_limit;
        if ((double)secs < cpu_limit)
            secs++;
        rl.rlim_cur = secs;
        rl.rlim_max = secs + 1;
        if (setrlimit(RLIMIT_CPU, &rl) != 0) {
            perror("timedexec: setrlimit(RLIMIT_CPU)");
            _exit(EXIT_INTERNAL);
        }
    }
    if (mem_mb > 0) {
        /* this only makes malloc fail, it doesn't kill anything, so the real
           memory check is the RSS polling in the parent. set to 2x so the
           parent's check (at 1x) usually fires first and gives a clean message */
        rl.rlim_cur = (rlim_t)(mem_mb * 2.0 * 1024.0 * 1024.0);
        rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_DATA, &rl) != 0) {
            perror("timedexec: setrlimit(RLIMIT_DATA)");
            _exit(EXIT_INTERNAL);
        }
    }

    /* execvp replaces this process with the command and searches PATH.
       it only returns if it failed, so anything below is the error path.
       _exit (not exit) so we don't flush a copy of the parent's stdio buffers */
    execvp(cmd[0], cmd);
    fprintf(stderr, "timedexec: %s: %s\n", cmd[0], strerror(errno));
    _exit(errno == ENOENT ? EXIT_NOTFOUND : EXIT_NOEXEC);
}

/* real entry point, main() below just calls this */
int timedexec_main(int argc, char **argv)
{
    double cpu_limit = 0, mem_mb = 0, wall_limit = 0;
    int quiet = 0, verbose = 0, opt;
    struct sigaction sa;
    struct rusage ru;
    struct timespec nap = { 0, POLL_NS };
    enum reason reason = R_NONE;
    pid_t pid, r;
    int status = 0;
    double start, elapsed;
    int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
    size_t i;
    int code;

    /* reset getopt so this works if it gets called more than once */
    optind = 1;

    /* leading '+' = stop at the first non-option, so in "timedexec -c 2 ls -l"
       the -l goes to ls and not to us. a ':' means that option takes a value */
    while ((opt = getopt(argc, argv, "+c:m:t:qvh")) != -1) {
        switch (opt) {
        case 'c':
            if (parse_positive(optarg, &cpu_limit) != 0) {
                fprintf(stderr, "timedexec: invalid CPU limit '%s'\n", optarg);
                return EXIT_INTERNAL;
            }
            break;
        case 'm':
            if (parse_positive(optarg, &mem_mb) != 0) {
                fprintf(stderr, "timedexec: invalid memory limit '%s'\n", optarg);
                return EXIT_INTERNAL;
            }
            break;
        case 't':
            if (parse_positive(optarg, &wall_limit) != 0) {
                fprintf(stderr, "timedexec: invalid wall limit '%s'\n", optarg);
                return EXIT_INTERNAL;
            }
            break;
        case 'q':
            quiet = 1;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            usage(stderr, argv[0]);
            return EXIT_INTERNAL;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "timedexec: no command given\n");
        usage(stderr, argv[0]);
        return EXIT_INTERNAL;
    }
    if (cpu_limit <= 0 && mem_mb <= 0 && wall_limit <= 0) {
        fprintf(stderr, "timedexec: at least one of -c, -m, -t is required\n");
        usage(stderr, argv[0]);
        return EXIT_INTERNAL;
    }

    /* install handlers BEFORE fork so there's no gap where a signal could
       kill us and leave the child running. exec resets caught signals back
       to default so the command itself behaves normally */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = forward_signal;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0) {
            perror("timedexec: sigaction");
            return EXIT_INTERNAL;
        }
    }

    start = now_sec();

    /* fork returns 0 in the child, the child's pid in the parent, -1 on error */
    pid = fork();
    if (pid < 0) {
        perror("timedexec: fork");
        return EXIT_INTERNAL;
    }
    if (pid == 0)
        child_exec(&argv[optind], cpu_limit, mem_mb);

    g_child = pid;

    if (verbose) {
        fprintf(stderr, "timedexec: pid=%d cpu=%.2fs mem=%.1fMB wall=%.2fs\n",
                (int)pid, cpu_limit, mem_mb, wall_limit);
    }

    /* main loop: WNOHANG means wait4 returns right away instead of blocking.
       0 = child still running, pid = child finished (and reaped), -1 = error.
       wait4 (not waitpid) so we also get rusage back for the summary */
    for (;;) {
        r = wait4(pid, &status, WNOHANG, &ru);
        if (r == pid)
            break;
        if (r < 0) {
            /* EINTR just means a signal interrupted the call, try again */
            if (errno == EINTR)
                continue;
            perror("timedexec: wait4");
            kill(pid, SIGKILL);
            return EXIT_INTERNAL;
        }

        /* only check limits if we haven't already decided to kill it */
        if (reason == R_NONE) {
            double cpu, rss;
            if (sample(pid, &cpu, &rss) == 0) {
                if (cpu_limit > 0 && cpu >= cpu_limit)
                    reason = R_CPU;
                else if (mem_mb > 0 && rss > mem_mb * 1024.0)
                    reason = R_MEM;
            }
            if (reason == R_NONE && wall_limit > 0 && now_sec() - start >= wall_limit)
                reason = R_WALL;

            /* SIGKILL can't be caught or ignored so the child can't dodge it */
            if (reason != R_NONE)
                kill(pid, SIGKILL);
        }

        /* sleep so we don't burn 100% CPU just watching.
           tradeoff: a limit can be overshot by up to ~10 ms */
        nanosleep(&nap, NULL);
    }

    elapsed = now_sec() - start;

    /* child is reaped now, clear the pid so the handler can't signal
       some random process that reused the same pid */
    g_child = 0;

    /* if the kernel's RLIMIT_CPU got there first, the child died from SIGXCPU.
       still count that as a CPU limit hit */
    if (reason == R_NONE && WIFSIGNALED(status) && WTERMSIG(status) == SIGXCPU)
        reason = R_CPU;

    /* pick the exit code: 124 if we killed it for a limit,
       128+signal if something else killed it, otherwise the command's own code.
       WIFSIGNALED / WTERMSIG / WEXITSTATUS decode the packed status int */
    if (reason != R_NONE) {
        code = EXIT_LIMIT;
        if (!quiet) {
            switch (reason) {
            case R_CPU:
                fprintf(stderr, "timedexec: killed, CPU time limit (%.2fs) exceeded\n", cpu_limit);
                break;
            case R_MEM:
                fprintf(stderr, "timedexec: killed, memory limit (%.1fMB) exceeded\n", mem_mb);
                break;
            case R_WALL:
                fprintf(stderr, "timedexec: killed, wall-clock limit (%.2fs) exceeded\n", wall_limit);
                break;
            default:
                break;
            }
        }
    } else if (WIFSIGNALED(status)) {
        code = 128 + WTERMSIG(status);
        if (!quiet)
            fprintf(stderr, "timedexec: command terminated by signal %d (%s)\n",
                    WTERMSIG(status), strsignal(WTERMSIG(status)));
    } else {
        code = WEXITSTATUS(status);
        if (!quiet)
            fprintf(stderr, "timedexec: command exited with status %d\n", code);
    }

    /* usage numbers from wait4's rusage. maxrss is peak memory in KB on Linux */
    if (!quiet) {
        fprintf(stderr, "timedexec: wall=%.3fs user=%.3fs sys=%.3fs maxrss=%ldKB\n",
                elapsed,
                (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6,
                (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6,
                ru.ru_maxrss);
    }
    return code;
}

/* compiling with -DCUSTOM_SHELL removes this main() so timedexec_main
   can be linked into another program without a duplicate main() */
#ifndef CUSTOM_SHELL
int main(int argc, char **argv)
{
    return timedexec_main(argc, argv);
}
#endif
