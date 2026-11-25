# CSC33200_OS_Group_Project

# Group 3 - Custom Shell

## CSC 33200 - Operating Systems | Fall 2026

## Developed By

- Nurul Huda (Group Leader)
- Tausif Ahmed
- Abid Khan
- Farhan Chowdhury
- Mobin Ahmed

## Project Overview

A custom shell written in C integrating five commands: `sharedmempipe`, `filecrypt`, `loganalyzer`, `processgroup`, and `timedexec`. The project demonstrates shared memory, pipes, process management, file operations, and resource limitations.

## Part 1: Linux / WSL Terminal

Requirements: Linux or WSL, GCC, and standard C/POSIX libraries.

### Step 1: Exit the custom shell if it is already running

```bash
exit
```

On Windows, run `wsl` if not already in Linux. Adjust the directory path to match your repository location.

### Step 2: Navigate to the downloaded or cloned repository

```bash
wsl
cd CSC33200_OS_Group_Project
ls
```

### Step 3: Compile all six C files.

```bash
gcc -Wall -Wextra shell.c sharedmempipe.c filecrypt.c loganalyzer.c processgroup.c timedexec.c -o customshell
```

### Step 4: Prepare sharedmempipe input file.

```bash
echo "Group 3 - Testing shared memory and pipes" > input.txt
```

### Step 5: Prepare filecrypt input file.

```bash
echo "This is a confidential message from Group 3." > notes.txt
```

### Step 6: Prepare loganalyzer input files.

```bash
printf '%s\n' \
"2026-11-01 10:00:01 INFO Server started on port 8080" \
"2026-11-01 10:00:05 DEBUG Loading config file /etc/app.conf" \
"2026-11-01 10:01:12 INFO User alice logged in" \
"2026-11-01 10:02:30 WARN Disk usage at 85%" \
"2026-11-01 10:03:44 ERROR Database connection timeout" \
"" \
"2026-11-01 10:04:10 INFO Retrying database connection" \
"2026-11-01 10:04:15 ERROR Database connection timeout" \
"2026-11-01 10:05:00 INFO User bob logged in" \
"2026-11-01 10:06:21 WARN Slow response: 2300 ms" \
"2026-11-01 10:07:02 DEBUG Cache hit ratio 0.92" \
"2026-11-01 10:08:59 INFO User alice logged out" > sample.log

truncate -s 0 empty.log
```

### Step 7: Prepare timedexec testing programs.

```bash
mkdir -p tests
gcc cpu_hog.c -o tests/cpu_hog
gcc mem_hog.c -o tests/mem_hog
ls -l tests
```

### Step 8: Verify testing files.

```bash
ls
cat input.txt
cat notes.txt
cat sample.log
```

### Step 9: Launch the Custom Shell.

```bash
./customshell
```

## Part 2: Inside the Custom Shell

Run each command individually. Do not exit between tests.

### Step 10: Display available commands.

```bash
help
```

### Step 11: Test sharedmempipe.

```bash
sharedmempipe -h
sharedmempipe input.txt output.txt
sharedmempipe -b 8 input.txt output.txt
sharedmempipe -b 0 input.txt output.txt
```

**Purpose:**
- Display help menu.
- Transfer data using default 4096-byte buffer.
- Transfer data using custom 8-byte buffer.
- Demonstrate invalid buffer error handling.

Expected output for the zero-byte buffer test:

```text
Invalid buffer size
Command failed with exit status 1
```

### Step 12: Test filecrypt.

```bash
filecrypt -e -v -k demo123 notes.txt
filecrypt -d -k demo123 -o back.txt notes.txt.enc
filecrypt -d -k wrong -o bad.txt notes.txt.enc
filecrypt -e -o c.enc notes.txt
```

**Purpose:**
- Encrypt a file.
- Decrypt using the correct key.
- Test an incorrect key.
- Test missing key argument.

### Step 13: Test processgroup.

```bash
processgroup -n 3 -t 5
processgroup -n 5 -t 3
processgroup -n 0 -t 5
processgroup -h
```

**Purpose:**
- Create three processes for five seconds.
- Create five processes for three seconds.
- Test invalid process count.
- Display help menu.

### Step 14: Test timedexec.

```bash
timedexec -v -c 2 ./tests/cpu_hog
timedexec -m 50 ./tests/mem_hog
timedexec -t 1 sleep 10
timedexec -c 5 -m 100 ls
```

**Purpose:**
- Test CPU time limitation.
- Test memory limitation.
- Test execution time limitation.
- Test normal program execution.

### Step 15: Test loganalyzer.

```bash
loganalyzer sample.log
loganalyzer -t sample.log
loganalyzer -k timeout sample.log
loganalyzer -t -k alice sample.log
loganalyzer -h
loganalyzer empty.log
loganalyzer missing.log
loganalyzer -z sample.log
loganalyzer
```

**Purpose:**
- Analyze a log file.
- Test timestamp-related option.
- Filter by keyword.
- Combine command options.
- Display help menu.
- Test empty file.
- Test missing file.
- Test invalid option.
- Test missing argument.

### Step 16: Exit Custom Shell.

```bash
exit
```

## Part 3: Verify Results in the Linux / WSL Terminal

### Step 17: Verify sharedmempipe results.

```bash
cat input.txt
cat output.txt
cmp input.txt output.txt && echo "Transfer successful"
```

For a fresh transfer test, use a previously unused output filename and compare that file instead.

### Step 18: Verify filecrypt results.

```bash
cat notes.txt
cat back.txt
cmp notes.txt back.txt && echo "Decryption successful"
```

## Custom Shell Implementation

The main shell uses `getline()` to read input, `strtok()` to separate arguments, `strcmp()` to identify commands, `fork()` to create child processes, `waitpid()` to wait for completion, `signal()` to handle Ctrl+C, `_exit()` to terminate child processes, and `free()` to release allocated input memory.

## Important Reminders

1. Compile everything inside WSL.
2. Prepare all testing files BEFORE launching Custom Shell.
3. Do not run echo, cat, gcc, printf, or cmp inside Custom Shell.
4. Do not exit between custom command tests.
5. Some tests intentionally produce errors.
6. Wait for CustomShell> before running the next command.
7. Verify output files after exiting Custom Shell.
8. Complete a practice run before the live presentation.

## Conclusion

This project provides hands-on experience with process creation, inter-process communication, shared memory, pipes, file operations, resource management, and error handling.
