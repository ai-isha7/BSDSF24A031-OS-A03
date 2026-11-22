#include "shell.h"

/* ---------------------------------------------------------------------
 * GNU Readline: line editing, arrow-key history and TAB completion.
 * readline() replaces our old read_cmd(): it prints the prompt, lets the
 * user edit the line and returns a malloc'd string (NULL on Ctrl+D).
 * ------------------------------------------------------------------- */
static const char* builtin_names[] = { "exit", "cd", "help", "jobs", "history", NULL };

int is_builtin(const char* name) {
    for (int i = 0; builtin_names[i]; i++)
        if (strcmp(name, builtin_names[i]) == 0) return 1;
    return 0;
}

/* Generator called repeatedly by readline: yields one matching command
 * per call (built-ins first, then every file found in the $PATH dirs). */
static char* command_generator(const char* text, int state) {
    static int bi;
    static char* pathbuf;
    static char* saveptr;
    static DIR* dir;
    static int started;
    static size_t len;
    struct dirent* de;

    if (!state) {                       // first call for this completion
        bi = 0; started = 0; len = strlen(text);
        if (dir) { closedir(dir); dir = NULL; }
        free(pathbuf);
        const char* p = getenv("PATH");
        pathbuf = strdup(p ? p : "");
    }
    while (builtin_names[bi]) {
        const char* n = builtin_names[bi++];
        if (strncmp(n, text, len) == 0) return strdup(n);
    }
    for (;;) {
        if (!dir) {
            char* d = strtok_r(started ? NULL : pathbuf, ":", &saveptr);
            started = 1;
            if (!d) { free(pathbuf); pathbuf = NULL; return NULL; }
            dir = opendir(d);
            if (!dir) continue;
        }
        while ((de = readdir(dir)) != NULL)
            if (de->d_name[0] != '.' && strncmp(de->d_name, text, len) == 0)
                return strdup(de->d_name);
        closedir(dir); dir = NULL;
    }
}

/* Called by readline when TAB is pressed. If the word being completed is
 * in command position (start of line, or after | ; &) complete commands.
 * Returning NULL makes readline fall back to filename completion. */
static char** shell_completion(const char* text, int start, int end) {
    (void) end;
    int i = start - 1;
    while (i >= 0 && (rl_line_buffer[i] == ' ' || rl_line_buffer[i] == '\t')) i--;
    if (i < 0 || rl_line_buffer[i] == '|' || rl_line_buffer[i] == ';' || rl_line_buffer[i] == '&')
        return rl_completion_matches(text, command_generator);
    return NULL;
}

void init_readline(void) {
    rl_attempted_completion_function = shell_completion;
}

static int is_op_char(char c) { return c == '<' || c == '>' || c == '|'; }

/* Splits the line into tokens. Words are separated by blanks; text inside
 * "..." or '...' is kept together (quotes removed). The operators  <  >  >>  |
 * become tokens of their own even without surrounding spaces. */
char** tokenize(const char* cmdline) {
    size_t cap = 16, n = 0;
    char** toks = malloc(cap * sizeof(char*));
    const char* p = cmdline;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char* buf = malloc(strlen(p) + 3);
        size_t len = 0;

        if (is_op_char(*p)) {
            buf[len++] = *p++;
            if (buf[0] == '>' && *p == '>') buf[len++] = *p++;     // ">>"
        } else {
            char quote = 0;
            while (*p && (quote || !(*p == ' ' || *p == '\t' || is_op_char(*p)))) {
                if (quote) {
                    if (*p == quote) quote = 0; else buf[len++] = *p;
                    p++;
                } else if (*p == '"' || *p == '\'') {
                    quote = *p++;
                } else {
                    buf[len++] = *p++;
                }
            }
            if (quote) {
                fprintf(stderr, "syntax error: unterminated quote\n");
                free(buf);
                toks[n] = NULL;
                free_tokens(toks);
                return NULL;
            }
        }
        buf[len] = '\0';

        if (n + 2 > cap) { cap *= 2; toks = realloc(toks, cap * sizeof(char*)); }
        toks[n++] = buf;
    }

    if (n == 0) { free(toks); return NULL; }
    toks[n] = NULL;
    return toks;
}

void free_tokens(char** tokens) {
    if (!tokens) return;
    for (int i = 0; tokens[i]; i++) free(tokens[i]);
    free(tokens);
}

static int is_operator(const char* t) {
    return !strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, ">>") || !strcmp(t, "|");
}

/* Turns the flat token list into a pipeline of commands. */
int parse_pipeline(char** tokens, pipeline_t* pl) {
    int ntok = 0;
    while (tokens[ntok]) ntok++;

    memset(pl, 0, sizeof(*pl));
    pl->ncmds = 1;
    command_t* cur = &pl->cmds[0];
    cur->argv = calloc(ntok + 1, sizeof(char*));
    int argc = 0;

    for (int i = 0; tokens[i]; i++) {
        const char* t = tokens[i];
        if (strcmp(t, "|") == 0) {
            if (argc == 0 || pl->ncmds == MAX_CMDS) goto syntax_error;
            cur = &pl->cmds[pl->ncmds++];
            cur->argv = calloc(ntok + 1, sizeof(char*));
            argc = 0;
        } else if (!strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, ">>")) {
            if (tokens[i + 1] == NULL || is_operator(tokens[i + 1])) goto syntax_error;
            if (t[0] == '<') cur->infile = tokens[++i];
            else { cur->append = (t[1] == '>'); cur->outfile = tokens[++i]; }
        } else {
            cur->argv[argc++] = tokens[i];
        }
    }
    if (argc == 0) goto syntax_error;
    return 0;

