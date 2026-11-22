#include "shell.h"

int main() {
    init_readline();
    char* cmdline;

    for (;;) {
        reap_jobs();                         // collect finished background children
        if ((cmdline = readline(PROMPT)) == NULL)
            break;                           // Ctrl+D

        // !n must be expanded BEFORE tokenizing and before storing in history
        if (cmdline[0] == '!') {
            char* expanded = hist_expand(cmdline);
            free(cmdline);
            if (expanded == NULL)
                continue;
            cmdline = expanded;
            printf("%s\n", cmdline);         // show what is being re-run
        }

        // store non-blank lines (our own list for `history`/!n, and Readline's for the arrow keys)
        for (char* p = cmdline; *p; p++)
            if (*p != ' ' && *p != '\t') { hist_add(cmdline); add_history(cmdline); break; }

        // 1. split at ';' and '&'   2. run each piece in order
        stmtlist_t list;
        stmtlist_init(&list);
        int ok = (split_line(cmdline, &list) == 0);

        // An unfinished "if" -> keep reading continuation lines until the matching "fi"
        while (ok && if_depth(&list) > 0) {
            char* more = readline("> ");
            if (more == NULL) {
                fprintf(stderr, "syntax error: unexpected end of input (missing 'fi')\n");
                ok = 0;
                break;
            }
            if (more[0] != '\0') { hist_add(more); add_history(more); }
            ok = (split_line(more, &list) == 0);
            free(more);
        }

        if (ok)
            run_block(&list, 0, list.count);
        stmtlist_free(&list);
        free(cmdline);
    }

    printf("\nShell exited.\n");
    return 0;
}
