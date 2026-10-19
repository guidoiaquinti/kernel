// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real EL0 parser with isolated syscall stubs; libc I/O stays intact.
#include "test.h"
#define main shell_test_main
#define read(...) shell_mock_read(__VA_ARGS__)
#define write(...) shell_mock_write(__VA_ARGS__)
#define chdir(...) shell_mock_chdir(__VA_ARGS__)
#define getcwd(...) shell_mock_getcwd(__VA_ARGS__)
#define volume_info(...) shell_mock_volume_info(__VA_ARGS__)
#define assign_info(...) shell_mock_assign_info(__VA_ARGS__)
#define assign(...) shell_mock_assign(__VA_ARGS__)
#define spawn_flags(...) shell_mock_spawn_flags(__VA_ARGS__)
#define waitpid(...) shell_mock_waitpid(__VA_ARGS__)
#define sync(...) shell_mock_sync(__VA_ARGS__)
#include "../../user/shell.c"
#undef main
#undef read
#undef write
#undef chdir
#undef getcwd
#undef volume_info
#undef assign_info
#undef assign
#undef spawn_flags
#undef waitpid
#undef sync

static char input[3 * LINE_MAX], output[2 * LINE_MAX];
static size_t input_size, input_position, output_size, read_chunk;
static unsigned spawn_calls, assign_calls, background_reaps;
static int last_argc, last_spawn_flags;
static bool background_job_live;
static bool last_argument_terminated, maximal_assign_received;

ssize_t shell_mock_read(int fd, void *buffer, size_t size) {
    CHECK(fd == 0 && size > 0, "shell requests console input");
    if (input_position == input_size) return 0;
    size_t count = 0;
    char *destination = buffer;
    while (count < size && (!read_chunk || count < read_chunk) && input_position < input_size) {
        char value = input[input_position++];
        destination[count++] = value;
        if (value == '\n') break; // The console syscall returns at a newline.
    }
    return (ssize_t)count;
}
ssize_t shell_mock_write(int fd, const void *buffer, size_t size) {
    CHECK(fd == 1, "shell writes stdout");
    const char *source = buffer;
    for (size_t i = 0; i < size && output_size + 1 < sizeof(output); i++) {
        output[output_size++] = source[i];
    }
    output[output_size] = 0;
    return (ssize_t)size;
}
int shell_mock_chdir(const char *path) { (void)path; return 0; }
int shell_mock_getcwd(char *buffer, size_t size) {
    if (size < 6) return -1;
    const char *root = "boot:";
    for (unsigned i = 0; i < 6; i++) buffer[i] = root[i];
    return 0;
}
int shell_mock_volume_info(unsigned index, struct volume_info *info) {
    (void)index; (void)info; return 0;
}
int shell_mock_assign_info(unsigned index, struct assign_info *info) {
    (void)index; (void)info; return 0;
}
int shell_mock_assign(const char *name, const char *target) {
    assign_calls++;
    maximal_assign_received = name && target && kstrlen(name) == 31 &&
                              kstrlen(target) == KORA_PATH_MAX - 1;
    return 0;
}
int shell_mock_spawn_flags(const char *name, int argc, char *const argv[], int flags) {
    (void)name;
    spawn_calls++;
    last_argc = argc;
    last_spawn_flags = flags;
    last_argument_terminated = argc > 0 && kstreq(argv[argc - 1], "a");
    if ((flags & 0xf00) == SPAWN_CORE(9)) {
        return -1;  // no such core
    }
    if (flags & SPAWN_NOWAIT) {
        background_job_live = true;
        return 124;
    }
    return 123;
}
int shell_mock_sync(void) { return 0; }
// A background job (pid 124) has finished by the next prompt.
int shell_mock_waitpid(int pid, int *code, int flags) {
    if (pid == -1) {
        CHECK(flags == WNOHANG, "job reaping never blocks the prompt");
        if (!background_job_live) return 0;
        background_job_live = false;
        background_reaps++;
        *code = 7;
        return 124;
    }
    CHECK(pid == 123 && flags == 0, "foreground wait matches child");
    *code = 0;
    return pid;
}

