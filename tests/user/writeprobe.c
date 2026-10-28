// SPDX-License-Identifier: GPL-3.0-or-later
// Runs only on a disposable, writable FAT32 root. UTF-8 paths are compiled in
// because interactive console input currently accepts ASCII characters only.
//   writeprobe [volume:]   with a read-only volume (holding README.TXT), also
//                          checks that it refuses writes, creates and renames
#include "../../user/libk/koraos.h"

static unsigned char buffer[1024], verify[1024];
static int held[16];
static const char *initial = "/write tests/initial data.bin";
static const char *final = "/write tests/final data.bin";

static int failure(unsigned step) {
    kputs("writeprobe: FAILED step ");
    kput_int(step);
    kputs("\n");
    return 1;
}
static int write_all(int fd, const void *data, size_t size) {
    const unsigned char *bytes = data;
    size_t position = 0;
    while (position < size) {
        ssize_t count = write(fd, bytes + position, size - position);
        if (count <= 0) return -1;
        position += (size_t)count;
    }
    return 0;
}
static int write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) return -1;
    int result = write_all(fd, text, kstrlen(text));
    if (close(fd) < 0) result = -1;
    return result;
}
// `volume` followed by `name`, in a static buffer.
static const char *on_volume(const char *volume, const char *name) {
    static char path[96];
    size_t n = 0;
    for (; volume[n] != '\0' && n < sizeof(path) - 1; n++) path[n] = volume[n];
    for (size_t i = 0; name[i] != '\0' && n < sizeof(path) - 1; i++) path[n++] = name[i];
    path[n] = '\0';
    return path;
}
static unsigned char pattern(unsigned position) {
    return (unsigned char)(position * 37 + 11);
}

