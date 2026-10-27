#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""QEMU smoke test: boot the QEMU kernel on the raspi3b, raspi4b or virt
machine, drive the shell over the serial line, and check the output and the
framebuffer.

Waits for expected output (with timeouts) rather than sleeping, so it is not
sensitive to how fast the machine runs QEMU. Standard library only.

Usage: tests/run-qemu-smoke.py --target qemu_raspi3b|qemu_raspi4b|qemu_virt [--out DIR]
Build the kernel first with: ./build.sh --target qemu_raspi3b

Artifacts in --out (default build/qemu-smoke): serial.log and one PNG
screenshot per screen check.
"""

import argparse
import importlib.util
import os
import re
import select
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Kernel fault banners (src/arch/exception.c) start like this.
FAULT = re.compile(rb"\n\*\*\* ")


class Failure(Exception):
    pass


class Qemu:
    def __init__(self, qemu, kernel, outdir, machine="raspi3b", ram="256M", extra=(), el2=False,
                 smp=1, gic=2, accel="tcg"):
        self.outdir = outdir
        self.log = open(os.path.join(outdir, "serial.log"), "wb")
        self.pending = b""  # output not yet consumed by expect()
        # The monitor socket path is relative to QEMU's working directory:
        # UNIX socket paths are limited to ~104 bytes and temp dirs are long.
        machine_args = ["-M", machine] if machine.startswith("raspi") else [
            "-M", "virt,gic-version=%d,highmem=off" % gic + (",virtualization=on" if el2 else ""),
            *(["-accel", "hvf", "-cpu", "host"] if accel == "hvf" else ["-cpu", "cortex-a72"]),
            "-smp", str(smp), "-nic", "none", "-global", "virtio-mmio.force-legacy=false", "-m", ram]
        self.proc = subprocess.Popen(
            [qemu, *machine_args, *extra, "-kernel", kernel,
             "-serial", "stdio", "-display", "none",
             "-monitor", "unix:monitor.sock,server,nowait"],
            cwd=outdir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)

    def expect(self, pattern, timeout=30):
        """Wait until `pattern` (bytes regex) appears in the output; consume
        the output up to the end of the match."""
        regex = re.compile(pattern, re.MULTILINE)
        deadline = time.monotonic() + timeout
        while True:
            match = regex.search(self.pending)
            if match:
                self.pending = self.pending[match.end():]
                return match
            if FAULT.search(self.pending):
                self.drain(0.5)  # the rest of the fault report
                raise Failure("kernel fault:\n" + self.tail())
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise Failure("timed out waiting for %r; last output:\n%s"
                              % (pattern, self.tail()))
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if ready:
                data = os.read(self.proc.stdout.fileno(), 4096)
                if not data:
                    raise Failure("QEMU exited; last output:\n" + self.tail())
                self.log.write(data)
                self.log.flush()
                self.pending += data

    def drain(self, seconds):
        """Collect output for `seconds` more (after a fault, for its report)."""
        deadline = time.monotonic() + seconds
        while (remaining := deadline - time.monotonic()) > 0:
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not ready:
                break
            data = os.read(self.proc.stdout.fileno(), 4096)
            if not data:
                break
            self.log.write(data)
            self.log.flush()
            self.pending += data

    def send(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def tail(self, n=1500):
        return self.pending[-n:].decode(errors="replace")

    def monitor(self, command):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        cwd = os.getcwd()
        os.chdir(self.outdir)
        try:
            sock.connect("monitor.sock")
        finally:
            os.chdir(cwd)
        sock.settimeout(30)

        def until_prompt():
            data = b""
            while not data.endswith(b"(qemu) "):
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
            return data

        until_prompt()  # banner
        sock.sendall(command.encode() + b"\n")
        reply = until_prompt()
        sock.close()
        return reply

    def screendump(self, name):
        """Capture the framebuffer; save <name>.png; return (w, h, rgb)."""
        ppm = os.path.join(self.outdir, name + ".ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        self.monitor("screendump " + name + ".ppm")
        width, height, rgb = read_ppm(ppm)
        write_png(os.path.join(self.outdir, name + ".png"), width, height, rgb)
        os.remove(ppm)
        return width, height, rgb

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        self.log.close()
        sock = os.path.join(self.outdir, "monitor.sock")
        if os.path.exists(sock):
            os.remove(sock)


def read_ppm(path):
    for _ in range(100):  # wait for QEMU to finish writing it
        if os.path.exists(path) and os.path.getsize(path) > 0:
            break
        time.sleep(0.1)
    with open(path, "rb") as f:
        data = f.read()
    tokens = data.split(maxsplit=4)  # P6, width, height, maxval, pixels
    if tokens[0] != b"P6":
        raise Failure("unexpected screendump format")
    width, height = int(tokens[1]), int(tokens[2])
    rgb = data[len(data) - width * height * 3:]
    return width, height, rgb


def write_png(path, width, height, rgb):
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride]
                   for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def pixel(shot, x, y):
    width, _, rgb = shot
    i = (y * width + x) * 3
    return tuple(rgb[i:i + 3])


def near(actual, expected, tolerance=8):
    return all(abs(a - e) <= tolerance for a, e in zip(actual, expected))


PROMPT = rb"^\$ "


def ps_table(q, tids=False):
    """Run `ps` in the shell: [(pid, ppid, core, state, name)], one row per
    thread, prompt consumed; with tids, [(pid, tid, ppid, core, state, name)]."""
    q.send("ps\r")
    q.expect(rb"^\s+PID\s+TID\s+PPID\s+CPU\s+STATE\s+NAME")
    rows = []
    while True:
        m = q.expect(rb"^\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\w+)\s+(\S+)\r?\n|"
                     rb"^\[\d+\] exited with 0")
        if m.group(1) is None:
            break
        pid, tid, ppid, core = (int(g) for g in m.groups()[:4])
        state, name = m.group(5).decode(), m.group(6).decode()
        rows.append((pid, tid, ppid, core, state, name) if tids else
                    (pid, ppid, core, state, name))
    q.expect(PROMPT)
    return rows


def core_of(q, name):
    for _, _, core, _, task in ps_table(q):
        if task == name:
            return core
    raise Failure("ps does not list %s" % name)


def sd_fixture():
    """tests/create-sd-card-image.py, for the expected card contents."""
    spec = importlib.util.spec_from_file_location("sd_card", os.path.join(ROOT, "tests/create-sd-card-image.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def storage_steps(q, what, label, writable, cores):
    """Steps for a scratch drive from tests/create-sd-card-image.py (an SD card or
    a USB drive) whose FAT32 volume is labelled `label`."""
    # FAT32 writes go one sector per request, and in QEMU a USB command takes
    # several full-speed frames: copying BIG.BIN to a USB drive takes a minute.
    timeout = 300 if what.startswith("USB") else 30
    card = sd_fixture()
    big = card.big_content()
    sums = {"README.TXT": (len(card.MARKER), card.fnv1a64(card.MARKER.encode())),
            "BIG.BIN": (len(big), card.fnv1a64(big))}
    device = []

    def check_sums(names, core=None):
        """sumprobe each drive file in `names` (path: expected file name), on
        `core` if given."""
        q.send("sumprobe %s%s\r" % (" ".join("%s:/%s" % (device[0], path) for path in names),
                                    "" if core is None else " @%d" % core))
        for path, expected in names.items():
            m = q.expect(rb"^sumprobe: \S+ (?:(\d+) ([0-9a-f]{16})|unreadable)\r?\n", timeout)
            if m.group(1) is None:
                raise Failure("cannot read %s:/%s from the %s" % (device[0], path, what))
            got = (int(m.group(1)), int(m.group(2), 16))
            if got != sums[expected]:
                raise Failure("%s:/%s from the %s: size/checksum %s, want %s"
                              % (device[0], path, what, got, sums[expected]))
        q.expect(rb"\] exited with 0")
        q.expect(PROMPT)

    def volume():
        q.send("volumes\r")
        m = q.expect(rb"^(df\d+): " + label.lower().encode() + rb"( \[read-only\])?\r?\n")
        q.expect(PROMPT)
        device.append(m.group(1).decode())
        if (m.group(2) is None) != writable:
            raise Failure("%s volume is %s" % (what, "writable" if m.group(2) is None else "read-only"))
        # Multi-block reads of an odd-sized file, compared with the host's bytes:
        # from core 0, where USB interrupts arrive, and from the last core.
        check_sums({"README.TXT": "README.TXT", "BIG.BIN": "BIG.BIN"}, 0)
        if cores > 1:
            check_sums({"BIG.BIN": "BIG.BIN"}, cores - 1)
        # A program loaded straight off the drive.
        q.send("%s:/bin/hello\r" % device[0])
        q.expect(rb"^Hello from userland \(ELF\)")
        q.expect(PROMPT)
    yield "%s: its FAT32 volume, multi-block reads and a program from it" % what, volume

    def writes():
        if not writable:
            q.send("mkdir %s:/NOPE\r" % device[0])
            q.expect(rb"mkdir: cannot create")
            q.expect(PROMPT)
            return
        for command in ("cp %s:/BIG.BIN %s:/COPY.BIN", "mkdir %s:/NEWDIR",
                        "cp %s:/README.TXT %s:/NEWDIR/NOTE.TXT", "sync"):
            q.send((command % ((device[0],) * command.count("%s"))) + "\r")
            if command != "sync":
                q.expect(rb"\] exited with 0", timeout)
            q.expect(PROMPT, timeout)
        check_sums({"COPY.BIN": "BIG.BIN", "NEWDIR/NOTE.TXT": "README.TXT"})
    yield ("%s: writes, verified on the host afterwards" % what if writable else
           "%s: read-only unless built --storage-writable" % what), writes


def run(q, graphics=True, keyboard=False, repeat=0, multi_volume=False, write_test=False, cores=1,
        stress=0, media=()):
    """Yield (name, check) steps; each check raises Failure on error. `media` lists
    the attached scratch drives as (what, volume label, writable) tuples."""

    def boot():
        q.expect(rb"KoraOS shell\. Type 'help'\.", timeout=90)
        q.expect(PROMPT)
    yield "boots to the shell", boot

    for what, label, writable in media:
        yield from storage_steps(q, what, label, writable, cores)

    def ls():
        q.send("ls /bin\r")
        for name in (b"hello", b"shell", b"termdemo"):
            q.expect(rb"^" + name + rb"\r?$")
        q.expect(rb"exited with 0")
        q.expect(PROMPT)
    yield "ls /bin lists the programs", ls

    def hello():
        q.send("hello\r")
        q.expect(rb"^Hello from userland \(ELF\)")
        q.expect(PROMPT)
    yield "hello runs", hello

    def echo():
        q.send("echo alpha beta\r")
        q.expect(rb"^alpha beta\r?$")
        q.expect(rb"exited with 2")  # echo exits with its argument count
        q.expect(PROMPT)
    yield "echo gets argv and exit code", echo

    def cat():
        q.send("cat /docs/a-long-file-name.txt\r")
        q.expect(rb"long-filename \(LFN\)")
        q.expect(PROMPT)
    yield "cat reads a long-filename file", cat

    def namespace():
        q.send("nsprobe%s\r" % (" extras" if multi_volume else ""))
        q.expect(rb"nsprobe: child cwd checked")
        if multi_volume:
            q.expect(rb"nsprobe: multi-volume checked")
        q.expect(rb"nsprobe: namespace checked")
        q.expect(rb"exited with 0")
        q.expect(PROMPT)
    yield "volume paths, cwd inheritance and retained file descriptors", namespace

    def shell_navigation():
        def command(text):
            q.send(text + "\r")
            q.expect(PROMPT)

        def pwd(expected):
            q.send("pwd\r")
            q.expect(rb"^" + re.escape(expected) + rb"\r?$")
            q.expect(PROMPT)

        q.send("pwd\r")
        root = q.expect(rb"^([a-zA-Z0-9_-]+:)\r?$").group(1)
        q.expect(PROMPT)
        q.send("echo" + " a" * 16 + "\r")
        q.expect(rb"shell: too many arguments")
        q.expect(PROMPT)
        q.send("echo accepted\r")
        q.expect(rb"^accepted\r?$")
        q.expect(rb"exited with 1")
        q.expect(PROMPT)
        command("cd docs")
        pwd(root + b"docs")
        q.send("ls\r")
        q.expect(rb"^a-long-file-name\.txt\r?$")
        q.expect(rb"exited with 0")
        q.expect(PROMPT)
        command("cd missing/..")
        pwd(root + b"docs")
        command("cd ..")
        pwd(root)
        command("cd /docs")
        pwd(root + b"docs")
        command(root.decode())
        pwd(root)
        q.send("volumes\r")
        q.expect(rb"^df[0-9]+: [^\r\n]*\[boot\]")
        if multi_volume:
            q.expect(rb"^df[0-9]+: extras[^\r\n]*\[read-only\]")
        q.expect(PROMPT)
        q.send("assign\r")
        q.expect(rb"^sys: = [^\r\n]*\[fixed\]")
        q.expect(rb"^c: = [^\r\n]*:bin")
        q.expect(PROMPT)
        command("SYS:")
        pwd(root)
        if multi_volume:
            command("ExTrAs:")
            pwd(b"extras:")
            command("cd docs")
            pwd(b"extras:docs")
            command("cd /")
            pwd(b"extras:")
            command("cd sys:docs")
            pwd(root + b"docs")
            command("assign c extras:bin")
            q.send("extrahello\r")
            q.expect(rb"^Hello from userland \(ELF\)")
            q.expect(rb"exited with 0")
            q.expect(PROMPT)
            q.send("c:/hello\r")
            q.expect(rb"^Hello from userland \(ELF\)")
            q.expect(rb"exited with 0")
            q.expect(PROMPT)
            command("assign c sys:docs")
            q.send("hello\r")
            q.expect(rb"shell: no such program: hello")
            q.expect(PROMPT)
            command("assign c")
            q.send("hello\r")
            q.expect(rb"shell: no such program: hello")
            q.expect(PROMPT)
            command("assign c sys:bin")
            command("sys:")
            pwd(root)
    yield "shell navigation, volume listing and single-target assigns", shell_navigation

    if write_test:
        def writable_filesystem():
            def tool(command, status=0):
                q.send(command + "\r")
                q.expect(rb"exited with " + str(status).encode() + rb"\r?$")
                q.expect(PROMPT)

            q.send("writeprobe\r")
            q.expect(rb"writeprobe: data, namespace, UTF-8 and sync checked", timeout=60)
            q.expect(rb"exited with 0")
            q.expect(PROMPT)
            tool('mkdir "write cli"')
            tool('cp sys:README.TXT "write cli/copy with spaces.txt"')
            tool("mv 'write cli/copy with spaces.txt' \"write cli/renamed copy.txt\"")
            tool('cp "write cli/renamed copy.txt" "quoted copy.txt"')
            tool('cp sys:README.TXT sys:README.TXT', 1)
            tool('mv "quoted copy.txt" sys:README.TXT', 1)
            tool('rm write\\ cli/renamed\\ copy.txt')
            tool('rmdir "write cli"')
            q.send("sync\r")
            result = q.expect(rb"(^sync: failed|^\$ )")
            if result.group(1) == b"sync: failed":
                raise Failure("explicit sync failed")
        yield "EL0 writes, UTF-8 paths, quoted file tools and sync", writable_filesystem

    def debug_console():
        q.send("\x14")  # Ctrl-T
        q.expect(rb"\[serial -> kernel console")
        q.expect(rb"koraos> ")
        q.send("version\r")
        q.expect(rb"KoraOS version \d")
        q.send("\x14")
        q.expect(rb"\[serial -> screen terminal")
    yield "Ctrl-T reaches the kernel debug console and back", debug_console

    def irqs():
        q.send("\x14")
        q.expect(rb"koraos> ")
        deadline = time.monotonic() + 5
        while True:
            q.send("irqs\r")
            q.expect(rb"^controller: (?:BCM2835 legacy|GIC-400|GICv[23] \(virt\))")
            m = q.expect(rb"^systick: (\d+) ticks, uptime (\d+)\.(\d+) s")
            ticks = int(m.group(1))
            uptime = int(m.group(2)) + int(m.group(3)) / 1000
            if ticks > uptime * 100 + 1:
                raise Failure("systick %d ticks after %.3f s uptime" % (ticks, uptime))
            m = q.expect(rb"^\s+\d+\s+(\d+)\s+uart \(PL011\)")
            if int(m.group(1)) == 0:
                raise Failure("the UART receive interrupt never fired")
            q.expect(rb"koraos> ")
            if ticks >= 5:
                break
            if time.monotonic() >= deadline:
                raise Failure("system tick never advanced to five interrupts")
            time.sleep(0.05)
        q.send("\x14")
        q.expect(rb"\[serial -> screen terminal")
    yield "irqs shows the system tick and the UART interrupt", irqs

    def per_core_ticks():
        def read_cpus():
            q.send("cpus\r")
            ticks = {}
            while True:
                m = q.expect(rb"^\s+(\d+)\s+(online|offline)(?:\s+(\d+)\s+(\S+))?\r?\n|"
                             rb"^cpus: (\d+) online, this is cpu (\d+)")
                if m.group(5):
                    if int(m.group(5)) != cores or m.group(6) != b"0":
                        raise Failure("cpus reports %s online on cpu %s, want %d on cpu 0"
                                      % (m.group(5).decode(), m.group(6).decode(), cores))
                    break
                if m.group(2) == b"online":
                    ticks[int(m.group(1))] = (int(m.group(3)), m.group(4).decode())
            q.expect(rb"koraos> ")
            return ticks
        q.send("\x14")
        q.expect(rb"koraos> ")
        first = read_cpus()
        time.sleep(0.3)
        second = read_cpus()
        q.send("\x14")
        q.expect(rb"\[serial -> screen terminal")
        if sorted(second) != list(range(cores)):
            raise Failure("online cores %s, want 0..%d" % (sorted(second), cores - 1))
        for core in range(cores):
            if second[core][0] <= first[core][0]:
                raise Failure("cpu %d's own timer did not tick (%d -> %d)"
                              % (core, first[core][0], second[core][0]))
            if core and second[core][1] != "idle%d" % core:
                raise Failure("cpu %d runs %s, want its idle task" % (core, second[core][1]))
    yield "every core is online with its own timer ticking", per_core_ticks

    def kernel_heap():
        def heap_stats():
            q.send("heap\r")
            m = q.expect(rb"^heap: (\d+) live, \d+ allocated, (\d+) failed, (\d+) bad frees")
            live, failed, bad = (int(g) for g in m.groups())
            m = q.expect(rb"^pages: (\d+) of (\d+) free")
            q.expect(rb"koraos> ")
            return live, failed, bad, int(m.group(1))

        q.send("\x14")
        q.expect(rb"koraos> ")
        live, failed, bad, pages = heap_stats()
        for _ in range(4):  # each run seeds from the uptime
            q.send("heaptest 3000\r")
            m = q.expect(rb"^heaptest: (?:3000 rounds ok|FAILED check (\d+))", timeout=60)
            if m.group(1):
                raise Failure("heaptest failed check %s" % m.group(1).decode())
            q.expect(rb"koraos> ")
        live2, failed2, bad2, pages2 = heap_stats()
        if (live2, failed2, bad2) != (live, failed, bad):
            raise Failure("heap changed across heaptest: live/failed/bad %s -> %s"
                          % ((live, failed, bad), (live2, failed2, bad2)))
        if bad:
            raise Failure("%d bad kfree calls since boot" % bad)
        # At most one cached empty slab per size class may stay behind.
        if pages2 < pages - 9:
            raise Failure("heaptest leaked pages: %d -> %d free" % (pages, pages2))
        q.send("\x14")
        q.expect(rb"\[serial -> screen terminal")
    yield "kernel heap stress leaves no leaks", kernel_heap

    def fp_context():
        q.send("fpprobe\r")
        m = q.expect(rb"^fpprobe: ([^\r\n]*)\r?\n")
        if m.group(1) != b"registers preserved":
            raise Failure("fpprobe: %s" % m.group(1).decode(errors="replace"))
        q.expect(rb"exited with 0")
        q.expect(PROMPT)
    yield "FP/SIMD registers are per task (lazy switching)", fp_context

    def read_tasks():
        """Run `tasks` on the debug console: ({name: (state, stack)}, peak, size)."""
        listed, peak, size, _ = read_tasks_bkl()
        return {name: info[:2] for name, info in listed.items()}, peak, size

    def read_tasks_bkl():
        """`tasks` (tid, pid, cpu, state, stack, name) with the forbid flag and the
        BKL holder: ({name: (state, stack, forbidden)}, peak, size, holder pid
        or None)."""
        q.send("\x14")
        q.expect(rb"koraos> ")
        q.send("tasks\r")
        listed = {}
        while True:
            m = q.expect(rb"^(?:\s+(\d+)\s+\d+\s+(\d+)\s+(\w+)\s+(\d+)\s+(\S+)( \(forbid\))?(?: \(killed\))?|"
                         rb"kernel stack peak: (\d+) of (\d+) bytes)\r?\n")
            if m.group(7):
                peak, size = int(m.group(7)), int(m.group(8))
                break
            listed[m.group(5).decode()] = (m.group(3).decode(), int(m.group(4)), m.group(6) is not None)
        m = q.expect(rb"^big kernel lock: (free|pid (\d+))\r?\n")
        holder = int(m.group(2)) if m.group(2) else None
        q.expect(rb"koraos> ")
        q.send("\x14")
        q.expect(rb"\[serial -> screen terminal")
        return listed, peak, size, holder

    def tasks():
        # The shell sleeps in read() until input arrives; it does not spin. (A
        # few retries cover the moment between printing its prompt and reading.)
        # With everyone asleep, nobody holds the big kernel lock.
        for _ in range(5):
            listed, peak, size, holder = read_tasks_bkl()
            if (listed.get("init", ("",))[0] == "blocked" and
                    listed.get("shell", ("",))[0] == "blocked" and holder is None):
                break
            time.sleep(0.2)
        else:
            raise Failure("expected init and the shell blocked and the BKL free, got %s, "
                          "holder %s" % (listed, holder))
        # Every task so far, the probes included: keep a quarter of the stack spare.
        if peak == 0 or peak > size * 3 // 4:
            raise Failure("kernel stack peak %d of %d bytes" % (peak, size))
    yield "tasks: init and the shell sleep; kernel stacks have headroom", tasks

    def scheduling():
        q.send("schedprobe sleep 300\r")
        m = q.expect(rb"^schedprobe: slept (\d+) ms")
        slept = int(m.group(1))
        if not 300 <= slept <= 1300:
            raise Failure("msleep(300) took %d ms" % slept)
        q.expect(rb"exited with 0")
        q.expect(PROMPT)

        # A background job that never makes a syscall, on the shell's own core:
        # the shell only answers while it runs if the timer tick preempts it.
        shell_core = core_of(q, "shell")
        q.send("schedprobe spin 5000 @%d &\r" % shell_core)
        m = q.expect(rb"^\[(\d+)\] started")
        pid = m.group(1)
        q.expect(PROMPT)
        q.send("echo alive\r")
        # "alive" anchored (the echoed command line contains it too); the
        # background job's line may follow a prompt.
        m = q.expect(rb"^alive|schedprobe: spun")
        if m.group(0) != b"alive":
            raise Failure("the shell did not run while a background job was spinning")
        q.expect(PROMPT)
        # The shell may be asleep in read() or, preempted between its prompt and
        # that read, waiting for its next slice; the spinner is always runnable.
        listed, _, _ = read_tasks()
        if listed.get("schedprobe", ("",))[0] != "runnable" or "shell" not in listed:
            raise Failure("expected a runnable schedprobe next to the shell, got %s" % listed)
        q.expect(rb"schedprobe: spun \d+ ms", timeout=60)
        # The job exits just after printing; the first prompt after that
        # reports it.
        for _ in range(50):
            q.send("\r")
            m = q.expect(rb"^(?:\[" + pid + rb"\] done, exit (\d+)\r?\n)?\$ ")
            if m.group(1) is not None:
                if m.group(1) != b"0":
                    raise Failure("background job exited with %s" % m.group(1).decode())
                break
            time.sleep(0.1)
        else:
            raise Failure("the shell never reported the finished background job")
    yield "msleep, background jobs and preemption of a spinning task", scheduling

    def forbid_permit():
        # With a spinner competing, a normal task loses the CPU for whole time
        # slices (10 ms); while forbid()den it must not lose it at all. Host
        # scheduling noise can stretch a gap, so allow a few attempts.
        # Both on the shell's core, so they compete for it.
        core = core_of(q, "shell")
        q.send("schedprobe spin 8000 @%d &\r" % core)
        m = q.expect(rb"^\[(\d+)\] started")
        spinner = m.group(1)
        q.expect(PROMPT)
        results = []
        for _ in range(3):
            q.send("forbidprobe @%d\r" % core)
            m = q.expect(rb"^forbidprobe: (?:longest gap free (\d+) us, forbidden (\d+) us|([^\r\n]*))\r?\n",
                         timeout=60)
            if m.group(3) is not None:
                raise Failure("forbidprobe: %s" % m.group(3).decode(errors="replace"))
            q.expect(rb"exited with 0")
            q.expect(PROMPT)
            free_gap, forbidden_gap = int(m.group(1)), int(m.group(2))
            results.append((free_gap, forbidden_gap))
            if free_gap >= 5000 and forbidden_gap < 5000:
                break
        else:
            raise Failure("forbid() did not keep the CPU (free, forbidden gaps in us): %s" % results)
        q.expect(rb"schedprobe: spun \d+ ms", timeout=60)

        # Background output may follow the shell's prompt on the same line, so
        # the background jobs' lines are matched unanchored.
        # While a task holds forbid(), `tasks` (in the UART interrupt) shows it
        # holding the BKL; the shell cannot run until it permits.
        q.send("forbidprobe hold 3000 &\r")
        q.expect(rb"forbidprobe: holding")
        listed, _, _, holder = read_tasks_bkl()
        if not listed.get("forbidprobe", ("", 0, False))[2] or holder is None:
            raise Failure("forbidden task or BKL holder not shown: %s, holder %s" % (listed, holder))
        q.expect(rb"forbidprobe: released", timeout=60)
        for _ in range(50):
            q.send("\r")
            m = q.expect(rb"^(?:\[(\d+)\] done, exit (\d+)\r?\n)?\$ ")
            if m.group(1) is not None and m.group(1) != spinner:
                break
            time.sleep(0.1)
        # Nobody keeps the BKL once the system is idle. (The shell, on its own
        # core, may be caught in a syscall: look again.)
        for _ in range(10):
            _, _, _, holder = read_tasks_bkl()
            if holder is None:
                break
            time.sleep(0.2)
        else:
            raise Failure("the BKL is still held (by pid %s) with the system idle" % holder)
    yield "forbid()/permit(): nesting, no preemption, BKL held across EL0", forbid_permit

    def serial_lines_whole():
        # Threads on every core print a line per tick while the debug console
        # prints tables on core 0: whole writes to the serial line are atomic,
        # so no line may come out split by another core's output.
        start = os.path.getsize(q.log.name)
        q.send("threadprobe chatter 3000 &\r")
        q.expect(rb"threadprobe: chatter \d+ line \d+ end")
        for _ in range(6):
            read_tasks_bkl()
        q.expect(rb"threadprobe: chatter done", timeout=60)
        for _ in range(50):  # reap it, so later steps see a quiet system
            q.send("\r")
            m = q.expect(rb"^(?:\[\d+\] done, exit (\d+)\r?\n)?\$ ")
            if m.group(1) is not None:
                break
            time.sleep(0.1)
        with open(q.log.name, "rb") as log:
            log.seek(start)
            text = log.read()
        # A prompt prints no newline, so a whole line may follow one.
        prompts = rb"^(?:\$ |koraos> )*"
        chatter = re.compile(prompts + rb"threadprobe: chatter (?:\d+ line \d+ end|done)\r?$")
        console = re.compile(prompts + rb"(?:big kernel lock: (?:free|pid \d+)|"
                             rb"kernel stack peak: \d+ of \d+ bytes)\r?$")
        lines = text.split(b"\n")
        broken = [line for line in lines
                  if (b"threadprobe: chatter" in line and not chatter.match(line)) or
                  ((b"big kernel lock" in line or b"kernel stack peak" in line) and not console.match(line))]
        if broken or sum(1 for line in lines if chatter.match(line)) < 20:
            raise Failure("serial lines split by another core's output: %r" % broken[:5])
    yield "serial output from several cores stays whole, line by line", serial_lines_whole

    def reap(count):
        """Wait until the shell has reaped every finished schedprobe job (each
        prompt reports and reaps finished jobs; `ps` shows any left, zombies
        included). `count` is only for the error message."""
        for _ in range(100):
            if not any(name == "schedprobe" for *_, name in ps_table(q)):
                return
            time.sleep(0.1)
        raise Failure("%d background jobs were never reaped: %s" % (count, ps_table(q)))

    def spin_gaps(placements, ms=1500):
        """Run one spinner per @core (None: unpinned), wait, return their
        longest gaps in us."""
        for core in placements:
            q.send("schedprobe spin %d%s &\r" % (ms, "" if core is None else " @%d" % core))
            q.expect(rb"^\[\d+\] started")
            q.expect(PROMPT)
        gaps = [int(q.expect(rb"schedprobe: spun \d+ ms, longest gap (\d+) us", timeout=60).group(1))
                for _ in placements]
        reap(len(placements))
        return gaps

    def placement_and_pinning():
        rows = ps_table(q)
        names = {name: core for _, _, core, _, name in rows}
        if not {"init", "shell", "ps"} <= set(names):
            raise Failure("ps lists %s" % rows)
        if any(core >= cores for core in names.values()):
            raise Failure("ps shows a core outside 0..%d: %s" % (cores - 1, rows))
        if cores == 1:
            q.send("echo hi @1\r")
            q.expect(rb"shell: cannot run echo on cpu 1")
            q.expect(PROMPT)
            return
        # Pinned: the task runs where it was put.
        last = cores - 1
        q.send("schedprobe spin 2000 @%d &\r" % last)
        q.expect(rb"^\[\d+\] started")
        q.expect(PROMPT)
        if core_of(q, "schedprobe") != last:
            raise Failure("schedprobe @%d runs elsewhere: %s" % (last, ps_table(q)))
        q.expect(rb"schedprobe: spun \d+ ms", timeout=60)
        reap(1)
        # Unpinned: two jobs go to different (least-loaded) cores.
        for _ in range(2):
            q.send("schedprobe spin 2000 &\r")
            q.expect(rb"^\[\d+\] started")
            q.expect(PROMPT)
        spinners = [core for _, _, core, _, name in ps_table(q) if name == "schedprobe"]
        if len(spinners) != 2 or spinners[0] == spinners[1]:
            raise Failure("two unpinned jobs landed on cores %s" % spinners)
        for _ in spinners:
            q.expect(rb"schedprobe: spun \d+ ms", timeout=60)
        reap(2)
        # Sharing a core, two spinners lose whole slices to each other; on
        # separate cores they run side by side. (Host scheduling noise can
        # stretch a gap on a separate core, hence a few attempts.)
        shared = spin_gaps([last, last])
        if min(shared) < 5000:
            raise Failure("two spinners on one core did not time-slice: gaps %s us" % shared)
        for _ in range(3):
            apart = spin_gaps([last - 1, last])
            if max(apart) < 5000:
                break
        else:
            raise Failure("spinners on cores %d and %d still lost slices: gaps %s us"
                          % (last - 1, last, apart))
    yield "ps, placement on the least-loaded core, @N pinning and parallel cores", placement_and_pinning

    def kernel_counts():
        """(live kernel heap allocations, free pages) from the debug console."""
        q.send("\x14")
        q.expect(rb"koraos> ")
        q.send("heap\r")
        m = q.expect(rb"^heap: (\d+) live")
        live = int(m.group(1))
        m = q.expect(rb"^pages: (\d+) of \d+ free")
        q.expect(rb"koraos> ")
        q.send("\x14")
        q.expect(rb"\[serial -> screen terminal")
        return live, int(m.group(1))

    def probe(command, ok, code=0, timeout=60):
        """Run a threadprobe mode in the foreground: its result line must match
        `ok` (else its FAILED line is reported), then it exits with `code`."""
        q.send("threadprobe %s\r" % command)
        m = q.expect(rb"^threadprobe: (?:(" + ok + rb")|FAILED ([^\r\n]*))\r?\n", timeout=timeout)
        if m.group(2) is not None:
            raise Failure("threadprobe %s: %s" % (command, m.group(2).decode(errors="replace")))
        m = q.expect(rb"\] exited with (-?\d+)", timeout=timeout)
        if int(m.group(1)) != code:
            raise Failure("threadprobe %s exited with %s, want %d" % (command, m.group(1).decode(), code))
        q.expect(PROMPT)

    def threads():
        probe("join %d" % cores, rb"joined %d threads on %d cores, results ok" % (2 * cores, cores))
        probe("jobs %d" % cores, rb"8 jobs on %d cores, results ok" % cores)
        probe("malloc %d" % cores, rb"malloc from %d threads ok" % cores, timeout=120)
    yield "threads on every core: create/join, libk jobs, one malloc heap", threads

    def thread_lifetime():
        def both():
            # exit() from main ends threads that spin on every core or block
            # in msleep, a console read and a join.
            probe("exit %d" % cores, rb"exiting with 7", code=7)
            # The program lives on until its last thread is done.
            q.send("threadprobe last\r")
            q.expect(rb"threadprobe: main thread exits")
            q.expect(rb"threadprobe: last thread done")
            m = q.expect(rb"\] exited with (-?\d+)")
            if m.group(1) != b"0":
                raise Failure("threadprobe last exited with %s, want 0" % m.group(1).decode())
            q.expect(PROMPT)
            left = [row for row in ps_table(q) if row[-1] == "threadprobe"]
            if left:
                raise Failure("threadprobe tasks outlived their program: %s" % left)
        both()  # warms every cache
        before = kernel_counts()
        both()
        after = kernel_counts()
        if after != before:
            raise Failure("kernel heap/pages (live, free) %s before, %s after the threaded "
                          "programs ended" % (before, after))
    yield "process lifetime: exit() kills every thread, the last thread ends it, nothing leaks", thread_lifetime

    def kill_job():
        q.send("kill 99999\r")
        q.expect(rb"kill: no such program: 99999")
        q.expect(PROMPT)
        q.send("threadprobe hang %d &\r" % cores)
        # The job's own line can come before or after the shell's.
        pid = None
        for _ in range(2):
            m = q.expect(rb"^\[(\d+)\] started|threadprobe: hanging")
            if m.group(1) is not None:
                pid = int(m.group(1))
        rows = [row for row in ps_table(q, tids=True) if row[-1] == "threadprobe"]
        tids = {row[1] for row in rows}
        if (len(rows) != cores + 1 or {row[0] for row in rows} != {pid} or len(tids) != len(rows)
                or pid not in tids or not set(range(cores)) <= {row[3] for row in rows}):
            raise Failure("ps shows threadprobe as %s, want %d threads of pid %d on cores 0..%d"
                          % (rows, cores + 1, pid, cores - 1))
        # Its threads die on their own cores within a tick or so, and a prompt
        # then reports the job: maybe the one right after kill (on one core
        # the woken threads run before the shell does), maybe a later one.
        q.send("kill %d\r" % pid)
        for _ in range(50):
            m = q.expect(rb"^\[%d\] done, exit (-?\d+)|^\$ " % pid)
            if m.group(1) is not None:
                break
            time.sleep(0.1)
            q.send("\r")
        else:
            raise Failure("killed job %d never finished: %s" % (pid, ps_table(q, tids=True)))
        if m.group(1) != b"-9":
            raise Failure("killed job %d exited with %s, want -9" % (pid, m.group(1).decode()))
        q.expect(PROMPT)
        left = [row for row in ps_table(q) if row[-1] == "threadprobe"]
        if left:
            raise Failure("threadprobe tasks outlived kill: %s" % left)
    yield "kill ends a multi-threaded background job on every core", kill_job

    def jobdemo():
        # One job, then one per core: the renderings must agree (the demo
        # checks), and with several cores the jobs must run in parallel. The
        # thread start and join each wait up to a tick (no IPIs), and the host
        # adds noise, hence a modest bar and a few attempts.
        for attempt in range(3):
            q.send("jobdemo 256\r")
            m = q.expect(rb"^jobdemo: (?:(\d+)x(\d+), 256 iterations: 1 core (\d+) ms, %d cores? "
                         rb"(\d+) ms, speedup (\d+)\.(\d\d)x(, press Enter)?|([^\r\n]*))\r?\n"
                         % cores, timeout=120)
            if m.group(8) is not None:
                raise Failure("jobdemo: %s" % m.group(8).decode(errors="replace"))
            if graphics != (m.group(7) is not None):
                raise Failure("jobdemo %s the picture with graphics %s"
                              % ("kept" if m.group(7) else "did not keep", "on" if graphics else "off"))
            if graphics:
                # The line reaches the serial port before the screen terminal
                # has redrawn its text, and jobdemo puts the picture back only
                # after that: wait for the whole picture, the top-left corner
                # (far outside the set: dark blue, not text) included.
                for _ in range(20):
                    shot = q.screendump("jobdemo")
                    colours = {shot[2][i:i + 3] for i in range(0, len(shot[2]), 3)}
                    r, g, b = pixel(shot, 2, 2)
                    if len(colours) >= 40 and b >= 64 and r < 32 and g < 32:
                        break
                    time.sleep(0.2)
                else:
                    raise Failure("expected the Mandelbrot set on screen: %d colours, top left %s"
                                  % (len(colours), pixel(shot, 2, 2)))
                q.send("\r")
            q.expect(rb"\] exited with 0")
            q.expect(PROMPT)
            speedup = int(m.group(5)) + int(m.group(6)) / 100
            if cores == 1 or speedup >= 1.4:
                break
        else:
            raise Failure("jobdemo on %d cores: speedup only %.2fx" % (cores, speedup))
    yield "jobdemo: Mandelbrot as one job, then one job per core", jobdemo

    def pindemo():
        specs = (["6@1", "2@2", "2@2", "2@2"] if cores >= 3 else
                 ["2@1", "1@0", "1@1"] if cores == 2 else ["2", "1", "1"])
        q.send("pindemo %s\r" % " ".join(specs))
        for _ in specs:
            m = q.expect(rb"^pindemo: (?:worker (\d+) \((\d+) units?, cpu (\d+|\?)( pinned)?\) done|"
                         rb"([^\r\n]*)\r?\n)", timeout=120)
            if m.group(5) is not None:
                raise Failure("pindemo: %s" % m.group(5).decode(errors="replace"))
            spec = specs[int(m.group(1)) - 1]
            if "@" in spec and m.group(3).decode() != spec.split("@")[1]:
                raise Failure("pindemo worker %s ran on cpu %s" % (spec, m.group(3).decode()))
        q.expect(rb"^pindemo: %d workers done in \d+ ms" % len(specs))
        q.expect(rb"\] exited with 0")
        q.expect(PROMPT)
    yield "pindemo: unbalanced sub-processes pinned by hand", pindemo

    if stress:
        def stress_rounds():
            for _ in range(stress):
                for check in (threads, thread_lifetime, kill_job, jobdemo, pindemo):
                    check()
        yield "stress: %d rounds of threads, process lifetime, kill and both demos" % stress, stress_rounds

    if repeat:
        def allocation_lifetime():
            baseline = None
            for run in range(repeat):
                q.send("allocprobe\r")
                q.expect(rb"allocprobe: heap checked", timeout=60)
                q.expect(rb"exited with 0")
                q.expect(PROMPT)
                # After the first run every cache is warm: from then on each
                # reaped probe, including what it leaked on purpose, must give
                # back exactly what it took.
                if run == 0 or run == repeat - 1:
                    counts = kernel_counts()
                    if baseline is None:
                        baseline = counts
                    elif counts != baseline:
                        raise Failure("kernel heap/pages (live, free) %s after the first run, "
                                      "%s after %d runs" % (baseline, counts, repeat))
        yield "repeated EL0 page runs, malloc heap and nested-process lifetime (%d runs)" % repeat, allocation_lifetime

    if keyboard:
        def keyboard_input():
            # Typed entirely through the virtual device, including Shift,
            # release transitions, editing and Enter. UART is only the observer.
            for key in ("e", "c", "h", "o", "spc", "shift-a", "b", "c", "backspace", "ret"):
                reply = q.monitor("sendkey " + key + " 10")
                if b"Error" in reply or b"unknown" in reply.lower():
                    raise Failure("QEMU key injection failed: %r" % reply)
                time.sleep(0.02)
            q.expect(rb"^Ab\r?$")
            q.expect(rb"exited with 1")
            q.expect(PROMPT)
            q.monitor("sendkey shift-pgup 10")
            time.sleep(0.02)
            q.monitor("sendkey shift-pgdn 10")
        yield "VirtIO keyboard handles Shift, release, editing and Enter", keyboard_input

    if not graphics:
        return

    def termdemo():
        q.send("termdemo\r")
        q.expect(rb"Shift\+PgUp")
        q.expect(PROMPT)
        shot = q.screendump("termdemo")
        colours = {shot[2][i:i + 3] for i in range(0, len(shot[2]), 3)}
        if len(colours) < 200:
            raise Failure("expected the 256/24-bit colour tables on screen, "
                          "found only %d distinct colours" % len(colours))
    yield "termdemo renders colours on the screen", termdemo

    def gfxdemo():
        q.send("gfxdemo\r")
        q.expect(rb"gfxdemo: painted (\d+)x(\d+)")
        q.expect(PROMPT)
        shot = q.screendump("gfxdemo")
        width, height, _ = shot
        # gfxdemo paints red rising with x and green rising with y. Sample
        # below the rows the shell prints on.
        for x, y in ((8, 700), (width - 8, 700), (width - 8, 300)):
            expected = ((x * 255) // width, (y * 255) // height, 0)
            if not near(pixel(shot, x, y), expected):
                raise Failure("pixel (%d,%d) = %s, expected about %s"
                              % (x, y, pixel(shot, x, y), expected))
    yield "gfxdemo paints the framebuffer", gfxdemo


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--target", choices=("qemu_raspi3b", "qemu_raspi4b", "qemu_virt"))
    parser.add_argument("--machine", choices=("raspi3b", "virt"), help="deprecated target alias")
    parser.add_argument("--build-dir", default=os.environ.get("BUILD_DIR", os.path.join(ROOT, "build")))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--release", action="store_true")
    mode.add_argument("--debug", action="store_true")
    parser.add_argument("--ram", default="256M")
    parser.add_argument("--el2", action="store_true", help="enter virt via EL2 before dropping to EL1")
    parser.add_argument("--gic", type=int, choices=(2, 3), default=2,
                        help="virt interrupt controller version (hvf needs 3)")
    parser.add_argument("--accel", choices=("tcg", "hvf"), default="tcg",
                        help="virt on an Apple Silicon Mac: hvf runs on the host's cores")
    parser.add_argument("--smp", type=int, default=1,
                        help="virt cores (raspi3b and raspi4b always have 4); all must come online")
    parser.add_argument("--repeat", type=int, default=0, help="repeat the EL0 page/malloc/nested-process probe")
    parser.add_argument("--stress", type=int, default=0,
                        help="rounds of the thread, kill and demo steps to repeat at the end")
    parser.add_argument("--expect-root-failure", action="store_true", help="require a configured disk mount failure")
    parser.add_argument("--keyboard", action="store_true", help="inject keys through a VirtIO keyboard")
    parser.add_argument("--disk", help="external bare FAT32 or MBR FAT32 disk for virt")
    parser.add_argument("--multi-volume", action="store_true", help="require scratch BOOT/EXTRAS partitions")
    parser.add_argument("--write-test", action="store_true", help="mutate a disposable writable FAT32 root")
    parser.add_argument("--disk-writable", action="store_true", help="enable raw writes to the supplied test disk")
    parser.add_argument("--no-graphics", action="store_true")
    parser.add_argument("--sd", help="raspi3b/raspi4b: SD card image (tests/create-sd-card-image.py)")
    parser.add_argument("--sd-writable", action="store_true",
                        help="attach the card writable; the kernel must be built --storage-writable")
    parser.add_argument("--usb-stick", help="raspi3b: USB drive image (tests/create-sd-card-image.py --label USBSTICK)")
    parser.add_argument("--usb-stick-writable", action="store_true",
                        help="attach the USB drive writable; the kernel must be built --storage-writable")
    parser.add_argument("--kernel")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--out")
    args = parser.parse_args()
    if args.machine:
        alias = "qemu_virt" if args.machine == "virt" else "qemu_raspi3b"
        if args.target and args.target != alias:
            parser.error("--target conflicts with --machine")
        args.target = alias
        print("warning: --machine is deprecated; use --target " + alias, file=sys.stderr)
    args.target = args.target or "qemu_raspi3b"
    args.machine = args.target[len("qemu_"):]
    kernel_dir = os.path.join(os.path.abspath(args.build_dir),
                              "release" if args.release else "debug", args.target)
    args.out = args.out or os.path.join(kernel_dir, "qemu-smoke")
    if args.stress < 0 or args.stress > 100:
        parser.error("--stress must be between 0 and 100")
    if args.repeat < 0 or args.repeat > 2000:
        parser.error("--repeat must be between 0 and 2000")
    if args.el2 and args.machine != "virt":
        parser.error("--el2 requires virt")
    if args.write_test and (args.machine != "virt" or not args.disk or not args.disk_writable):
        parser.error("--write-test requires virt, --disk and --disk-writable")
    if args.multi_volume and not args.disk:
        parser.error("--multi-volume requires --disk")
    if args.expect_root_failure and not args.disk:
        parser.error("--expect-root-failure requires --disk")

    if args.kernel is None:
        args.kernel = os.path.join(kernel_dir, "kernel.img")
    if not os.path.exists(args.kernel):
        sys.exit("no kernel at %s -- build it with: ./build.sh --target %s%s"
                 % (args.kernel, args.target, " --release" if args.release else ""))
    os.makedirs(args.out, exist_ok=True)

    extra = ["-device", "ramfb"] if args.machine == "virt" and not args.no_graphics else []
    if args.sd:
        if not args.machine.startswith("raspi") or not os.path.isfile(args.sd):
            parser.error("--sd requires raspi3b or raspi4b and an existing card image")
        # Read-only runs never touch the image (QEMU's SD card cannot be read-only).
        drive = "format=raw,file=%s%s" % (os.path.abspath(args.sd).replace(",", ",,"),
                                          "" if args.sd_writable else ",snapshot=on")
        if args.machine == "raspi3b":
            extra += ["-drive", "if=sd," + drive]
        else:
            # The Pi 4's slot is on EMMC2, but QEMU puts if=sd cards on the
            # Arasan controller: attach it to EMMC2's bus, the first SDHCI.
            extra += ["-drive", "if=none,id=sdcard," + drive,
                      "-device", "sd-card,drive=sdcard,bus=main-system-bus/generic-sdhci/sd-bus"]
    elif args.sd_writable:
        parser.error("--sd-writable requires --sd")
    if args.usb_stick:
        # QEMU's raspi4b has no PCIe, so no xHCI: USB is raspi3b only.
        if args.machine != "raspi3b" or not os.path.isfile(args.usb_stick):
            parser.error("--usb-stick requires raspi3b and an existing drive image")
        extra += ["-drive", "if=none,id=stick,format=raw,file=%s%s" %
                  (os.path.abspath(args.usb_stick).replace(",", ",,"),
                   "" if args.usb_stick_writable else ",snapshot=on"),
                  "-device", "usb-storage,drive=stick",
                  # A keyboard too, so its interrupt transfers (polled every
                  # frame on core 0) run beside the drive's bulk ones.
                  "-device", "usb-kbd"]
    elif args.usb_stick_writable:
        parser.error("--usb-stick-writable requires --usb-stick")
    if args.keyboard:
        if args.machine != "virt":
            parser.error("--keyboard requires virt")
        extra += ["-device", "virtio-keyboard-device"]
    if args.disk:
        if args.machine != "virt" or not os.path.isfile(args.disk):
            parser.error("--disk requires virt and an existing image")
        extra += ["-drive", "if=none,id=root,format=raw,file=%s,readonly=%s" %
                  (os.path.abspath(args.disk), "off" if args.disk_writable else "on"),
                  "-device", "virtio-blk-device,drive=root"]
    if args.smp < 1 or args.smp > 4:
        parser.error("--smp must be between 1 and 4 (KoraOS manages up to 4 cores)")
    if args.smp != 1 and args.machine != "virt":
        parser.error("--smp applies to virt (raspi3b and raspi4b always have 4 cores)")
    if (args.gic != 2 or args.accel != "tcg") and args.machine != "virt":
        parser.error("--gic and --accel apply to virt")
    if args.accel == "hvf" and (args.gic != 3 or args.el2):
        parser.error("--accel hvf needs --gic 3 and no --el2 (hvf has no EL2)")
    cores = 4 if args.machine.startswith("raspi") else args.smp
    q = Qemu(args.qemu, os.path.abspath(args.kernel), os.path.abspath(args.out), args.machine, args.ram,
             extra, args.el2, args.smp, args.gic, args.accel)
    failed = False
    try:
        try:
            # The first line names the build (git commit, "-dirty" for local
            # changes), so a log always shows which kernel ran.
            m = q.expect(rb"^KoraOS \d+\.\d+ \(([0-9a-zA-Z.+-]+)\)\r?\n", timeout=90)
            build = m.group(1).decode()
            if build == "unknown" and os.path.isdir(os.path.join(ROOT, ".git")):
                raise Failure("boot line has no build identifier in a git checkout")
            print("ok   - boot line names the build (%s)" % build)
            # The kernel heap checks itself before any later bring-up step
            # uses it; it is also the first sign the kernel booted at all.
            q.expect(rb"^\[heap\] self-test ok, \d+ of \d+ pages free", timeout=90)
            m = q.expect(rb"^smp: (\d+) cores? online", timeout=30)
            if int(m.group(1)) != cores:
                raise Failure("%s of %d cores came online" % (m.group(1).decode(), cores))
            print("ok   - %d core%s online" % (cores, "s" if cores > 1 else ""))
        except Failure as e:
            print("FAIL - kernel boots (heap self-test): %s" % e)
            print("qemu smoke: FAILED (artifacts in %s)" % args.out)
            sys.exit(1)  # QEMU is still stopped by the finally below
        if args.disk:
            q.expect(rb"\[blkdev\] using VirtIO disk")
        if args.usb_stick:
            # Circle enumerates before the block layer comes up.
            m = q.expect(rb"^\[usb\] drive umsd1: (\d+) MB, (read-only|writable)")
            q.expect(rb"^circle: USB keyboard ready")
            want = (str(os.path.getsize(args.usb_stick) // (1024 * 1024)).encode(),
                    b"writable" if args.usb_stick_writable else b"read-only")
            if m.groups() != want:
                raise Failure("USB drive line %r, want %s" % (m.group(0), want))
            print("ok   - USB drive: %s MB, %s" % (m.group(1).decode(), m.group(2).decode()))
        if args.machine.startswith("raspi"):
            # The SD slot: a card announces itself, an empty slot says so.
            m = q.expect(rb"^\[sd\] (no card|(SDSC|SDHC/SDXC) card, (\d+) MB, (read-only|writable))")
            if args.sd is None and m.group(1) != b"no card":
                raise Failure("an SD card showed up without one attached: %s" % m.group(1).decode())
            if args.sd is not None:
                mb = os.path.getsize(args.sd) // (1024 * 1024)
                want = (b"SDHC/SDXC" if mb > 2048 else b"SDSC", str(mb).encode(),
                        b"writable" if args.sd_writable else b"read-only")
                if (m.group(2), m.group(3), m.group(4)) != want:
                    raise Failure("SD card line %r, want %s" % (m.group(1), want))
            print("ok   - SD slot: %s" % m.group(1).decode())
        if args.expect_root_failure:
            q.expect(rb"fat32: mount failed:")
            q.expect(rb"kernel: failed to load /bin/init")
            print("ok   - configured invalid disk fails without ramdisk fallback")
            return
        if args.keyboard:
            q.expect(rb"\[virtio-input\] keyboard ready")
        media = ([("SD card", "SDCARD", args.sd_writable)] if args.sd else []) + \
            ([("USB drive", "USBSTICK", args.usb_stick_writable)] if args.usb_stick else [])
        for name, check in run(q, not args.no_graphics, args.keyboard, args.repeat, args.multi_volume,
                               args.write_test, cores, args.stress, media):
            try:
                check()
                print("ok   - " + name)
            except Failure as e:
                print("FAIL - %s: %s" % (name, e))
                failed = True
                break  # later steps depend on the shell's state
    finally:
        q.close()
    print("qemu smoke: %s (artifacts in %s)" % ("FAILED" if failed else "all passed", args.out))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
