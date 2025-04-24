#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define CMDLINE_MAX 512
#define ARG_MAX 16
#define PIPE_MAX 4

pid_t background_pid = -1;
char background_cmdline[CMDLINE_MAX];

int execute_pipeline(char *cmdline_copy, char *tokens[], int token_count, int background);
int is_builtin_command(char *cmd);
int execute_builtin_command(char *args[], int argc, char *cmdline_copy);

int main(void) {
    char cmdline[CMDLINE_MAX];
    char cmdline_copy[CMDLINE_MAX];
    char *args[ARG_MAX + 1];
    int status;
    pid_t pid;

    while (1) {
        if (background_pid > 0) {
            if (waitpid(background_pid, &status, WNOHANG) > 0) {
                fprintf(stderr, "+ completed '%s' [%d]\n", background_cmdline, WEXITSTATUS(status));
                background_pid = -1;
            }
        }

        printf("sshell@ucd$ ");
        fflush(stdout);

        if (fgets(cmdline, CMDLINE_MAX, stdin) == NULL) {
            break;
        }

        if (!isatty(STDIN_FILENO)) {
            printf("%s", cmdline);
            fflush(stdout);
        }

        size_t len = strlen(cmdline);
        if (len > 0 && cmdline[len - 1] == '\n') {
            cmdline[len - 1] = '\0';
        }

        strncpy(cmdline_copy, cmdline, CMDLINE_MAX);
        cmdline_copy[CMDLINE_MAX - 1] = '\0';
        
        if (strlen(cmdline) == 0) {
            continue;
        }

        int background = 0;

        char *tokens[CMDLINE_MAX];
        int token_count = 0;
        char parse_cmdline[CMDLINE_MAX];
        strncpy(parse_cmdline, cmdline, CMDLINE_MAX);
        parse_cmdline[CMDLINE_MAX - 1] = '\0';
        
        char *token = strtok(parse_cmdline, " \t");
        while (token != NULL && token_count < CMDLINE_MAX - 1) {
            tokens[token_count++] = token;
            token = strtok(NULL, " \t");
        }
        tokens[token_count] = NULL;

        if (token_count == 0) {
            continue;
        }
        
        if (token_count > 0) {
            char *last_token = tokens[token_count - 1];
            size_t last_token_len = strlen(last_token);

            if (strcmp(last_token, "&") == 0) {
                background = 1;
                tokens[token_count - 1] = NULL;
                token_count--;

                if (token_count == 0) {
                    fprintf(stderr, "Error: missing command\n");
                    continue;
                }
            } else if (last_token_len > 1 && last_token[last_token_len - 1] == '&') {
                background = 1;
                last_token[last_token_len - 1] = '\0';
                
                if (strlen(last_token) == 0) {
                    fprintf(stderr, "Error: missing command\n");
                    continue;
                }
            }
        }

        for (int i = 0; i < token_count; i++) {
             if (strcmp(tokens[i], "&") == 0) {
                  fprintf(stderr, "Error: mislocated background sign\n");
                  goto next_command;
             }
        }

        int has_pipe = 0;
        int has_redirect = 0;
        for (int i = 0; i < token_count; i++) {
            if (strcmp(tokens[i], "|") == 0) {
                 has_pipe = 1;
            } else if (strcmp(tokens[i], ">") == 0 || strcmp(tokens[i], "<") == 0) {
                 has_redirect = 1;
            }
        }

        if (background && !has_pipe && !has_redirect && is_builtin_command(tokens[0])) {
             fprintf(stderr, "Error: builtin cannot be backgrounded\n");
             continue;
        }

        if (!has_pipe && !has_redirect && strcmp(tokens[0], "exit") == 0) {
            if (background_pid > 0) {
                fprintf(stderr, "Error: active job still running\n");
                fprintf(stderr, "* completed 'exit' [1]\n");
                continue;
            }
            fprintf(stderr, "Bye...\n");
            fprintf(stderr, "+ completed 'exit' [0]\n");
            break;
        }

        if (!has_pipe && !has_redirect && !background && is_builtin_command(tokens[0])) {
            execute_builtin_command(tokens, token_count, cmdline_copy);
            continue;
        }
        
        if (has_pipe || has_redirect || background) {
            int result = execute_pipeline(cmdline_copy, tokens, token_count, background); 
            if (result < 0) {
                continue;
            }
        } else {
            int argc = 0;
            args[0] = tokens[0];
            
            while (tokens[argc] != NULL && argc < ARG_MAX) {
                args[argc] = tokens[argc];
                argc++;
            }
            
            if (argc >= ARG_MAX && args[argc] != NULL) {
                fprintf(stderr, "Error: too many process arguments\n");
                continue;
            }
            
            args[argc] = NULL;

            pid = fork();
            if (pid < 0) {
                perror("fork");
                exit(EXIT_FAILURE);
            } else if (pid == 0) {
                execvp(args[0], args);
                fprintf(stderr, "Error: command not found\n");
                exit(1);
            } else {
                waitpid(pid, &status, 0);
                fprintf(stderr, "+ completed '%s' [%d]\n", cmdline_copy, WEXITSTATUS(status));
            }
        }

next_command:;
    }

    return 0;
}

