"""Drive a kiln guest under QEMU over its serial console."""

import re
import subprocess
import threading
import time

PROMPT = re.compile(rb"kiln:[^\n$]*\$ $")


class Machine:
    def __init__(self, qemu, kernel, image, cpus=4, mem="128M", bootargs="", log=None):
        cmd = [
            qemu, "-machine", "virt", "-bios", "default", "-smp", str(cpus), "-m", mem,
            "-nographic", "-no-reboot", "-kernel", kernel,
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={image},if=none,format=raw,id=hd0",
            "-device", "virtio-blk-device,drive=hd0,bus=virtio-mmio-bus.0",
            "-append", bootargs,
        ]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.buf = b""
        self.pos = 0
        self.cond = threading.Condition()
        self.log = log
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        while True:
            chunk = self.proc.stdout.read1(4096)
            with self.cond:
                if not chunk:
                    self.cond.notify_all()
                    return
                self.buf += chunk
                if self.log:
                    self.log.write(chunk)
                    self.log.flush()
                self.cond.notify_all()

    def expect(self, pattern, timeout=30):
        """Wait for pattern (bytes regex) after the current position; return the
        text before the match and move past it."""
        if isinstance(pattern, str):
            pattern = re.compile(re.escape(pattern.encode()))
        deadline = time.time() + timeout
        with self.cond:
            while True:
                m = pattern.search(self.buf, self.pos)
                if m:
                    before = self.buf[self.pos:m.start()]
                    self.pos = m.end()
                    return before.decode(errors="replace"), m
                left = deadline - time.time()
                if left <= 0 or (self.proc.poll() is not None and not self.reader.is_alive()):
                    tail = self.buf[self.pos:][-2000:].decode(errors="replace")
                    raise TimeoutError(f"timed out waiting for {pattern.pattern!r}; got:\n{tail}")
                self.cond.wait(min(left, 0.5))

    def send(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def boot(self, timeout=60):
        self.expect(PROMPT, timeout)

    def run(self, command, timeout=60):
        """Type a command at the prompt and return its output (without the echo)."""
        self.send(command + "\n")
        self.expect(command + "\n", 10)   # the console echoes what we type
        out, _ = self.expect(PROMPT, timeout)
        return out

    def wait_exit(self, timeout=30):
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
