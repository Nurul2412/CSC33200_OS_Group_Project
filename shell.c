#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define MAX_ARGS 64

// Declare all five custom command functions
int sharedmempipe(int argc, char *argv[]);
int filecrypt_main(int argc, char *argv[]);
int loganalyzer_main(int argc, char *argv[]);
int processgroup(int argc, char *argv[]);
int timedexec_main(int argc, char *argv[]);


// Execute the corresponding command
int execute_command(int argc, char *argv[]) {

    if (strcmp(argv[0], "sharedmempipe") == 0) {
        return sharedmempipe(argc, argv);
    }
    else if (strcmp(argv[0], "filecrypt") == 0) {
        return filecrypt_main(argc, argv);
    }
    else if (strcmp(argv[0], "loganalyzer") == 0) {
        return loganalyzer_main(argc, argv);
    }
    else if (strcmp(argv[0], "processgroup") == 0) {
        return processgroup(argc, argv);
    }
    else if (strcmp(argv[0], "timedexec") == 0) {
        return timedexec_main(argc, argv);
    }

    fprintf(stderr, "Unknown command: %s\n", argv[0]);
    return 1;
}


// Display available commands
void display_help(void) {

    printf("\nAvailable Commands:\n");
    printf("  sharedmempipe  - Shared memory data transfer\n");
    printf("  filecrypt      - File encryption and decryption\n");
    printf("  loganalyzer    - Log file analysis\n");
    printf("  processgroup   - Process group management\n");
    printf("  timedexec      - Resource-limited program execution\n");
    printf("  help           - Display available commands\n");
    printf("  exit           - Exit Custom Shell\n\n");
}


// Main shell function
int main(void) {

    char *input = NULL;
    size_t capacity = 0;
    char *argv[MAX_ARGS];

    // Prevent Ctrl+C from terminating the main shell
    signal(SIGINT, SIG_IGN);

    printf("====================================\n");
    printf("       Group 3 - Custom Shell       \n");
    printf("====================================\n");
    printf("Type 'help' for available commands.\n\n");

    // Continuously accept commands from the user
    while (1) {

        printf("CustomShell> ");
        fflush(stdout);

        // Read the user's input
        if (getline(&input, &capacity, stdin) == -1) {
            break;
        }

        // Separate the command and its arguments
        int argc = 0;
        char *token = strtok(input, " \t\r\n");

        while (token != NULL && argc < MAX_ARGS - 1) {
            argv[argc++] = token;
            token = strtok(NULL, " \t\r\n");
        }

        // Validate the number of arguments
        if (token != NULL) {
            fprintf(stderr, "Too many arguments\n");
            continue;
        }

        // Terminate the argument array
        argv[argc] = NULL;

        // Ignore empty input
        if (argc == 0) {
            continue;
        }

        // Handle the exit command
        if (strcmp(argv[0], "exit") == 0) {
            break;
        }

        // Handle the help command
        if (strcmp(argv[0], "help") == 0) {
            display_help();
            continue;
        }

        // Flush output before creating a child process
        fflush(NULL);

        // Create a child process to execute the command
        pid_t pid = fork();

        if (pid == -1) {
            perror("fork failed");
            continue;
        }

        if (pid == 0) {

            // Allow Ctrl+C to interrupt the running command
            signal(SIGINT, SIG_DFL);

            // Execute one of the five custom commands
            int result = execute_command(argc, argv);

            // Flush output before terminating
            fflush(NULL);

            _exit(result);
        }
        else {

            int status;
            pid_t result;

            // Wait for the child process to finish
            do {
                result = waitpid(pid, &status, 0);
            } while (result == -1 && errno == EINTR);

            // Handle process execution errors
            if (result == -1) {
                perror("waitpid failed");
            }
            else if (WIFSIGNALED(status)) {
                printf("Command terminated by signal %d\n",
                       WTERMSIG(status));
            }
            else if (WIFEXITED(status) &&
                     WEXITSTATUS(status) != 0) {

                printf("Command failed with exit status %d\n",
                       WEXITSTATUS(status));
            }
        }
    }

    // Release dynamically allocated memory
    free(input);

    printf("\nExiting Custom Shell...\n");

    return 0;
}