int is_builtin_command(char *cmd) {
    return (strcmp(cmd, "cd") == 0 || strcmp(cmd, "pwd") == 0);
}

int execute_builtin_command(char *args[], int argc, char *cmdline_copy) {
    if (strcmp(args[0], "pwd") == 0) {
        char cwd[CMDLINE_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
            fprintf(stderr, "+ completed '%s' [0]\n", cmdline_copy);
            return 0;
        } else {
            perror("getcwd");
            fprintf(stderr, "+ completed '%s' [1]\n", cmdline_copy);
            return 1;
        }
    } else if (strcmp(args[0], "cd") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: cd requires exactly one argument\n");
            fprintf(stderr, "+ completed '%s' [1]\n", cmdline_copy);
            return 1;
        }
        
        if (chdir(args[1]) != 0) {
            fprintf(stderr, "Error: cannot cd into directory\n");
            fprintf(stderr, "+ completed '%s' [1]\n", cmdline_copy);
            return 1;
        }
        
        fprintf(stderr, "+ completed '%s' [0]\n", cmdline_copy);
        return 0;
    }
    
    return -1;
}

int execute_pipeline(char *cmdline_copy, char *tokens[], int token_count, int background) {
    int cmd_start[PIPE_MAX];
    int cmd_end[PIPE_MAX];
    int cmd_count = 0;
    char *input_file = NULL;
    char *output_file = NULL;
    
    cmd_start[0] = 0;
    int last_pipe_idx = -1;
    
    for (int i = 0; i < token_count; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            if (i == cmd_start[cmd_count]) {
                fprintf(stderr, "Error: missing command\n");
                return -1;
            }
            
            cmd_end[cmd_count] = i;
            cmd_count++;
            last_pipe_idx = i;
            
            if (cmd_count >= PIPE_MAX) {
                fprintf(stderr, "Error: too many commands in pipeline\n");
                return -1;
            }
            
            cmd_start[cmd_count] = i + 1;
        } else if (strcmp(tokens[i], ">") == 0) {
            if (i == cmd_start[cmd_count]) {
                fprintf(stderr, "Error: missing command\n");
                return -1;
            }
            
            if (last_pipe_idx != -1 && i < last_pipe_idx) {
                fprintf(stderr, "Error: mislocated output redirection\n");
                return -1;
            }

            if (cmd_count > 0 && i < cmd_start[cmd_count]) {
                fprintf(stderr, "Error: mislocated output redirection\n");
                return -1;
            }
            
            if (output_file != NULL) {
                fprintf(stderr, "Error: multiple output redirections\n");
                return -1;
            }
            
            if (i + 1 >= token_count || tokens[i+1] == NULL || strcmp(tokens[i+1], "|") == 0 || strcmp(tokens[i+1], "<") == 0 || strcmp(tokens[i+1], ">") == 0) {
                fprintf(stderr, "Error: no output file\n");
                return -1;
            }
            
            output_file = tokens[i + 1];
            tokens[i] = NULL;
        } else if (strcmp(tokens[i], "<") == 0) {
            if (i == cmd_start[cmd_count]) {
                fprintf(stderr, "Error: missing command\n");
                return -1;
            }
            
            if (cmd_count > 0) {
                fprintf(stderr, "Error: mislocated input redirection\n");
                return -1;
            }
            
            if (input_file != NULL) {
                fprintf(stderr, "Error: multiple input redirections\n");
                return -1;
            }
            
            if (i + 1 >= token_count || tokens[i+1] == NULL || strcmp(tokens[i+1], "|") == 0 || strcmp(tokens[i+1], "<") == 0 || strcmp(tokens[i+1], ">") == 0) {
                fprintf(stderr, "Error: no input file\n");
                return -1;
            }
            
            input_file = tokens[i + 1];
            tokens[i] = NULL;
        } else if ( (input_file != NULL && tokens[i] == input_file) || (output_file != NULL && tokens[i] == output_file) ){
             tokens[i] = NULL;
        }
    }
    
    int last_cmd_empty = 1;
    for (int j = cmd_start[cmd_count]; j < token_count && tokens[j] != NULL; j++) {
        last_cmd_empty = 0;
        break;
    }
     if (last_cmd_empty && cmd_start[cmd_count] >= token_count) {
         if (cmd_start[cmd_count] > 0 || (input_file != NULL || output_file != NULL)) { 
             fprintf(stderr, "Error: missing command\n");
             return -1;
         }
     }
     else if (last_cmd_empty && tokens[cmd_start[cmd_count]] == NULL) {
          fprintf(stderr, "Error: missing command\n");
          return -1;
     }
    
    cmd_end[cmd_count] = token_count;
    cmd_count++;
    
    int pipes[PIPE_MAX - 1][2];
    for (int i = 0; i < cmd_count - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            exit(EXIT_FAILURE);
        }
    }
    
    pid_t pids[PIPE_MAX];
    
    for (int i = 0; i < cmd_count; i++) {
        char *cmd_args[ARG_MAX + 1];
        int arg_idx = 0;
        
        for (int j = cmd_start[i]; j < cmd_end[i]; j++) {
            if (tokens[j] != NULL && arg_idx < ARG_MAX) {
                 cmd_args[arg_idx++] = tokens[j];
            } else if (tokens[j] != NULL && arg_idx >= ARG_MAX) {
                 fprintf(stderr, "Error: too many process arguments\n");
                 return -1; 
            }
        }
        
        if (arg_idx == 0) {
             fprintf(stderr, "Error: missing command\n");
             return -1;
        }
        
        if (arg_idx >= ARG_MAX && cmd_end[i] < token_count && tokens[cmd_end[i]] != NULL && strcmp(tokens[cmd_end[i]],"|") != 0) {
             fprintf(stderr, "Error: too many process arguments\n");
             return -1;
        }
        
        cmd_args[arg_idx] = NULL;
        
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        } else if (pids[i] == 0) {
            
            if (i == 0 && input_file != NULL) {
                int fd = open(input_file, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "Error: cannot open input file\n");
                    exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            
            if (i == cmd_count - 1 && output_file != NULL) {
                int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    fprintf(stderr, "Error: cannot open output file\n");
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }
            
            if (i < cmd_count - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            
            for (int j = 0; j < cmd_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            
            execvp(cmd_args[0], cmd_args);
            fprintf(stderr, "Error: command not found\n");
            exit(1);
        }
    }
    
    for (int i = 0; i < cmd_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    
    if (background) {
        background_pid = pids[cmd_count - 1];
        strncpy(background_cmdline, cmdline_copy, CMDLINE_MAX);
        background_cmdline[CMDLINE_MAX-1] = '\0';
        return 0;
    }
    
    int exit_status[PIPE_MAX];
    for (int i = 0; i < cmd_count; i++) {
        waitpid(pids[i], &exit_status[i], 0);
    }
    
    fprintf(stderr, "+ completed '%s' ", cmdline_copy);
    for (int i = 0; i < cmd_count; i++) {
        fprintf(stderr, "[%d]", WEXITSTATUS(exit_status[i]));
    }
    fprintf(stderr, "\n");
    
    return 0;
}
