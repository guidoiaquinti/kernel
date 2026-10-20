/* KoraOS shell: a tiny interactive command line running in EL0. It reads a
 * line, splits it into argv, and spawns the named program with those arguments
 * (so, e.g., "echo hello world" runs the echo program with two args). The
 * kernel starts this as the first user program.
 */
#include "libk/koraos.h"

#define MAX_ARGV 16
#define LINE_MAX (2 * KORA_PATH_MAX + 64)

// Qualified paths and enumeration records live in BSS, keeping the one-page
// EL0 stack available for call frames and argv.
static char line[LINE_MAX];
static char cwd_buffer[KORA_PATH_MAX];
static struct assign_info assign_buffer;

// Read a whole command before dispatch. A full buffer without a newline is
// drained and rejected so its prefix cannot become a separate command.
static int read_command_line(void) {
    size_t used = 0;
    while (used < sizeof(line) - 1) {
        long n = read(0, line + used, sizeof(line) - 1 - used);
        if (n <= 0) {
            return 0;
        }
        used += (size_t)n;
        if (line[used - 1] == '\n') {
            line[used] = 0;
            return 1;
        }
    }
    char discard[64];
    for (;;) {
        long n = read(0, discard, sizeof(discard));
        if (n <= 0 || discard[n - 1] == '\n') {
            break;
        }
    }
    kputs("shell: line too long\n");
    return 0;
}

// Whether each token was quoted or escaped: such a token is never an operator
// (see take_background).
static unsigned char literal_tokens[MAX_ARGV];

static int whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Compact tokens in place. Quotes concatenate with adjacent text; empty quotes
// produce an empty argument. No expansion or command substitution is performed.
static int tokenize(char *text, char **argv) {
    char *source = text, *destination = text;
    int argc = 0;
    for (;;) {
        while (whitespace(*source)) source++;
        if (!*source) return argc;
        if (argc == MAX_ARGV) return -1;
        argv[argc] = destination;
        literal_tokens[argc] = 0;
        char quote = 0;
        while (*source) {
            char c = *source++;
            if (quote) {
                if (c == '\n' || c == '\r') return -2;
                if (c == quote) {
                    quote = 0;
                    continue;
                }
                if (c == '\\' && quote == '"') {
                    if (!*source || *source == '\n' || *source == '\r') return -2;
                    c = *source++;
                }
                *destination++ = c;
                continue;
            }
            if (whitespace(c)) break;
            if (c == '\'' || c == '"') {
                quote = c;
                literal_tokens[argc] = 1;
            } else if (c == '\\') {
                if (!*source || *source == '\n' || *source == '\r') return -2;
                literal_tokens[argc] = 1;
                *destination++ = *source++;
            } else {
                *destination++ = c;
            }
        }
        if (quote) return -2;
        *destination++ = 0;
        argc++;
    }
}

static void help(void) {
    kputs("KoraOS shell commands:\n");
    kputs("  help            show this text\n");
    kputs("  exit            leave the shell\n");
    kputs("  cd [path]       change directory, or print cwd\n");
    kputs("  pwd             print current directory\n");
    kputs("  volumes         list devices and volume labels\n");
    kputs("  name:           change to a device, volume or assign\n");
    kputs("  assign          list assigns\n");
    kputs("  assign name target  assign an existing directory\n");
    kputs("  assign name     remove an assign (sys: is fixed)\n");
    kputs("  ls [path]       list a directory (default current)\n");
    kputs("  cat <path>...   print file contents\n");
    kputs("  cp/rm/mkdir/rmdir/mv  manage files (cp and mv preserve existing targets)\n");
    kputs("  sync            flush writable volumes\n");
    kputs("  quote paths containing spaces; backslash escapes the next character\n");
    kputs("  <program> [args...]  run a program from c:\n");
    kputs("  <program> [args...] &  run it in the background\n");
    kputs("  <program> [args...] @N  run it on core N (with & too: ... @N &)\n");
    kputs("  ps              list tasks and the core each runs on\n");
    kputs("  kill <pid>      end a program (a background job)\n");
    kputs("programs in /bin: hello, echo, gfxdemo, termdemo, ls, cat (try 'ls /bin')\n");
}

static void print_cwd(void) {
    if (getcwd(cwd_buffer, sizeof(cwd_buffer)) < 0) {
        kputs("pwd: cannot read current directory\n");
        return;
    }
    kputs(cwd_buffer);
    kputs("\n");
}

static void list_volumes(void) {
    struct volume_info info;
    int rc;
    for (unsigned i = 0; (rc = volume_info(i, &info)) == 1; i++) {
        kputs(info.device);
        kputs(": ");
        kputs(info.label[0] ? info.label : "(unlabelled)");
        if (info.flags & VOLUME_BOOT) {
            kputs(" [boot]");
        }
        if (info.flags & VOLUME_READ_ONLY) {
            kputs(" [read-only]");
        }
        kputs("\n");
    }
    if (rc < 0) {
        kputs("volumes: enumeration failed\n");
    }
}

static void assign_command(int argc, char **argv) {
    if (argc > 3) {
        kputs("usage: assign [name [target]]\n");
        return;
    }
    if (argc > 1) {
        if (assign(argv[1], argc == 3 ? argv[2] : (const char *)0) < 0) {
            kputs("assign: cannot update ");
            kputs(argv[1]);
            kputs("\n");
        }
        return;
    }
    int rc;
    for (unsigned i = 0; (rc = assign_info(i, &assign_buffer)) == 1; i++) {
        kputs(assign_buffer.name);
        kputs(": = ");
        kputs(assign_buffer.target);
        if (assign_buffer.flags & ASSIGN_IMMUTABLE) {
            kputs(" [fixed]");
        }
        kputs("\n");
    }
    if (rc < 0) {
        kputs("assign: enumeration failed\n");
    }
}

