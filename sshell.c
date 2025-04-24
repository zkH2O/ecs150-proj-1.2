#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>

#define MAX_CMD_LEN 512
#define MAX_ARGS 16
#define MAX_TOKEN_LEN 32
#define MAX_COMMANDS 4
#define PROMPT "sshell@ucd$ "
#define EXIT_CMD "exit"
#define CD_CMD "cd"
#define PWD_CMD "pwd"

//make data tings
typedef struct {
    char *args[MAX_ARGS + 1];
    char *inFile;
    char *outFile;
    int argc;
} SimpleCommand;

typedef struct {
    SimpleCommand commands[MAX_COMMANDS];
    int numCommands;
    int background;
    char originalLine[MAX_CMD_LEN + 1];
} Job;

//keep track of background job, only allow one
Job *backgroundJob = NULL;
pid_t bgPids[MAX_COMMANDS];
int bgNumPids = 0;

//func dec
Job* parse_command(char *cmdLine);
void execute_job(Job *job);
void cleanup_job(Job *job);
void check_background_completion();

//self explanatory
void print_completion(const char *cmdLine, int *statuses, int numStatuses) {
    fprintf(stderr, "+ completed '%s'", cmdLine);
    for (int i = 0; i < numStatuses; ++i) {
        fprintf(stderr, " [%d]", statuses[i]);
    }
    fprintf(stderr, "\n");
}

// mem cean up
void cleanup_job(Job *job) {
    if (!job) return;
    for (int i = 0; i < job->numCommands; ++i) {
        for (int j = 0; j < job->commands[i].argc; ++j) {
            free(job->commands[i].args[j]);
        }
        free(job->commands[i].inFile);
        free(job->commands[i].outFile);
    }
    free(job);
}

// run built in cmmds
int execute_builtin(SimpleCommand *cmd, Job *job, int *statusCode) {
    if (strcmp(cmd->args[0], PWD_CMD) == 0) {
        if (cmd->argc > 1) {
             fprintf(stderr, "Error: %s takes no arguments\n", PWD_CMD);
             *statusCode = 1;
             return 1;
        }
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
            *statusCode = 0;
        } else {
            perror("pwd failed");
            *statusCode = 1;
        }
        return 1;
    } else if (strcmp(cmd->args[0], CD_CMD) == 0) {
         if (cmd->argc != 2) {
            fprintf(stderr, "Error: %s requires exactly one argument\n", CD_CMD);
            *statusCode = 1;
            return 1;
        }
        if (chdir(cmd->args[1]) != 0) {
            fprintf(stderr, "Error: cannot cd into directory\n");
            *statusCode = 1;
        } else {
            *statusCode = 0;
        }
        return 1;
    } else if (strcmp(cmd->args[0], EXIT_CMD) == 0) {
        check_background_completion();
        if (backgroundJob) {
            fprintf(stderr, "Error: active job still running\n");
            *statusCode = 1;
            return 1;
        } else {
            fprintf(stdout, "Bye...\n");
            *statusCode = 0;
            print_completion(EXIT_CMD, statusCode, 1);
            cleanup_job(job);
            exit(EXIT_SUCCESS);
        }
    }
    return 0;
}