int main(int argc, char **argv) {
    const char *read_only = argc > 1 ? argv[1] : NULL;  // a volume, e.g. "df0:"
    if (mkdir("/write tests") != 0 || mkdir("/write tests/nested old") != 0) return failure(1);
    int fd = open(initial, O_RDWR | O_CREAT | O_EXCL);
    if (fd < 0) return failure(2);
    if (write(fd, NULL, 0) != 0 || read(fd, NULL, 0) != 0 ||
        read(fd, buffer, 0x100000000UL) != -1 || write(fd, buffer, 0x100000000UL) != -1 ||
        read(999, NULL, 0) != -1 || write(999, NULL, 0) != -1) return failure(3);
    for (unsigned position = 0; position < 8193;) {
        unsigned count = 8193 - position;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        for (unsigned i = 0; i < count; i++) buffer[i] = pattern(position + i);
        if (write_all(fd, buffer, count) < 0) return failure(4);
        position += count;
    }
    if (lseek(fd, 509, SEEK_SET) != 509 || write_all(fd, "ZZZZZZZZZZZZZZZZZZZ", 19) < 0 ||
        lseek(fd, 0, SEEK_SET) != 0) return failure(5);
    for (unsigned position = 0; position < 8193;) {
        unsigned count = 8193 - position;
        if (count > sizeof(verify)) count = sizeof(verify);
        if (read(fd, verify, count) != (ssize_t)count) return failure(6);
        for (unsigned i = 0; i < count; i++) {
            unsigned at = position + i;
            unsigned char expected = at >= 509 && at < 528 ? 'Z' : pattern(at);
            if (verify[i] != expected) return failure(7);
        }
        position += count;
    }
    if (close(fd) != 0) return failure(8);
    int reader = open(initial, O_RDONLY);
    int first = open(initial, O_WRONLY | O_APPEND);
    int second = open(initial, O_WRONLY | O_APPEND);
    if (reader < 0 || first < 0 || second < 0 || write(reader, NULL, 0) != -1 ||
        read(first, NULL, 0) != -1 || write_all(first, "append A\n", 9) < 0 ||
        write_all(second, "append B\n", 9) < 0 || lseek(reader, 8193, SEEK_SET) != 8193 ||
        read(reader, verify, 18) != 18) return failure(9);
    const char *appended = "append A\nappend B\n";
    for (unsigned i = 0; i < 18; i++) if (verify[i] != (unsigned char)appended[i]) return failure(10);
    if (unlink(initial) != -1 || rename(initial, final) != -1) return failure(11);
    if (close(reader) != 0 || close(first) != 0 || close(second) != 0 ||
        rename(initial, final) != 0) return failure(12);
    struct stat info;
    if (stat(final, &info) != 0 || info.size != 8211 || info.is_dir || stat(initial, &info) != -1 ||
        open(final, O_RDONLY | O_TRUNC) != -1 || open(final, O_WRONLY | O_EXCL) != -1 ||
        open(final, 0x1000) != -1 || open("/write tests", O_RDWR) != -1 ||
        (read_only != NULL && open(on_volume(read_only, "README.TXT"), O_WRONLY) != -1))
        return failure(13);
    for (unsigned i = 0; i < 16; i++) {
        held[i] = open(final, O_RDONLY);
        if (held[i] < 0) return failure(14);
    }
    if (open("/write tests/no fd create.txt", O_WRONLY | O_CREAT | O_EXCL) != -1 ||
        open(final, O_WRONLY | O_TRUNC) != -1 ||
        stat("/write tests/no fd create.txt", &info) != -1 ||
        stat(final, &info) != 0 || info.size != 8211) return failure(15);
    for (unsigned i = 0; i < 16; i++) if (close(held[i]) != 0) return failure(16);
    if (write_text("/write tests/truncate.txt", "discard this longer content\n") < 0) return failure(17);
    fd = open("/write tests/truncate.txt", O_WRONLY | O_TRUNC);
    if (fd < 0 || write_all(fd, "short\n", 6) < 0 || close(fd) != 0) return failure(18);
    if (write_text("/write tests/résumé-notes 東京.txt", "unicode payload\n") < 0 ||
        write_text("/write tests/nested old/check.txt", "nested\n") < 0 ||
        rename("/write tests/nested old", "/write tests/nested new") != 0) return failure(19);
    if (write_text("/write tests/delete me.txt", "temporary\n") < 0 ||
        unlink("/write tests/delete me.txt") != 0 || mkdir("/write tests/remove dir") != 0 ||
        rmdir("/write tests/remove dir") != 0 || rmdir("/write tests") != -1) return failure(20);
    if (mkdir("/write tests/pinned parent") != 0 || mkdir("/write tests/pinned parent/child") != 0 ||
        assign("hold", "sys:write tests/pinned parent/child") != 0 ||
        rmdir("/write tests/pinned parent/child") != -1 ||
        rename("/write tests/pinned parent", "/moved parent") != -1 ||
        assign("hold", NULL) != 0 || rmdir("/write tests/pinned parent/child") != 0 ||
        rmdir("/write tests/pinned parent") != 0) return failure(21);
    if (chdir("/write tests") != 0 || rename("sys:write tests", "sys:renamed tests") != -1 ||
        chdir("sys:") != 0 ||
        (read_only != NULL && rename(final, on_volume(read_only, "moved.txt")) != -1) ||
        stat(final, &info) != 0 || info.size != 8211) return failure(22);
    fd = open("/write tests/read only create.txt", O_RDONLY | O_CREAT | O_EXCL);
    if (fd < 0 || read(fd, verify, sizeof(verify)) != 0 || write(fd, "x", 1) != -1 ||
        close(fd) != 0 || open("/write tests/read only create.txt", O_RDONLY | O_CREAT | O_EXCL) != -1 ||
        (read_only != NULL &&
         open(on_volume(read_only, "no writable create.txt"), O_RDONLY | O_CREAT | O_EXCL) != -1))
        return failure(23);
    if (sync() != 0) return failure(24);
    kputs("writeprobe: data, namespace, UTF-8 and sync checked\n");
    return 0;
}