// kill <pid>: end a program. Its exit (EXIT_KILLED) is reported as a job's at
// the next prompt, once its threads are gone.
static void kill_command(int argc, char **argv) {
    int pid = 0;
    const char *p = argc == 2 ? argv[1] : "";
    for (; *p >= '0' && *p <= '9' && pid < 100000000; p++) {
        pid = pid * 10 + (*p - '0');
    }
    if (argc != 2 || *p != '\0' || pid <= 0) {
        kputs("usage: kill <pid>\n");
    } else if (kill(pid) < 0) {
        kputs("kill: no such program: ");
        kputs(argv[1]);
        kputs("\n");
    }
}

static void print_job_end(int pid, const char *what, int code) {
    kputs("[");
    kput_int(pid);
    kputs(what);
    kput_int(code);
    kputs("\n");
}

// Report background jobs that finished since the last prompt (reaping them).
static void reap_jobs(void) {
    int code;
    int pid;
    while ((pid = waitpid(-1, &code, WNOHANG)) > 0) {
        print_job_end(pid, "] done, exit ", code);
    }
}

// A trailing "&" (its own word, or the end of the last one) asks for a
// background job; strip it and say so. A quoted or escaped token is literal:
// `echo '&'` and `echo a\&` print an ampersand.
static int take_background(int *argc, char **argv) {
    char *last = argv[*argc - 1];
    size_t len = kstrlen(last);
    if (literal_tokens[*argc - 1] || len == 0 || last[len - 1] != '&') {
        return 0;
    }
    if (len == 1) {
        (*argc)--;
    } else {
        last[len - 1] = '\0';
    }
    return 1;
}

// A trailing "@N" word (after any "&" is taken off) pins the program to core
// N: returns N, -1 if there is none, or -2 if the word is malformed. A quoted
// or escaped "@2" is an ordinary argument.
#define MAX_CORE_DIGITS 3

static int take_core(int *argc, char **argv) {
    if (*argc < 2) {
        return -1;  // "@2" alone would be the program name
    }
    char *last = argv[*argc - 1];
    if (literal_tokens[*argc - 1] || last[0] != '@') {
        return -1;
    }
    int core = 0;
    unsigned digits = 0;
    for (const char *p = last + 1; *p; p++, digits++) {
        if (*p < '0' || *p > '9' || digits == MAX_CORE_DIGITS) {
            return -2;
        }
        core = core * 10 + (*p - '0');
    }
    if (digits == 0) {
        return -2;
    }
    (*argc)--;
    return core;
}

int main(void) {
    kputs("\nKoraOS shell. Type 'help'.\n");

    char *argv[MAX_ARGV];

    for (;;) {
        reap_jobs();
        kputs("$ ");
        if (!read_command_line()) {
            continue;
        }

        int argc = tokenize(line, argv);
        if (argc < 0) {
            kputs(argc == -1 ? "shell: too many arguments\n" : "shell: invalid quoting\n");
            continue;
        }
        if (argc == 0) {
            continue;
        }

        if (kstreq(argv[0], "exit")) {
            kputs("bye\n");
            return 0;
        }
        if (kstreq(argv[0], "help")) {
            help();
            continue;
        }

        if (kstreq(argv[0], "pwd")) {
            if (argc == 1) {
                print_cwd();
            } else {
                kputs("usage: pwd\n");
            }
            continue;
        }
        if (kstreq(argv[0], "cd")) {
            if (argc == 1) {
                print_cwd();
            } else if (argc == 2) {
                if (chdir(argv[1]) < 0) {
                    kputs("cd: cannot change directory to ");
                    kputs(argv[1]);
                    kputs("\n");
                }
            } else {
                kputs("usage: cd [path]\n");
            }
            continue;
        }
        if (kstreq(argv[0], "volumes")) {
            if (argc == 1) {
                list_volumes();
            } else {
                kputs("usage: volumes\n");
            }
            continue;
        }
        if (kstreq(argv[0], "sync")) {
            if (argc != 1) {
                kputs("usage: sync\n");
            } else if (sync() < 0) {
                kputs("sync: failed\n");
            }
            continue;
        }
        if (kstreq(argv[0], "assign")) {
            assign_command(argc, argv);
            continue;
        }
        if (kstreq(argv[0], "kill")) {
            kill_command(argc, argv);
            continue;
        }
        size_t command_len = kstrlen(argv[0]);
        if (argc == 1 && command_len > 1 && argv[0][command_len - 1] == ':') {
            if (chdir(argv[0]) < 0) {
                kputs("cd: cannot change directory to ");
                kputs(argv[0]);
                kputs("\n");
            }
            continue;
        }

        int background = take_background(&argc, argv);
        if (argc == 0) {
            continue;
        }
        int core = take_core(&argc, argv);
        if (core == -2) {
            kputs("shell: @N needs a core number, e.g. @1\n");
            continue;
        }
        int flags = (background ? SPAWN_NOWAIT : 0) | (core >= 0 ? SPAWN_CORE(core) : 0);
        int pid = spawn_flags(argv[0], argc, argv, flags);
        if (pid < 0) {
            if (core >= 0) {
                kputs("shell: cannot run ");
                kputs(argv[0]);
                kputs(" on cpu ");
                kput_int(core);
                kputs(" (no such program, or no such core)\n");
            } else {
                kputs("shell: no such program: ");
                kputs(argv[0]);
                kputs("\n");
            }
            continue;
        }
        if (background) {
            kputs("[");
            kput_int(pid);
            kputs("] started\n");
            continue;
        }
        print_job_end(pid, "] exited with ", wait(pid));
    }
}
