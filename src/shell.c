#include "shell.h"

/* ---------------------------------------------------------------------
 * GNU Readline: line editing, arrow-key history and TAB completion.
 * readline() replaces our old read_cmd(): it prints the prompt, lets the
 * user edit the line and returns a malloc'd string (NULL on Ctrl+D).
 * ------------------------------------------------------------------- */
static const char* builtin_names[] = { "exit", "cd", "help", "jobs", "history", "set", "export", "unset", NULL };

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
int last_status = 0;

int handle_builtin(char** arglist) {
    if (arglist == NULL || arglist[0] == NULL)
        return 0;
    last_status = 0;                    // built-ins report success unless they say otherwise

    if (strcmp(arglist[0], "exit") == 0) {
        printf("Exiting shell.\n");
        exit(0);
    }

    if (strcmp(arglist[0], "cd") == 0) {
        const char* dir = arglist[1];
        if (dir == NULL || dir[0] == '\0')   // plain "cd" goes HOME, like bash
            dir = getenv("HOME");
        if (dir == NULL) {
            fprintf(stderr, "cd: HOME not set\n");
            last_status = 1;
        } else if (chdir(dir) == -1) {
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
            last_status = 1;
        }
        return 1;
    }

    if (strcmp(arglist[0], "help") == 0) {
        printf("Built-in commands:\n");
        printf("  exit        terminate the shell\n");
        printf("  cd <dir>    change the current working directory\n");
        printf("  help        show this help message\n");
        printf("  jobs        list running background jobs\n");
        printf("  history     show the numbered list of previous commands\n");
        printf("  set         print all shell variables\n");
        printf("  export V    export variable V (or V=value) to child processes\n");
        printf("  unset V     remove variable V\n");
        printf("  if/then/else/fi   conditional execution based on a command's exit status\n");
        printf("  !n          re-run command number n from history (!! = last)\n");
        return 1;
    }

    if (strcmp(arglist[0], "history") == 0) {
        hist_print();
        return 1;
    }

    if (strcmp(arglist[0], "set") == 0) {
        var_print();
        return 1;
    }

    if (strcmp(arglist[0], "unset") == 0) {
        for (int i = 1; arglist[i]; i++) { var_unset(arglist[i]); unsetenv(arglist[i]); }
        return 1;
    }

    if (strcmp(arglist[0], "export") == 0) {      // export NAME  /  export NAME=value
        if (arglist[1] == NULL) { var_print(); return 1; }
        for (int i = 1; arglist[i]; i++) {
            char* name = strdup(arglist[i]);
            char* eq = strchr(name, '=');
            if (eq) { *eq = '\0'; var_set(name, eq + 1); }
            const char* val = var_get(name);
            if (val) setenv(name, val, 1);        // now visible to child processes
            free(name);
        }
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

    // Peel off leading keywords so "then echo hi" becomes the statements: then | echo hi
    static const char* kw[] = { "if", "then", "else", "fi", NULL };
    char* t = tmp;
    for (int found = 1; found; ) {
        found = 0;
        while (isspace((unsigned char) *t)) t++;
        for (int k = 0; kw[k]; k++) {
            size_t kl = strlen(kw[k]);
            if (strncmp(t, kw[k], kl) == 0 && (t[kl] == '\0' || isspace((unsigned char) t[kl]))) {
                stmtlist_add(out, kw[k], 0);
                t += kl;
                found = 1;
                break;
            }
        }
    }
    if (*t) stmtlist_add(out, t, bg);
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

/* ---------------------------------------------------------------------
 * Shell variables: a singly linked list of (name, value) pairs.
 * ------------------------------------------------------------------- */
typedef struct var {
    char* name;
    char* value;
    struct var* next;
} var_t;

static var_t* vars_head = NULL;

void var_set(const char* name, const char* value) {
    for (var_t* v = vars_head; v; v = v->next)
        if (strcmp(v->name, name) == 0) {          // re-assignment: replace the value
            free(v->value);
            v->value = strdup(value);
            return;
        }
    var_t* v = malloc(sizeof(var_t));
    v->name = strdup(name);
    v->value = strdup(value);
    v->next = vars_head;
    vars_head = v;
}

const char* var_get(const char* name) {
    for (var_t* v = vars_head; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v->value;
    return NULL;
}

void var_unset(const char* name) {
    for (var_t** pp = &vars_head; *pp; pp = &(*pp)->next)
        if (strcmp((*pp)->name, name) == 0) {
            var_t* v = *pp;
            *pp = v->next;
            free(v->name); free(v->value); free(v);
            return;
        }
}

void var_print(void) {
    for (var_t* v = vars_head; v; v = v->next)
        printf("%s=%s\n", v->name, v->value);
}

/* Is t of the form NAME=value, with NAME = [A-Za-z_][A-Za-z0-9_]* ? */
static int is_assignment(const char* t) {
    if (!(isalpha((unsigned char) t[0]) || t[0] == '_')) return 0;
    int i = 1;
    while (isalnum((unsigned char) t[i]) || t[i] == '_') i++;
    return t[i] == '=';
}

static int all_assignments(char** tokens) {
    for (int i = 0; tokens[i]; i++)
        if (!is_assignment(tokens[i])) return 0;
    return tokens[0] != NULL;
}

/* Returns a NEW malloc'd string in which $NAME, ${NAME}, $? and $$ in t are
 * replaced by their values (unknown names expand to ""). Lookup order:
 * shell variables, then the environment. */
static char* expand_token(const char* t) {
    size_t cap = strlen(t) + 64, len = 0;
    char* out = malloc(cap);

    #define APPEND(str) do { const char* _s = (str); size_t _l = strlen(_s); \
        while (len + _l + 1 > cap) { cap *= 2; out = realloc(out, cap); } \
        memcpy(out + len, _s, _l); len += _l; } while (0)

    for (const char* p = t; *p; ) {
        if (*p != '$') { char c[2] = { *p++, '\0' }; APPEND(c); continue; }

        p++;                                              // skip '$'
        char name[256];
        int n = 0;
        if (*p == '?')      { char b[16]; snprintf(b, sizeof b, "%d", last_status); APPEND(b); p++; }
        else if (*p == '$') { char b[16]; snprintf(b, sizeof b, "%d", (int) getpid()); APPEND(b); p++; }
        else if (*p == '{') {
            const char* close = strchr(p, '}');
            if (!close || close - p - 1 >= (int) sizeof name) { APPEND("$"); continue; }
            n = close - p - 1;
            memcpy(name, p + 1, n); name[n] = '\0';
            p = close + 1;
            const char* v = var_get(name); if (!v) v = getenv(name);
            if (v) APPEND(v);
        } else if (isalpha((unsigned char) *p) || *p == '_') {
            while ((isalnum((unsigned char) *p) || *p == '_') && n < (int) sizeof name - 1) name[n++] = *p++;
            name[n] = '\0';
            const char* v = var_get(name); if (!v) v = getenv(name);
            if (v) APPEND(v);
        } else {
            APPEND("$");                                  // lone '$' stays literal
        }
    }
    out[len] = '\0';
    #undef APPEND
    return out;
}

/* Replaces every token that contains '$' by its expansion. A token such as
 * $UNSET that expands to nothing is dropped, like in bash. */
static void expand_tokens(char** tokens) {
    int w = 0;
    for (int r = 0; tokens[r]; r++) {
        if (strchr(tokens[r], '$') == NULL) { tokens[w++] = tokens[r]; continue; }
        char* e = expand_token(tokens[r]);
        int drop = (e[0] == '\0' && tokens[r][0] == '$');
        free(tokens[r]);
        if (drop) free(e);
        else tokens[w++] = e;
    }
    tokens[w] = NULL;
}

int run_statement(const char* text, int background) {
    char** tokens = tokenize(text);
    if (tokens == NULL) return 0;

    expand_tokens(tokens);                       // $VAR substitution (after tokenizing, before executing)
    if (tokens[0] == NULL) {                     // e.g. "$EMPTY" expanded to nothing
        free(tokens);
        return last_status = 0;
    }
    if (all_assignments(tokens)) {               // NAME=value is handled by the shell itself: no fork()
        for (int i = 0; tokens[i]; i++) {
            char* eq = strchr(tokens[i], '=');
            *eq = '\0';
            var_set(tokens[i], eq + 1);
        }
        free_tokens(tokens);
        return last_status = 0;
    }

    int status = 2;
    pipeline_t pl;
    if (parse_pipeline(tokens, &pl) == 0) {
        pl.background = background;
        status = execute_pipeline(&pl, text);
        free_pipeline(&pl);
    }
    free_tokens(tokens);
    last_status = status;
    return status;
}

/* ---------------------------------------------------------------------
 * if / then / else / fi
 * A multi-line block has been flattened into a list of statements where
 * the keywords are statements of their own:
 *        if | grep root /etc/passwd | then | echo yes | else | echo no | fi
 * run_block() walks such a list; for an "if" it finds the matching
 * then / else / fi (counting nesting depth), runs the condition, and picks
 * the branch according to the condition's exit status (0 = success).
 * ------------------------------------------------------------------- */
static int is_kw(const stmtlist_t* l, int i, const char* kw) {
    return strcmp(l->items[i].text, kw) == 0;
}

int if_depth(const stmtlist_t* l) {
    int d = 0;
    for (int i = 0; i < l->count; i++) {
        if (is_kw(l, i, "if")) d++;
        else if (is_kw(l, i, "fi")) d--;
    }
    return d;
}

int run_block(const stmtlist_t* l, int lo, int hi) {
    int i = lo;
    while (i < hi) {
        const char* t = l->items[i].text;

        if (strcmp(t, "if") == 0) {
            int depth = 0, then_i = -1, else_i = -1, fi_i = -1;
            for (int j = i; j < hi; j++) {
                if (is_kw(l, j, "if")) depth++;
                else if (is_kw(l, j, "fi")) { if (--depth == 0) { fi_i = j; break; } }
                else if (depth == 1 && is_kw(l, j, "then") && then_i < 0) then_i = j;
                else if (depth == 1 && is_kw(l, j, "else") && else_i < 0 && then_i >= 0) else_i = j;
            }
            if (fi_i < 0 || then_i < 0 || then_i == i + 1) {
                fprintf(stderr, "syntax error: malformed if statement\n");
                return last_status = 2;
            }

            run_block(l, i + 1, then_i);                    // the condition
            int ok = (last_status == 0);
            if (ok)
                run_block(l, then_i + 1, else_i >= 0 ? else_i : fi_i);
            else if (else_i >= 0)
                run_block(l, else_i + 1, fi_i);
            else
                last_status = 0;                            // false condition, no else: status 0
            i = fi_i + 1;
        } else if (!strcmp(t, "then") || !strcmp(t, "else") || !strcmp(t, "fi")) {
            fprintf(stderr, "syntax error near unexpected token '%s'\n", t);
            return last_status = 2;
        } else {
            run_statement(t, l->items[i].background);
            i++;
        }
    }
    return last_status;
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
