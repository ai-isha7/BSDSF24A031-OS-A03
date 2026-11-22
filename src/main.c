#include "shell.h"

int main() {
    char* cmdline;
    char** arglist;

    while ((cmdline = read_cmd(PROMPT, stdin)) != NULL) {
        // !n must be expanded BEFORE tokenizing and before storing in history
        if (cmdline[0] == '!') {
            char* expanded = hist_expand(cmdline);
            free(cmdline);
            if (expanded == NULL)
                continue;
            cmdline = expanded;
            printf("%s\n", cmdline);           // show what is being re-run
        }

        // store non-blank lines
        for (char* p = cmdline; *p; p++)
            if (*p != ' ' && *p != '\t') { hist_add(cmdline); break; }

        if ((arglist = tokenize(cmdline)) != NULL) {
            // Built-ins run in the shell itself; everything else is fork-exec-wait.
            if (!handle_builtin(arglist))
                execute(arglist);

            // Free the memory allocated by tokenize()
            for (int i = 0; arglist[i] != NULL; i++) {
                free(arglist[i]);
            }
            free(arglist);
        }
        free(cmdline);
    }

    printf("\nShell exited.\n");
    return 0;
}