syntax_error:
    fprintf(stderr, "syntax error in command line\n");
    free_pipeline(pl);
    return -1;
}

void free_pipeline(pipeline_t* pl) {
    for (int i = 0; i < pl->ncmds; i++) { free(pl->cmds[i].argv); pl->cmds[i].argv = NULL; }
    pl->ncmds = 0;
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
        printf("  jobs        list running background jobs\n");
        printf("  history     show the numbered list of previous commands\n");
        printf("  !n          re-run command number n from history (!! = last)\n");
        return 1;
    }

    if (strcmp(arglist[0], "history") == 0) {
        hist_print();
        return 1;
    }

    if (strcmp(arglist[0], "jobs") == 0) {
        print_jobs();
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


/* ---------------------------------------------------------------------
 * Statement list + splitting at ';' and '&'
 * ------------------------------------------------------------------- */
void stmtlist_init(stmtlist_t* l) { l->items = NULL; l->count = l->cap = 0; }

void stmtlist_add(stmtlist_t* l, const char* text, int background) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, l->cap * sizeof(stmt_t));
    }
    l->items[l->count].text = strdup(text);
    l->items[l->count].background = background;
    l->count++;
}

void stmtlist_free(stmtlist_t* l) {
    for (int i = 0; i < l->count; i++) free(l->items[i].text);
    free(l->items);
    stmtlist_init(l);
}

static void flush_segment(stmtlist_t* out, const char* start, size_t len, int bg) {
    while (len && isspace((unsigned char) *start)) { start++; len--; }
    while (len && isspace((unsigned char) start[len - 1])) len--;
    if (len == 0) return;
    char* tmp = strndup(start, len);
    stmtlist_add(out, tmp, bg);
    free(tmp);
}

/* "a ; b & c"  ->  [a, fg] [b, bg] [c, fg]   (separators inside quotes are ignored) */
int split_line(const char* line, stmtlist_t* out) {
    const char* seg = line;
    char quote = 0;
    for (const char* p = line; *p; p++) {
        if (quote) { if (*p == quote) quote = 0; continue; }
        if (*p == '"' || *p == '\'') { quote = *p; continue; }
        if (*p == ';') {
            flush_segment(out, seg, p - seg, 0);
            seg = p + 1;
        } else if (*p == '&') {
            if (p[1] == '&') { fprintf(stderr, "'&&' is not supported\n"); return -1; }
            flush_segment(out, seg, p - seg, 1);
            seg = p + 1;
        }
    }
    flush_segment(out, seg, strlen(seg), 0);
    return 0;
}

int run_statement(const char* text, int background) {
    char** tokens = tokenize(text);
    if (tokens == NULL) return 0;

    int status = 2;
    pipeline_t pl;
    if (parse_pipeline(tokens, &pl) == 0) {
        pl.background = background;
        status = execute_pipeline(&pl, text);
        free_pipeline(&pl);
    }
    free_tokens(tokens);
    return status;
}

/* ---------------------------------------------------------------------
 * Background jobs: a linked list of (job id, pid, command text).
 * ------------------------------------------------------------------- */
typedef struct job {
    int id;
    pid_t pid;
    char* cmd;
    struct job* next;
} job_t;

static job_t* jobs_head = NULL;
static int next_job_id = 1;

void job_add(pid_t pid, const char* cmd) {
    job_t* j = malloc(sizeof(job_t));
    j->id = next_job_id++;
    j->pid = pid;
    j->cmd = strdup(cmd);
    j->next = NULL;
    job_t** pp = &jobs_head;                 // append at the tail
    while (*pp) pp = &(*pp)->next;
    *pp = j;
    printf("[%d] %d\n", j->id, (int) pid);
}

/* Called before every prompt. waitpid(-1, WNOHANG) returns immediately: it
 * collects (reaps) any finished child, so no zombies stay in the process table. */
void reap_jobs(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (job_t** pp = &jobs_head; *pp; pp = &(*pp)->next) {
            if ((*pp)->pid == pid) {
                job_t* j = *pp;
                printf("[%d]+  Done    %s\n", j->id, j->cmd);
                *pp = j->next;
                free(j->cmd);
                free(j);
                break;
            }
        }
    }
    if (jobs_head == NULL) next_job_id = 1;
}

void print_jobs(void) {
    if (jobs_head == NULL) { printf("No background jobs.\n"); return; }
    for (job_t* j = jobs_head; j; j = j->next)
        printf("[%d]  Running  pid=%d  %s &\n", j->id, (int) j->pid, j->cmd);
}
