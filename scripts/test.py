#!/usr/bin/env python3
"""System tests for kiln.

Boots the kernel under QEMU on a scratch copy of the disk image and checks:
  1. usertests, the in-guest kernel test suite;
  2. the shell: pipes, redirection, globbing, job control, Ctrl-C;
  3. files written by kiln, read back on the host with e2fsprogs (debugfs),
     and files written on the host, read back by kiln after a reboot;
  4. e2fsck finds the image clean after every boot.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from qemu import Machine  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failures = []


def check(name, ok, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    if not ok:
        failures.append(name)
        if detail:
            print("       " + detail.replace("\n", "\n       "))


def run_shell_checks(m):
    def same(cmd, want, timeout=60):
        got = m.run(cmd, timeout).replace("\r", "")
        check(f"$ {cmd}", got == want, f"want {want!r}\ngot  {got!r}")

    def like(cmd, pattern, timeout=60):
        got = m.run(cmd, timeout).replace("\r", "")
        check(f"$ {cmd}", re.search(pattern, got, re.M) is not None,
              f"want /{pattern}/\ngot  {got!r}")

    same("echo hello world", "hello world\n")
    same("echo a b | wc", "      1       2       4\n")
    same("seq 1 12 | grep 1 | sort -rn | head -n 2", "12\n11\n")
    same("ls /bin | grep -c sh", "2\n")                      # sh, shutdown
    same("false || echo recovered", "recovered\n")
    same("true && echo chained", "chained\n")
    same("false && echo never; echo $?", "1\n")
    same("cd /tmp; pwd; cd /", "/tmp\n")
    same("echo 'single $? quoted' \"double $?\" back\\ slash", "single $? quoted double 0 back slash\n")
    same("echo redirected > /tmp/r.txt; cat /tmp/r.txt", "redirected\n")
    same("echo more >> /tmp/r.txt; wc /tmp/r.txt", "      2       2      16 /tmp/r.txt\n")
    same("cat < /tmp/r.txt | tail -n 1", "more\n")
    same("ls /missing 2> /tmp/err.txt; cat /tmp/err.txt",
         "ls: /missing: No such file or directory\n")
    same("ls /missing 2>&1 | wc -l", "      1\n")
    same("nosuchcmd", "sh: nosuchcmd: command not found\n")
    same("nosuchcmd; echo $?", "sh: nosuchcmd: command not found\n127\n")
    same("primes 40 | wc -l", "     12\n")
    same("ls /home/*.txt", "/home/hello.txt\n")
    same("echo /bin/s?", "/bin/sh\n")
    same("echo nomatch*", "nomatch*\n")
    same("mkdir -p /tmp/a/b/c && echo deep > /tmp/a/b/c/f && cat /tmp/a/b/c/f", "deep\n")
    same("rm -r /tmp/a; ls /tmp/a", "ls: /tmp/a: No such file or directory\n")
    same("yes kiln | head -n 3", "kiln\nkiln\nkiln\n")      # yes dies of SIGPIPE quietly
    like("ps", r"^\s+1\s+0\s+1 S\s+-\s+\d+\s+[\d:]+ init$")
    like("uname -a", r"^kiln 0\.1 riscv64")
    like("date", rf"UTC {time.gmtime().tm_year}$")
    like("free", r"^memory:\s+\d+ KiB total")
    like("df", r"^/dev/vda\s+65\d+ ")

    # Background jobs and job reporting.
    like("sleep 1 &", r"^\[\d+\]$")
    like("jobs", r"^\[\d+\] running$")
    same("wait; jobs", "")

    # Ctrl-C kills the foreground job but not the shell.
    m.send("sleep 30\n")
    m.expect("sleep 30\n", 10)
    time.sleep(0.5)
    t0 = time.time()
    m.send("\x03")
    m.expect("^C\n", 5)
    m.boot(10)
    check("Ctrl-C interrupts a foreground job", time.time() - t0 < 5)
    same("echo $?", "130\n")
    # Ctrl-C at an empty prompt is harmless.
    m.send("\x03")
    m.expect("^C\n", 5)
    m.send("\n")
    m.boot(10)
    same("echo still here", "still here\n")

    # Crash reporting: the shell names the signal.
    same("sh -c 'kill -SEGV $$'", "Segmentation fault\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default="qemu-system-riscv64")
    ap.add_argument("--e2fsck", default="e2fsck")
    ap.add_argument("--debugfs", default="debugfs")
    ap.add_argument("--cpus", type=int, default=4)
    ap.add_argument("--kernel", default=os.path.join(ROOT, "build", "kernel.bin"))
    ap.add_argument("--image", default=os.path.join(ROOT, "build", "fs.img"))
    ap.add_argument("--skip-usertests", action="store_true")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="kiln-test-")
    img = os.path.join(work, "fs.img")
    shutil.copyfile(args.image, img)
    log = open(os.path.join(work, "console.log"), "wb")
    print(f"scratch image: {img}\nconsole log:   {log.name}")

    def boot():
        m = Machine(args.qemu, args.kernel, img, cpus=args.cpus,
                    bootargs="kiln.quiet_faults", log=log)
        m.boot(90)
        return m

    def poweroff(m, name):
        m.send("shutdown\n")
        try:
            m.expect("kiln: powering off", 30)
            code = m.wait_exit(30)
            check(f"{name}: clean power-off", code == 0, f"qemu exit code {code}")
        except Exception as e:  # noqa: BLE001
            check(f"{name}: clean power-off", False, str(e))
            m.kill()

    def fsck(name):
        r = subprocess.run([args.e2fsck, "-fn", img], capture_output=True, text=True)
        check(f"{name}: e2fsck -fn reports a clean file system", r.returncode == 0,
              r.stdout + r.stderr)

    def debugfs(cmd, write=False):
        argv = [args.debugfs] + (["-w"] if write else []) + ["-R", cmd, img]
        return subprocess.run(argv, capture_output=True).stdout

    # ---- boot 1: the test suites, then leave data behind ----
    print("boot 1")
    m = boot()
    try:
        if not args.skip_usertests:
            out = m.run("usertests", timeout=900)
            for line in out.splitlines():
                if line.startswith("test "):
                    print("    " + line.strip())
            check("usertests", re.search(r"ALL \d+ TESTS PASSED", out) is not None,
                  out[-1500:])
        run_shell_checks(m)

        # The kernel image itself sits in the root FS (big enough to need
        # double-indirect blocks); kiln's CRC must match the host's.
        with open(os.path.join(ROOT, "build", "root", "usr", "share", "kernel.elf"), "rb") as f:
            data = f.read()
        want = f"{zlib.crc32(data):08x} {len(data)} /usr/share/kernel.elf\n"
        got = m.run("crc32 /usr/share/kernel.elf")
        check("kiln reads a host-written multi-indirect file", got == want, f"{want!r} vs {got!r}")

        m.run("echo persistent-data-123 > /home/persist.txt")
        m.run("yes kiln-wrote-this | head -n 20000 > /home/big.txt")
        m.run("mkdir /home/made-by-kiln && ln /home/persist.txt /home/made-by-kiln/hardlink")
        m.run("mv /home/hello.txt /home/made-by-kiln/renamed.txt")
    finally:
        poweroff(m, "boot 1")
    fsck("boot 1")

    # ---- host side: read kiln's files with e2fsprogs, add one of our own ----
    big = debugfs("cat /home/big.txt")
    check("debugfs reads a 380 KB file kiln wrote", big == b"kiln-wrote-this\n" * 20000,
          f"{len(big)} bytes")
    check("debugfs sees kiln's hard link",
          debugfs("cat /home/made-by-kiln/hardlink") == b"persistent-data-123\n")
    stat = debugfs("stat /home/persist.txt").decode(errors="replace")
    check("hard link count is 2 on disk", re.search(r"Links: 2\b", stat) is not None, stat[:300])
    check("rename is visible on disk", b"ext2 file system" in debugfs("cat /home/made-by-kiln/renamed.txt"))

    host_file = os.path.join(work, "from-host.txt")
    payload = b"".join(b"line %05d written by debugfs on the host\n" % i for i in range(3000))
    with open(host_file, "wb") as f:
        f.write(payload)
    debugfs(f"write {host_file} /home/from-host.txt", write=True)
    fsck("after host write")

    # ---- boot 2: everything survived, and kiln reads the host's file ----
    print("boot 2")
    m = boot()
    try:
        same = lambda cmd, want: check(f"$ {cmd}", m.run(cmd) == want,  # noqa: E731
                                       f"want {want!r}")
        same("cat /home/persist.txt", "persistent-data-123\n")
        same("wc -l /home/big.txt", "  20000 /home/big.txt\n")
        same("crc32 /home/from-host.txt",
             f"{zlib.crc32(payload):08x} {len(payload)} /home/from-host.txt\n")
        same("rm /home/big.txt /home/made-by-kiln/hardlink && cat /home/persist.txt",
             "persistent-data-123\n")
        same("rm -r /home/made-by-kiln; ls /home",
             "from-host.txt\npersist.txt\n")
    finally:
        poweroff(m, "boot 2")
    fsck("boot 2")
    check("deleted files are gone on disk", debugfs("cat /home/big.txt") == b"")

    log.close()
    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED: " + ", ".join(failures))
        with open(log.name, "rb") as f:
            tail = f.read().decode(errors="replace").splitlines()[-80:]
        print("---- last lines of the console ----")
        print("\n".join(tail))
        print(f"console log: {log.name}")
        return 1
    print("all system tests passed")
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
