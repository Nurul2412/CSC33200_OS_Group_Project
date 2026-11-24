
/*
 * processgroup command
 * Mobin Ahmed
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <errno.h>

volatile sig_atomic_t processgroup_stop = 0;

// Handle Ctrl+C
void processgroup_signal(int sig) {
    (void)sig;
    processgroup_stop = 1;
}

// Run the processgroup command
int processgroup(int argc, char *argv[]) {
    int count = 3;
    int seconds = 5;
    int option;
    int created = 0;
    int status;
    int result = 0;

    pid_t pid[20];
    pid_t group = 0;

    struct sigaction action, old_int, old_term;

    processgroup_stop = 0;
    optind = 1;
    opterr = 0;

    // Read command options
    while ((option = getopt(argc, argv, "n:t:h")) != -1) {
        switch (option) {
            case 'n':
                count = atoi(optarg);
                break;

            case 't':
                seconds = atoi(optarg);
                break;

            case 'h':
                printf("Usage: processgroup [-n processes] [-t seconds]\n");
                return 0;

            default:
                printf("Invalid option.\n");
                return 1;
        }
    }

    if (optind != argc || count < 1 || count > 20 ||
        seconds < 1 || seconds > 60) {
        printf("Enter 1-20 processes and 1-60 seconds.\n");
        return 1;
    }

    // Set up signal handling
    action.sa_handler = processgroup_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(SIGINT, &action, &old_int) == -1) {
        perror("sigaction");
        return 1;
    }

    if (sigaction(SIGTERM, &action, &old_term) == -1) {
        perror("sigaction");
        sigaction(SIGINT, &old_int, NULL);
        return 1;
    }

    // Create child processes
    for (int i = 0; i < count; i++) {
        pid_t child = fork();

        if (child < 0) {
            perror("fork");
            result = 1;
            break;
        }

        if (child == 0) {
            signal(SIGINT, SIG_IGN);
            signal(SIGTERM, SIG_DFL);

            if (setpgid(0, group == 0 ? 0 : group) == -1) {
                perror("setpgid");
                _exit(1);
            }

            printf("Child %d started. PID: %ld\n",
                   i + 1, (long)getpid());
            fflush(stdout);

            while (1)
                pause();
        }

        if (group == 0)
            group = child;

        pid[created++] = child;

        if (setpgid(child, group) == -1) {
            perror("setpgid");
            result = 1;
            break;
        }
    }

    if (created > 0) {
        printf("Process group ID: %ld\n", (long)group);
        fflush(stdout);
    }

    // Monitor the group
    for (int i = 0; i < seconds && !processgroup_stop &&
                    result == 0; i++) {
        printf("Second %d: Monitoring %d processes\n",
               i + 1, created);
        fflush(stdout);
        sleep(1);
    }

    // Send one signal to the whole group
    if (created > 0) {
        printf("Stopping the process group...\n");
        fflush(stdout);

        if (kill(-group, SIGTERM) == -1) {
            perror("kill");
            result = 1;
        }
    }

    // Wait for children and check resource usage
    for (int i = 0; i < created; i++) {
        struct rusage usage;

        if (wait4(pid[i], &status, 0, &usage) == -1) {
            perror("wait4");
            result = 1;
        } else {
            printf("Child PID %ld finished\n", (long)pid[i]);
            printf("CPU time: %ld.%06ld seconds\n",
                   (long)usage.ru_utime.tv_sec,
                   (long)usage.ru_utime.tv_usec);
            printf("Maximum memory: %ld KB\n",
                   usage.ru_maxrss);
        }
    }

    // Restore the shell's signal handlers
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);

    printf("Processgroup command finished.\n");

    if (processgroup_stop)
        return 130;

    return result;
}