static void reset(void) {
    input_size = input_position = output_size = read_chunk = 0;
    spawn_calls = assign_calls = background_reaps = 0;
    last_argc = last_spawn_flags = 0;
    background_job_live = false;
    last_argument_terminated = maximal_assign_received = false;
    output[0] = 0;
}
static void append(const char *text) {
    while (*text) input[input_size++] = *text++;
}
static bool output_contains(const char *needle) {
    size_t length = kstrlen(needle);
    for (size_t i = 0; i + length <= output_size; i++) {
        size_t j = 0;
        while (j < length && output[i + j] == needle[j]) j++;
        if (j == length) return true;
    }
    return false;
}
static void fragmented_reads(void) {
    reset();
    append("echo a\nexit\n");
    read_chunk = 2;
    CHECK(read_command_line() == 1 && kstreq(line, "echo a\n"),
          "split console reads assembled before dispatch");
    CHECK(input_position == 7, "next command remains unread");
    CHECK(read_command_line() == 1 && kstreq(line, "exit\n"), "next fragmented command intact");
    reset();
    append("echo a");
    CHECK(read_command_line() == 0 && spawn_calls == 0, "EOF before newline cannot dispatch prefix");
}
static void line_boundaries(void) {
    reset();
    for (unsigned i = 0; i < LINE_MAX - 2; i++) input[input_size++] = 'a';
    input[input_size++] = '\n';
    CHECK(read_command_line() == 1 && input_position == LINE_MAX - 1 &&
          line[LINE_MAX - 2] == '\n' && line[LINE_MAX - 1] == 0,
          "largest complete line accepted and terminated");
    reset();
    for (unsigned i = 0; i < LINE_MAX + 133; i++) input[input_size++] = 'a';
    input[input_size++] = '\n';
    append("echo a\nexit\n");
    CHECK(read_command_line() == 0 && output_contains("shell: line too long"),
          "oversized line drained and rejected");
    CHECK(read_command_line() == 1 && kstreq(line, "echo a\n"),
          "draining leaves subsequent command intact");
    reset();
    for (unsigned i = 0; i < LINE_MAX - 1; i++) input[input_size++] = 'a';
    input[input_size++] = '\n';
    append("exit\n");
    CHECK(read_command_line() == 0 && read_command_line() == 1 && kstreq(line, "exit\n"),
          "full buffer without newline rejected at exact boundary");
}
static void drained_line_not_dispatched(void) {
    reset();
    append("echo a ");
    while (input_size < LINE_MAX + 133) input[input_size++] = ' ';
    append("\necho a\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 1 && last_argc == 2,
          "oversized command never dispatches its prefix and following command runs once");
    CHECK(output_contains("shell: line too long"), "dispatch path reports drained line");
}
static void argument_boundaries(void) {
    reset();
    append("echo");
    for (unsigned i = 0; i < 16; i++) append(" a");
    append("\necho a\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 1 && last_argc == 2,
          "seventeenth token rejected without dispatch, next command runs");
    CHECK(output_contains("shell: too many arguments"), "excess arguments diagnostic");
    reset();
    append("echo");
    for (unsigned i = 0; i < 15; i++) append(" a");
    append(" \t\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 1 && last_argc == 16,
          "sixteen tokens accepted with trailing whitespace");
    CHECK(last_argument_terminated, "last accepted token excludes trailing whitespace/newline");
}
static void maximal_assign_line(void) {
    reset();
    append("assign ");
    for (unsigned i = 0; i < 31; i++) input[input_size++] = 'n';
    input[input_size++] = ' ';
    for (unsigned i = 0; i < KORA_PATH_MAX - 1; i++) input[input_size++] = 't';
    append("\nexit\n");
    read_chunk = 97;
    CHECK(shell_test_main() == 0 && assign_calls == 1 && maximal_assign_received,
          "maximum assign name and qualified target reach syscall intact");
    CHECK(spawn_calls == 0, "assign built-in never dispatches as an ELF command");
}
static void quoted_tokens(void) {
    char *argv[MAX_ARGV];
    char text[] = "cp 'single quoted.txt' \"double\\\" quoted.txt\" escaped\\ name.txt '' ab\"cd\"\\ ef & '&'";
    int count = tokenize(text, argv);
    CHECK(count == 8, "quotes, escapes, empty argument and concatenation tokenized");
    if (count == 8) {
        CHECK(kstreq(argv[1], "single quoted.txt") && kstreq(argv[2], "double\" quoted.txt") &&
              kstreq(argv[3], "escaped name.txt") && kstreq(argv[4], "") &&
              kstreq(argv[5], "abcd ef"), "quote decoding preserves argument contents");
        CHECK(kstreq(argv[6], "&") && !literal_tokens[6] &&
              kstreq(argv[7], "&") && literal_tokens[7], "quoted operators retain literal provenance");
    }
    char single[] = "echo 'back\\slash' résumé-notes.txt";
    CHECK(tokenize(single, argv) == 3 && kstreq(argv[1], "back\\slash") &&
          kstreq(argv[2], "résumé-notes.txt"), "single quotes preserve slashes and lexer preserves UTF-8 bytes");
    const char *invalid[] = {"echo 'open", "echo \"open", "echo trailing\\", "echo \"escape\\\n"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char malformed[64];
        unsigned j = 0;
        do { malformed[j] = invalid[i][j]; } while (invalid[i][j++]);
        CHECK(tokenize(malformed, argv) == -2, "malformed quote/escape rejected");
    }
    reset();
    append("echo 'unterminated\necho a\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 1 && output_contains("shell: invalid quoting"),
          "malformed quoting never dispatches its prefix and next command runs");
}
static void background_jobs(void) {
    reset();
    append("echo a &\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 1 && last_argc == 2 &&
          last_spawn_flags == SPAWN_NOWAIT && last_argument_terminated,
          "separate & starts a background job without passing \"&\" as an argument");
    CHECK(output_contains("[124] started") && output_contains("[124] done, exit 7") &&
          background_reaps == 1, "background job announced and reaped at the next prompt");
    reset();
    append("echo a&\necho a\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 2 && last_spawn_flags == 0 &&
          last_argument_terminated, "trailing & is stripped; the next command runs in front");
    CHECK(output_contains("[123] exited with 0"), "foreground job still waited for");
    reset();
    append("&\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 0, "a lone & runs nothing");
    reset();
    append("echo '&'\necho a\\&\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 2 && last_spawn_flags == 0 &&
          !background_reaps && !output_contains("started"),
          "quoted and escaped ampersands are arguments, not background jobs");
}

static void core_pinning(void) {
    reset();
    append("echo a @2\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 1 && last_argc == 2 &&
          last_spawn_flags == SPAWN_CORE(2) && last_argument_terminated,
          "trailing @2 pins to core 2 and is not an argument");
    reset();
    append("echo a @3 &\nexit\n");
    CHECK(shell_test_main() == 0 && last_argc == 2 &&
          last_spawn_flags == (SPAWN_NOWAIT | SPAWN_CORE(3)), "@N combines with &");
    reset();
    append("echo a '@2'\necho a \\@2\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 2 && last_argc == 3 && last_spawn_flags == 0,
          "quoted and escaped @2 are arguments");
    reset();
    append("echo a @x\necho a @\necho a @12345\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 0 &&
          output_contains("shell: @N needs a core number"), "malformed @N rejected, nothing run");
    reset();
    append("echo a @9\nexit\n");
    CHECK(shell_test_main() == 0 && spawn_calls == 1 &&
          output_contains("shell: cannot run echo on cpu 9"), "spawn failure on a pinned core reported");
}

TEST_MAIN(fragmented_reads, line_boundaries, drained_line_not_dispatched,
          argument_boundaries, maximal_assign_line, quoted_tokens, background_jobs, core_pinning)
