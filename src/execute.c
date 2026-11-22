#include "shell.h"

/* Wire a command's own < and > files onto fd 0 / fd 1.
 * Runs in the CHILD (before exec) or temporarily in the shell for built-ins. */
static int apply_redirections(command_t* c) {
    if (c->infile) {
        int fd = open(c->infile, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", c->infile, strerror(errno)); return -1; }
        dup2(fd, STDIN_FILENO);          // fd 0 now refers to the file
        close(fd);                       // original descriptor no longer needed
    }
    if (c->outfile) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->outfile, flags, 0644);
        if (fd < 0) { fprintf(stderr, "%s: %s\n", c->outfile, strerror(errno)); return -1; }
        dup2(fd, STDOUT_FILENO);         // fd 1 now refers to the file
        close(fd);
    }
    return 0;
}

/* A single built-in runs in the shell process, so save fds 0/1, apply the
 * redirections, run it, then restore. */
static int run_builtin_redirected(command_t* c) {
    fflush(stdout);
    int saved_in = dup(STDIN_FILENO), saved_out = dup(STDOUT_FILENO);
    if (apply_redirections(c) == 0)
        handle_builtin(c->argv);
    fflush(stdout);
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);
    return 0;
}

/* Runs cmd1 | cmd2 | ... | cmdN  (N may be 1).
 * N-1 pipes are created in the parent, then one child is forked per command.
 * Every process closes every pipe end it does not use, otherwise a reader
 * would never see EOF and the pipeline would hang. */
int execute_pipeline(pipeline_t* pl) {
    int n = pl->ncmds;

    if (n == 1 && is_builtin(pl->cmds[0].argv[0]))
        return run_builtin_redirected(&pl->cmds[0]);

    int pipes[MAX_CMDS][2];
    pid_t pids[MAX_CMDS];

    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            return 1;
        }
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == -1) { perror("fork failed"); exit(1); }

        if (pid == 0) {                                   // ---- child ----
            if (i > 0)     dup2(pipes[i - 1][0], STDIN_FILENO);   // read end of previous pipe
            if (i < n - 1) dup2(pipes[i][1],     STDOUT_FILENO);  // write end of next pipe
            for (int j = 0; j < n - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }

            if (apply_redirections(&pl->cmds[i]) == -1) exit(1);  // < and > override the pipe

            if (is_builtin(pl->cmds[i].argv[0])) {        // e.g.  history | grep ls
                handle_builtin(pl->cmds[i].argv);
                fflush(stdout);
                exit(0);
            }
            execvp(pl->cmds[i].argv[0], pl->cmds[i].argv);
            fprintf(stderr, "%s: command not found\n", pl->cmds[i].argv[0]);
            exit(127);
        }
        pids[i] = pid;                                    // ---- parent ----
    }

    for (int j = 0; j < n - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); }

    int status = 0;
    for (int i = 0; i < n; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        if (i == n - 1) status = st;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