// run job with pipe
void execute_job(Job *job) {
    if (!job || job->numCommands == 0 || job->commands[0].argc == 0) {
        return;
    }

    if (job->numCommands == 1) {
        int statusCode;
        if (execute_builtin(&job->commands[0], job, &statusCode)) {
             print_completion(job->originalLine, &statusCode, 1);
             return;
        }
    }

     if (job->numCommands > 1 || job->background) {
         for(int i=0; i < job->numCommands; ++i) {
             char* cmdName = job->commands[i].args[0];
             if (strcmp(cmdName, EXIT_CMD) == 0 || strcmp(cmdName, CD_CMD) == 0 || strcmp(cmdName, PWD_CMD) == 0) {
                 fprintf(stderr, "Error: built-in command '%s' cannot be part of a pipeline or run in background\n", cmdName);
                 int failStatus[] = {1};
                 print_completion(job->originalLine, failStatus, 1);
                 return;
             }
         }
     }

    int numPipes = job->numCommands - 1;
    int pipeFds[MAX_COMMANDS - 1][2];
    pid_t pids[MAX_COMMANDS];
    int statuses[MAX_COMMANDS];

    for (int i = 0; i < numPipes; ++i) {
        if (pipe(pipeFds[i]) == -1) {
            perror("pipe failed");
            return;
        }
    }

    for (int i = 0; i < job->numCommands; ++i) {
        pids[i] = fork();
        if (pids[i] == -1) {
            perror("fork failed");
            job->numCommands = i;
            break;
        } else if (pids[i] == 0) {
            int fdIn = -1, fdOut = -1;

            if (i == 0 && job->commands[i].inFile) {
                fdIn = open(job->commands[i].inFile, O_RDONLY);
                if (fdIn == -1) {
                    fprintf(stderr, "Error: cannot open input file\n");
                    _exit(1);
                }
                if (dup2(fdIn, STDIN_FILENO) == -1) {
                    perror("dup2 stdin failed");
                    _exit(1);
                }
                close(fdIn);
            }

            if (i == job->numCommands - 1 && job->commands[i].outFile) {
                fdOut = open(job->commands[i].outFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fdOut == -1) {
                    fprintf(stderr, "Error: cannot open output file\n");
                    _exit(1);
                }
                if (dup2(fdOut, STDOUT_FILENO) == -1) {
                    perror("dup2 stdout failed");
                    _exit(1);
                }
                close(fdOut);
            }

            if (i > 0) {
                if (dup2(pipeFds[i-1][0], STDIN_FILENO) == -1) {
                    perror("dup2 pipe stdin failed");
                    _exit(1);
                }
            }
            if (i < numPipes) {
                if (dup2(pipeFds[i][1], STDOUT_FILENO) == -1) {
                    perror("dup2 pipe stdout failed");
                    _exit(1);
                }
            }

            for (int j = 0; j < numPipes; ++j) {
                close(pipeFds[j][0]);
                close(pipeFds[j][1]);
            }

            execvp(job->commands[i].args[0], job->commands[i].args);
            fprintf(stderr, "Error: command not found\n");
            _exit(1);
        }
    }

    for (int i = 0; i < numPipes; ++i) {
        close(pipeFds[i][0]);
        close(pipeFds[i][1]);
    }
    if (job->background) {
        if (backgroundJob) {
             fprintf(stderr, "Error: another background job is already running\n");
              for (int i = 0; i < job->numCommands; ++i) {
                if (pids[i] > 0) kill(pids[i], SIGTERM);
              }
              int failStatus[] = {1};
              print_completion(job->originalLine, failStatus, 1);
              cleanup_job(job);
        } else {
            backgroundJob = job;
            bgNumPids = job->numCommands;
            for(int i=0; i< bgNumPids; ++i) {
                 bgPids[i] = pids[i];
            }
        }
    } else {
        for (int i = 0; i < job->numCommands; ++i) {
            int status;
             if (pids[i] > 0) {
                 waitpid(pids[i], &status, 0);
                 if (WIFEXITED(status)) {
                    statuses[i] = WEXITSTATUS(status);
                 } else {
                    statuses[i] = 1;
                 }
             } else {
                 statuses[i] = 1;
             }
        }
        print_completion(job->originalLine, statuses, job->numCommands);
    }
}

// check if background job done
void check_background_completion() {
    if (!backgroundJob) {
        return;
    }

    int completedCount = 0;
    int currentStatuses[MAX_COMMANDS];
    int allDone = 1;

    for (int i = 0; i < bgNumPids; ++i) {
         if (bgPids[i] <= 0) {
             completedCount++;
             currentStatuses[i] = 1;
             continue;
         }

        int status;
        pid_t result = waitpid(bgPids[i], &status, WNOHANG);

        if (result == -1) {
            perror("waitpid background error");
            currentStatuses[i] = 1;
            bgPids[i] = 0;
            completedCount++;
        } else if (result > 0) {
            if (WIFEXITED(status)) {
                currentStatuses[i] = WEXITSTATUS(status);
            } else {
                currentStatuses[i] = 1;
            }
            bgPids[i] = 0;
            completedCount++;
        } else {
             allDone = 0;
        }
    }

    if (allDone && completedCount > 0) {
         char bgCmdLine[MAX_CMD_LEN + 2];
         snprintf(bgCmdLine, sizeof(bgCmdLine), "%s&", backgroundJob->originalLine);
         print_completion(bgCmdLine, currentStatuses, bgNumPids);
         cleanup_job(backgroundJob);
         backgroundJob = NULL;
         bgNumPids = 0;
    }
}

