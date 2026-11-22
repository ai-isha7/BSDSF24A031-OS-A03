# myshell

A UNIX shell written in C for the Operating Systems course (PA-03).

## Build & run
```bash
make
./bin/myshell
```
`make clean` removes `bin/` and `obj/`.

## Layout
- `src/main.c`    – main read-eval loop
- `src/shell.c`   – input, parsing, built-ins, history, jobs, variables
- `src/execute.c` – fork / exec / wait, redirection, pipes
- `include/shell.h`
