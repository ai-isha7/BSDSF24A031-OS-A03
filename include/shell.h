#ifndef SHELL_H
#define SHELL_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_LEN 512
#define MAXARGS 10
#define ARGLEN 30
#define HISTORY_SIZE 20
#define PROMPT "FCIT> "

// Function prototypes
char* read_cmd(char* prompt, FILE* fp);
char** tokenize(char* cmdline);
int execute(char** arglist);
int handle_builtin(char** arglist);

// History (stores the last HISTORY_SIZE commands)
void  hist_add(const char* cmdline);
void  hist_print(void);
char* hist_expand(const char* cmdline);   // handles !n and !!; NULL on error   // returns 1 if arglist was a built-in, else 0

#endif // SHELL_H
