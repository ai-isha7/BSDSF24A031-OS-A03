#include "shell.h"

char* read_cmd(char* prompt, FILE* fp) {
    printf("%s", prompt);
    char* cmdline = (char*) malloc(sizeof(char) * MAX_LEN);
    int c, pos = 0;

    while ((c = getc(fp)) != EOF) {
        if (c == '\n') break;
        cmdline[pos++] = c;
    }

    if (c == EOF && pos == 0) {
        free(cmdline);
        return NULL; // Handle Ctrl+D
    }
    
    cmdline[pos] = '\0';
    return cmdline;
}

char** tokenize(char* cmdline) {
    // Edge case: empty command line
    if (cmdline == NULL || cmdline[0] == '\0' || cmdline[0] == '\n') {
        return NULL;
    }

    char** arglist = (char**)malloc(sizeof(char*) * (MAXARGS + 1));
    for (int i = 0; i < MAXARGS + 1; i++) {
        arglist[i] = (char*)malloc(sizeof(char) * ARGLEN);
        bzero(arglist[i], ARGLEN);
    }

    char* cp = cmdline;
    char* start;
    int len;
    int argnum = 0;

    while (*cp != '\0' && argnum < MAXARGS) {
        while (*cp == ' ' || *cp == '\t') cp++; // Skip leading whitespace
        
        if (*cp == '\0') break; // Line was only whitespace

        start = cp;
        len = 1;
        while (*++cp != '\0' && !(*cp == ' ' || *cp == '\t')) {
            len++;
        }
        strncpy(arglist[argnum], start, len);
        arglist[argnum][len] = '\0';
        argnum++;
    }

    if (argnum == 0) { // No arguments were parsed
        for(int i = 0; i < MAXARGS + 1; i++) free(arglist[i]);
        free(arglist);
        return NULL;
    }

    arglist[argnum] = NULL;
    return arglist;
}

/* ---------------------------------------------------------------------
 * Built-in commands.
 * These MUST run inside the shell process itself (not in a forked child),
 * because they change the shell's own state (cwd, lifetime, ...).
 * Returns 1 if arglist[0] was a built-in (and was executed), 0 otherwise.
 * ------------------------------------------------------------------- */
int handle_builtin(char** arglist) {
    if (arglist == NULL || arglist[0] == NULL)
        return 0;

    if (strcmp(arglist[0], "exit") == 0) {
        printf("Exiting shell.\n");
        exit(0);
    }

    if (strcmp(arglist[0], "cd") == 0) {
        const char* dir = arglist[1];
        if (dir == NULL || dir[0] == '\0')   // plain "cd" goes HOME, like bash
            dir = getenv("HOME");
        if (dir == NULL)
            fprintf(stderr, "cd: HOME not set\n");
        else if (chdir(dir) == -1)
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }

    if (strcmp(arglist[0], "help") == 0) {
        printf("Built-in commands:\n");
        printf("  exit        terminate the shell\n");
        printf("  cd <dir>    change the current working directory\n");
        printf("  help        show this help message\n");
        printf("  jobs        list background jobs (not yet implemented)\n");
        printf("  history     show the numbered list of previous commands\n");
        printf("  !n          re-run command number n from history (!! = last)\n");
        return 1;
    }

    if (strcmp(arglist[0], "history") == 0) {
        hist_print();
        return 1;
    }

    if (strcmp(arglist[0], "jobs") == 0) {
        printf("Job control not yet implemented.\n");
        return 1;
    }

    return 0;
}


/* ---------------------------------------------------------------------
 * Command history: a simple array of strings.
 * When full, the oldest entry is dropped and the rest shift down, so the
 * numbers shown by `history` always run from 1 to hist_count.
 * ------------------------------------------------------------------- */
static char* history[HISTORY_SIZE];
static int   hist_count = 0;

void hist_add(const char* cmdline) {
    if (hist_count == HISTORY_SIZE) {          // full: overwrite the oldest
        free(history[0]);
        memmove(&history[0], &history[1], sizeof(char*) * (HISTORY_SIZE - 1));
        hist_count--;
    }
    history[hist_count++] = strdup(cmdline);
}

void hist_print(void) {
    for (int i = 0; i < hist_count; i++)
        printf("%4d  %s\n", i + 1, history[i]);
}

/* cmdline starts with '!'. Returns a malloc'd replacement command line,
 * or NULL (after printing an error) if the event does not exist. */
char* hist_expand(const char* cmdline) {
    int n;
    if (cmdline[1] == '!' && cmdline[2] == '\0')
        n = hist_count;                         // !!  -> last command
    else {
        char* end;
        long v = strtol(cmdline + 1, &end, 10);
        if (end == cmdline + 1 || *end != '\0') {
            fprintf(stderr, "%s: event not found\n", cmdline);
            return NULL;
        }
        n = (int) v;
    }
    if (n < 1 || n > hist_count) {
        fprintf(stderr, "%s: event not found (history has %d entries)\n", cmdline, hist_count);
        return NULL;
    }
    return strdup(history[n - 1]);
}
