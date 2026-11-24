/*
Nurul Huda
Operating System Group 3 project
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h> 
#include <getopt.h>
#include <errno.h>


int sharedmempipe(int argc,char *argv[]){
    // Verify that the user provided an input and output filename
    size_t buffer_size = 4096;
    int opt;

    // Reset argument parsing for repeated calls from Custom Shell
    optind = 1;

    // Parse command-line options
    while ((opt = getopt(argc, argv, "b:h")) != -1) {
        switch (opt) {

            case 'b': {
                // Convert the supplied buffer size into a number
                char *end;
                errno = 0;
                long size = strtol(optarg, &end, 10);

                // Validate the buffer size (1 byte to 64 MB)
                if (errno != 0 || end == optarg || *end != '\0' ||
                    size <= 0 || size > 64L * 1024 * 1024) {
                    fprintf(stderr, "Invalid buffer size\n");
                    return 1;
                }

                buffer_size = (size_t)size;
                break;
            }

            case 'h':
                printf("Usage: sharedmempipe [-b size] <input> <output>\n");
                printf("-b    Buffer size in bytes (default: 4096)\n");
                printf("-h    Display help\n");
                return 0;

            default:
                fprintf(stderr, "Invalid command option\n");
                return 1;
        }
    }

    // Verify that two filenames remain after parsing options
    if (argc - optind != 2) {
        fprintf(stderr, "Usage: sharedmempipe [-b size] <input> <output>\n");
        return 1;
    }

    // Retrieve input and output filenames
    char *input_file = argv[optind];
    char *output_file = argv[optind + 1];

    // Prevent identical input and output filenames
    if (strcmp(input_file, output_file) == 0) {
        fprintf(stderr, "Input and output files must be different\n");
        return 1;
    
    }
  
    int pipefd[2];
    int sec_pipefd[2];
    if(pipe(pipefd)==-1){
        perror("Pipe creation failed");
        return 1;
    }
    if(pipe(sec_pipefd)==-1){
        perror("Second pipe creation failed");
        //Closing the first pipe because second pipe failed
        close(pipefd[0]);
        close(pipefd[1]);

        return 1;
    }

    // Create a shared memory region accessible by both processes
    void *shared_memory = mmap(
        NULL,
        buffer_size,
        PROT_READ | PROT_WRITE, // Allow reading and writing
        MAP_SHARED | MAP_ANONYMOUS, // Share memory without using a file
        -1,
        0
    );
    if(shared_memory==MAP_FAILED){
        perror("mmap failed");
        close(pipefd[0]);
        close(pipefd[1]);
        close(sec_pipefd[0]);
        close(sec_pipefd[1]);
        return 1;
    }

    int input_fd = open(input_file, O_RDONLY);
    if(input_fd == -1){
        perror("Failed to open file");
        munmap(shared_memory, buffer_size);
        close(pipefd[0]);
        close(pipefd[1]);
        close(sec_pipefd[0]);
        close(sec_pipefd[1]);

        return 1;

    }
    int output_fd = open(
        output_file,
        O_WRONLY | O_CREAT | O_TRUNC,
        0644

    );

    if(output_fd==-1){
        perror("Failed to open the output file");
        close(input_fd);
        munmap(shared_memory, buffer_size);
        close(pipefd[0]);
        close(pipefd[1]);
        close(sec_pipefd[0]);
        close(sec_pipefd[1]);

        return 1;
    }

    pid_t pid = fork();

    if(pid==-1){
        perror("fork failed");
        close(input_fd);
        close(output_fd);
        munmap(shared_memory, buffer_size);
        close(pipefd[0]);
        close(pipefd[1]);
        close(sec_pipefd[0]);
        close(sec_pipefd[1]);
        return 1;

    }
    if(pid==0){
        printf("Child process running\n");
        close(pipefd[1]);
        close(sec_pipefd[0]);
        close(input_fd);

        ssize_t bytes_available;
        char acknowledgement = 'D';
        while(read(pipefd[0], &bytes_available, sizeof(bytes_available))
        == sizeof(bytes_available)){
            if(bytes_available<=0 ||
            (size_t)bytes_available > buffer_size){
                fprintf(stderr, "Invalid byte count received\n");
                break;
            }
            if(write(output_fd,shared_memory,bytes_available)
            != bytes_available){
                perror("Failed to write output file");
                break;
            }
            if(write(sec_pipefd[1], &acknowledgement, 1)!= 1){
                perror("Failed to notify the parent");
                break;
            }
       

        }
        close(pipefd[0]);
        close(sec_pipefd[1]);
        close(output_fd);
        munmap(shared_memory, buffer_size);
        _exit(0);
        }
        else {
        printf("Parent process running\n");
        close(pipefd[0]);
        close(sec_pipefd[1]);
        close(output_fd);
        // Variables for data transfer and synchronization
        ssize_t bytes_read;
        char acknowledgement;
        while((bytes_read = read(input_fd,  shared_memory,
             buffer_size))>0){
             if(write(pipefd[1], &bytes_read, sizeof(bytes_read))
            != sizeof(bytes_read)){
                perror("failed to notify the child");
                break;
            }
            if(read(sec_pipefd[0], &acknowledgement, 1)!= 1){
                perror("Failed to recieve acknowledgment\n");
                break;
            }
        }
        if(bytes_read==-1){
            perror("Failed to read input file");
        
        }
        close(pipefd[1]);
        close(sec_pipefd[0]);
        close(input_fd);
        waitpid(pid, NULL, 0);
        munmap(shared_memory, buffer_size);
    }
    return 0;
}