// function to break up cmd line
Job* parse_command(char *cmdLine) {
    if (strlen(cmdLine) == 0 || strspn(cmdLine, " \t") == strlen(cmdLine)) {
        return NULL;
    }

    Job *job = calloc(1, sizeof(Job));
    if (!job) {
        perror("malloc failed");
        return NULL;
    }
    strncpy(job->originalLine, cmdLine, MAX_CMD_LEN);
    job->originalLine[MAX_CMD_LEN] = '\0';
    job->numCommands = 1;
    job->background = 0;

    int currentCmdIdx = 0;
    SimpleCommand *currentCmd = &job->commands[currentCmdIdx];
    currentCmd->argc = 0;
    currentCmd->inFile = NULL;
    currentCmd->outFile = NULL;

    char *lineCopy = strdup(cmdLine);
    if (!lineCopy) {
        perror("strdup failed");
        cleanup_job(job);
        return NULL;
    }
    char *token;
    char *rest = lineCopy;
    int argCountTotal = 0;

    enum State { TOKEN, IN_REDIR, OUT_REDIR };
    enum State state = TOKEN;

    while ((token = strtok_r(rest, " \t", &rest))) {

        if (strlen(token) > MAX_TOKEN_LEN) {
             fprintf(stderr, "Error: token too long near '%s'\n", token);
             free(lineCopy);
             cleanup_job(job);
             return NULL;
        }

        if (state == IN_REDIR) {
            if (strchr("|<>", token[0])) {
                fprintf(stderr, "Error: expected input filename after '<'\n");
                free(lineCopy);
                cleanup_job(job);
                return NULL;
            }
            currentCmd->inFile = strdup(token);
            state = TOKEN;
            continue;
        }
        if (state == OUT_REDIR) {
            if (strchr("|<>", token[0])) {
                fprintf(stderr, "Error: expected output filename after '>'\n");
                free(lineCopy);
                cleanup_job(job);
                return NULL;
            }
            currentCmd->outFile = strdup(token);
            state = TOKEN;
            continue;
        }

        if (strcmp(token, "|") == 0) {
            if (currentCmd->argc == 0) {
                fprintf(stderr, "Error: missing command\n");
                free(lineCopy);
                cleanup_job(job);
                return NULL;
            }
            currentCmd->args[currentCmd->argc] = NULL;
            currentCmdIdx++;
            if (currentCmdIdx >= MAX_COMMANDS) {
                fprintf(stderr, "Error: too many commands in pipeline (max %d)\n", MAX_COMMANDS);
                free(lineCopy);
                cleanup_job(job);
                return NULL;
            }
            job->numCommands++;
            currentCmd = &job->commands[currentCmdIdx];
            currentCmd->argc = 0;
            currentCmd->inFile = NULL;
            currentCmd->outFile = NULL;
            argCountTotal = 0;
        } else if (strcmp(token, ">") == 0) {
            if (currentCmd->outFile) {
                fprintf(stderr, "Error: multiple output redirects for one command\n");
                free(lineCopy);
                cleanup_job(job);
                return NULL;
            }
             if (currentCmd->argc == 0 && currentCmdIdx == 0 && job->numCommands == 1) {
                  fprintf(stderr, "Error: missing command\n");
                  free(lineCopy);
                  cleanup_job(job);
                  return NULL;
            }
            state = OUT_REDIR;
        } else if (strcmp(token, "<") == 0) {
             if (currentCmd->inFile) {
                fprintf(stderr, "Error: multiple input redirects for one command\n");
                free(lineCopy);
                cleanup_job(job);
                return NULL;
            }
             if (currentCmd->argc == 0 && currentCmdIdx == 0 && job->numCommands == 1) {
                fprintf(stderr, "Error: missing command\n");
                free(lineCopy);
                cleanup_job(job);
                return NULL;
            }
            state = IN_REDIR;
        } else if (strcmp(token, "&") == 0) {
            job->background = 1;
        } else {
            if (argCountTotal >= MAX_ARGS) {
                fprintf(stderr, "Error: too many process arguments\n");
                free(lineCopy);
                cleanup_job(job);
                return NULL;
            }
            currentCmd->args[currentCmd->argc++] = strdup(token);
            argCountTotal++;
        }
    }

    currentCmd->args[currentCmd->argc] = NULL;

    if (state == IN_REDIR) {
        fprintf(stderr, "Error: no input file\n");
        free(lineCopy);
        cleanup_job(job);
        return NULL;
    }
    if (state == OUT_REDIR) {
        fprintf(stderr, "Error: no output file\n");
        free(lineCopy);
        cleanup_job(job);
        return NULL;
    }
    if (job->numCommands > 1 && job->commands[job->numCommands - 1].argc == 0) {
         fprintf(stderr, "Error: missing command\n");
         free(lineCopy);
         cleanup_job(job);
         return NULL;
    }
    if (job->background && strcmp(cmdLine + strlen(cmdLine) - 1, "&") != 0) {
        char *lastChar = cmdLine + strlen(cmdLine) - 1;
        while (lastChar > cmdLine && isspace(*lastChar)) {
            lastChar--;
        }
        if (*lastChar != '&') {
             fprintf(stderr, "Error: mislocated background sign\n");
             free(lineCopy);
             cleanup_job(job);
             return NULL;
        }
         for(int i=0; i < job->numCommands; ++i) {
            if (job->commands[i].argc > 0 && strcmp(job->commands[i].args[job->commands[i].argc - 1], "&") == 0) {
                 free(job->commands[i].args[job->commands[i].argc - 1]);
                 job->commands[i].args[job->commands[i].argc - 1] = NULL;
                 job->commands[i].argc--;
                 break;
            }
        }
    }
    for (int i = 0; i < job->numCommands; ++i) {
        if (i > 0 && job->commands[i].inFile) {
            fprintf(stderr, "Error: mislocated input redirection\n");
            free(lineCopy);
            cleanup_job(job);
            return NULL;
        }
        if (i < job->numCommands - 1 && job->commands[i].outFile) {
            fprintf(stderr, "Error: mislocated output redirection\n");
            free(lineCopy);
            cleanup_job(job);
            return NULL;
        }
        if (job->commands[i].argc == 0 && !job->commands[i].inFile && !job->commands[i].outFile) {
             fprintf(stderr, "Error: missing command\n");
             free(lineCopy);
             cleanup_job(job);
             return NULL;
        }
    }

    free(lineCopy);

    if (job->commands[0].argc == 0 && !job->commands[0].inFile && !job->commands[0].outFile) {
        fprintf(stderr, "Error: missing command\n");
        cleanup_job(job);
        return NULL;
    }

    return job;
}

int main() {
    char cmdLine[MAX_CMD_LEN + 1];

    while (1) {
        check_background_completion();

        fprintf(stdout, "%s", PROMPT);
        fflush(stdout);

        if (fgets(cmdLine, sizeof(cmdLine), stdin) == NULL) {
            if (feof(stdin)) {
                fprintf(stdout, "\nBye...\n");
                break;
            } else {
                perror("fgets failed");
                continue;
            }
        }

        cmdLine[strcspn(cmdLine, "\n")] = 0;

        Job *job = parse_command(cmdLine);

        if (job) {
            execute_job(job);

            if (!job->background || backgroundJob == NULL) {
                 cleanup_job(job);
            }
        }
    }

    if (backgroundJob) {
         cleanup_job(backgroundJob);
    }

    return EXIT_SUCCESS;
}