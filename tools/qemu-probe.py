#!/usr/bin/env python3
"""Boot the Lumen ISO in QEMU headless, wait, then inspect the guest through QMP.

Used by phase checks and the integration harness to prove the kernel actually
ran (RIP inside the kernel image, expected serial lines present) rather than
trusting silence.

Usage: qemu-probe.py <iso> [--uefi OVMF_CODE.fd] [--wait SECONDS] [--smp N]
                     [--screenshot out.ppm] [--hmp "info mtree"] [--expect FILE]
                     [--quiet]

--expect FILE   every non-empty, non-# line of FILE must appear (as a substring,
                in order) in the serial output.
Exit status: 0 if RIP is in the kernel's higher half and all expectations hold.
"""
import argparse, json, os, re, socket, subprocess, sys, tempfile, time

KERNEL_BASE = 0xFFFFFFFF80000000
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.f = self.s.makefile("rwb", buffering=0)
        self.f.readline()                               # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write((json.dumps({"execute": name, "arguments": args}) + "\n").encode())
        while True:
            reply = json.loads(self.f.readline())
            if "return" in reply or "error" in reply:  # skip async events
                return reply.get("return", reply.get("error"))

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--uefi", metavar="OVMF_CODE")
    ap.add_argument("--wait", type=float, default=8.0)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--screenshot", metavar="FILE.ppm")
    ap.add_argument("--hmp", action="append", default=[], metavar="CMD")
    ap.add_argument("--expect", metavar="FILE")
    ap.add_argument("--quiet", action="store_true", help="only print the verdict lines")
    a = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="lumen-qmp-")
    sock = os.path.join(tmp, "qmp.sock")
    cmd = ["qemu-system-x86_64", "-machine", "q35", "-cpu", "qemu64,+pdpe1gb",
           "-smp", str(a.smp), "-m", "512M", "-cdrom", a.iso, "-boot", "d",
           "-serial", "stdio", "-display", "none", "-d", "guest_errors",
           "-no-reboot", "-no-shutdown", "-qmp", f"unix:{sock},server,nowait"]
    if a.uefi:
        cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={a.uefi}"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(a.wait)

    q = Qmp(sock)
    regs = q.hmp("info registers")
    extra = [(h, q.hmp(h)) for h in a.hmp]
    if a.screenshot:
        q.cmd("screendump", filename=os.path.abspath(a.screenshot), format="png")
        time.sleep(0.5)
    q.cmd("quit")
    try:
        out = proc.communicate(timeout=5)[0].decode(errors="replace")
    except subprocess.TimeoutExpired:
        proc.kill(); out = proc.communicate()[0].decode(errors="replace")

    # OVMF probes TPM/flash addresses that -d guest_errors reports; drop that noise.
    out = "\n".join(l for l in out.splitlines() if not l.startswith("Invalid "))
    clean = ANSI.sub("", out)

    if not a.quiet:
        print("---- serial ----"); print(clean.rstrip())
        for name, text in extra:
            print(f"---- {name} ----"); print(text.rstrip())
        print("---- cpu ----")
    rip = None
    for line in regs.splitlines():
        if line.startswith("RIP="):
            rip = int(line.split()[0][4:], 16)
            print(line.strip()); break
    ok = rip is not None and rip >= KERNEL_BASE
    print("kernel entered: " + ("YES (RIP in higher half)" if ok else f"NO (RIP={rip:#x})" if rip else "NO"))

    if a.expect:
        pos = 0
        for raw in open(a.expect, encoding="utf-8"):
            want = raw.rstrip("\n")
            if not want.strip() or want.lstrip().startswith("#"):
                continue
            idx = clean.find(want, pos)
            if idx < 0:
                print(f"expect FAILED: {want!r} not found (after offset {pos})")
                ok = False
                break
            pos = idx + len(want)
        else:
            print(f"expect OK: {a.expect}")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
