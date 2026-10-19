#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""QEMU smoke test: boot the QEMU kernel on the raspi3b machine, drive the
shell over the serial line, and check the output and the framebuffer.

Waits for expected output (with timeouts) rather than sleeping, so it is not
sensitive to how fast the machine runs QEMU. Standard library only.

Usage: tests/run-qemu-smoke.py --target qemu_raspi3b|qemu_virt [--out DIR]
Build the kernel first with: ./build.sh --target qemu_raspi3b

Artifacts in --out (default build/qemu-smoke): serial.log and one PNG
screenshot per screen check.
"""

import argparse
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
                 smp=1):
        self.outdir = outdir
        self.log = open(os.path.join(outdir, "serial.log"), "wb")
        self.pending = b""  # output not yet consumed by expect()
        # The monitor socket path is relative to QEMU's working directory:
        # UNIX socket paths are limited to ~104 bytes and temp dirs are long.
        machine_args = ["-M", "raspi3b"] if machine == "raspi3b" else [
            "-M", "virt,gic-version=2,highmem=off" + (",virtualization=on" if el2 else ""), "-cpu", "cortex-a72",
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


def run(q, graphics=True, keyboard=False, repeat=0, multi_volume=False, write_test=False, cores=1):
    """Yield (name, check) steps; each check raises Failure on error."""

    def boot():
        q.expect(rb"KoraOS shell\. Type 'help'\.", timeout=90)
        q.expect(PROMPT)
    yield "boots to the shell", boot

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
            q.expect(rb"^controller: (?:BCM2835 legacy|GICv2 \(virt\))")
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
        """`tasks` with the forbid flag and BKL holder: ({name: (state, stack,
        forbidden)}, peak, size, holder pid or None)."""
        q.send("\x14")
        q.expect(rb"koraos> ")
        q.send("tasks\r")
        listed = {}
        while True:
            m = q.expect(rb"^(?:\s+(\d+)\s+(\w+)\s+(\d+)\s+(\S+)( \(forbid\))?|kernel stack peak: (\d+) of (\d+) bytes)\r?\n")
            if m.group(6):
                peak, size = int(m.group(6)), int(m.group(7))
                break
            listed[m.group(4).decode()] = (m.group(2).decode(), int(m.group(3)), m.group(5) is not None)
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

        # A background job that never makes a syscall: the shell only answers
        # while it runs if the timer tick preempts it.
        q.send("schedprobe spin 5000 &\r")
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
        q.send("schedprobe spin 8000 &\r")
        m = q.expect(rb"^\[(\d+)\] started")
        spinner = m.group(1)
        q.expect(PROMPT)
        results = []
        for _ in range(3):
            q.send("forbidprobe\r")
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
        _, _, _, holder = read_tasks_bkl()
        if holder is not None:
            raise Failure("the BKL is still held (by pid %s) with the system idle" % holder)
    yield "forbid()/permit(): nesting, no preemption, BKL held across EL0", forbid_permit

    if repeat:
        def kernel_counts():
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
    parser.add_argument("--target", choices=("qemu_raspi3b", "qemu_virt"))
    parser.add_argument("--machine", choices=("raspi3b", "virt"), help="deprecated target alias")
    parser.add_argument("--build-dir", default=os.environ.get("BUILD_DIR", os.path.join(ROOT, "build")))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--release", action="store_true")
    mode.add_argument("--debug", action="store_true")
    parser.add_argument("--ram", default="256M")
    parser.add_argument("--el2", action="store_true", help="enter virt via EL2 before dropping to EL1")
    parser.add_argument("--smp", type=int, default=1,
                        help="virt cores (raspi3b always has 4); all must come online")
    parser.add_argument("--repeat", type=int, default=0, help="repeat the EL0 page/malloc/nested-process probe")
    parser.add_argument("--expect-root-failure", action="store_true", help="require a configured disk mount failure")
    parser.add_argument("--keyboard", action="store_true", help="inject keys through a VirtIO keyboard")
    parser.add_argument("--disk", help="external bare FAT32 or MBR FAT32 disk for virt")
    parser.add_argument("--multi-volume", action="store_true", help="require scratch BOOT/EXTRAS partitions")
    parser.add_argument("--write-test", action="store_true", help="mutate a disposable writable FAT32 root")
    parser.add_argument("--disk-writable", action="store_true", help="enable raw writes to the supplied test disk")
    parser.add_argument("--no-graphics", action="store_true")
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
    args.machine = "virt" if args.target == "qemu_virt" else "raspi3b"
    kernel_dir = os.path.join(os.path.abspath(args.build_dir),
                              "release" if args.release else "debug", args.target)
    args.out = args.out or os.path.join(kernel_dir, "qemu-smoke")
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
        parser.error("--smp applies to virt (raspi3b always has 4 cores)")
    cores = 4 if args.machine == "raspi3b" else args.smp
    q = Qemu(args.qemu, os.path.abspath(args.kernel), os.path.abspath(args.out), args.machine, args.ram,
             extra, args.el2, args.smp)
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
        if args.expect_root_failure:
            q.expect(rb"fat32: mount failed:")
            q.expect(rb"kernel: failed to load /bin/init")
            print("ok   - configured invalid disk fails without ramdisk fallback")
            return
        if args.keyboard:
            q.expect(rb"\[virtio-input\] keyboard ready")
        for name, check in run(q, not args.no_graphics, args.keyboard, args.repeat, args.multi_volume,
                               args.write_test, cores):
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
