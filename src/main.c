#include "shell.h"

int main() {
    init_readline();
    char* cmdline;
    char** tokens;

    while ((cmdline = readline(PROMPT)) != NULL) {
        // !n must be expanded BEFORE tokenizing and before storing in history
        if (cmdline[0] == '!') {
            char* expanded = hist_expand(cmdline);
            free(cmdline);
            if (expanded == NULL)
                continue;
            cmdline = expanded;
            printf("%s\n", cmdline);           // show what is being re-run
        }

        // store non-blank lines (our own list for `history`/!n, and Readline's for the arrow keys)
        for (char* p = cmdline; *p; p++)
            if (*p != ' ' && *p != '\t') { hist_add(cmdline); add_history(cmdline); break; }

        if ((tokens = tokenize(cmdline)) != NULL) {
            pipeline_t pl;
            if (parse_pipeline(tokens, &pl) == 0) {
                execute_pipeline(&pl);       // built-in, single command or pipeline
                free_pipeline(&pl);
            }
            free_tokens(tokens);
        }
        free(cmdline);
    }

    printf("\nShell exited.\n");
    return 0;
}