#ifndef SHELL_H
#define SHELL_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <dirent.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAX_LEN 512
#define MAXARGS 10
#define ARGLEN 30
#define HISTORY_SIZE 20
#define MAX_CMDS 16          // max commands in one pipeline
#define PROMPT "FCIT> "

// Function prototypes
void init_readline(void);   // installs our tab-completion function

// One simple command of a pipeline:  argv... [< infile] [> outfile | >> outfile]
typedef struct {
    char** argv;      // NULL-terminated argument vector (points into the token strings)
    char*  infile;    // NULL if no '<'
    char*  outfile;   // NULL if no '>' / '>>'
    int    append;    // 1 for '>>'
} command_t;

// cmd1 | cmd2 | cmd3 ...
typedef struct {
    command_t cmds[MAX_CMDS];
    int ncmds;
} pipeline_t;

char** tokenize(const char* cmdline);                  // quote-aware; <, >, >>, | are separate tokens
void   free_tokens(char** tokens);
int    parse_pipeline(char** tokens, pipeline_t* pl);  // 0 ok, -1 syntax error
void   free_pipeline(pipeline_t* pl);
int    execute_pipeline(pipeline_t* pl);               // returns exit status of last command
int handle_builtin(char** arglist);
int is_builtin(const char* name);

// History (stores the last HISTORY_SIZE commands)
void  hist_add(const char* cmdline);
void  hist_print(void);
char* hist_expand(const char* cmdline);   // handles !n and !!; NULL on error   // returns 1 if arglist was a built-in, else 0

#endif // SHELL_H
