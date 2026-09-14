#!/usr/bin/env python3
"""Boot the Lumen ISO in QEMU headless, drive it over serial, inspect it via QMP.

Used by phase checks and the integration harness to prove the kernel actually
ran (RIP inside the kernel image, expected serial lines present) rather than
trusting silence.

Usage: qemu-probe.py <iso> [--uefi OVMF_CODE.fd] [--wait SECONDS] [--smp N]
                     [--screenshot out.png] [--hmp "info mtree"] [--expect FILE]
                     [--quiet]

--expect FILE   every non-empty, non-# line of FILE must appear (as a substring,
                in order) in the serial output. Two directives are allowed:
                  !send <text>   type <text> + Enter into the serial console
                  !wait <secs>   pause before continuing
                Directives execute after the initial --wait, in file order.
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

def load_expect(path):
    """Returns a list of ("send"|"key", text) / ("wait", secs) / ("expect", text)."""
    steps = []
    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.startswith("!send "):
            steps.append(("send", line[6:]))
        elif line.startswith("!key "):
            steps.append(("key", line[5:]))
        elif line.startswith("!wait "):
            steps.append(("wait", float(line[6:])))
        else:
            steps.append(("expect", line))
    return steps

# QEMU qcode names for the characters "!key" can type (plus Enter at the end).
QCODE = {" ": "spc", "-": "minus", "=": "equal", ".": "dot", ",": "comma", "/": "slash",
         ";": "semicolon", "'": "apostrophe", "[": "bracket_left", "]": "bracket_right",
         "\\": "backslash", "`": "grave_accent"}

def type_keys(q, text):
    """Types text through the emulated PS/2 keyboard, then presses Enter."""
    for ch in text:
        if ch.isalnum():
            name = ch.lower()
        elif ch in QCODE:
            name = QCODE[ch]
        else:
            continue
        keys = [{"type": "qcode", "data": name}]
        if ch.isupper():
            keys.insert(0, {"type": "qcode", "data": "shift"})
        q.cmd("send-key", keys=keys)
        time.sleep(0.05)
    q.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--uefi", metavar="OVMF_CODE")
    ap.add_argument("--no-hpet", action="store_true", help="boot without an HPET (tests the PIT path)")
    ap.add_argument("--wait", type=float, default=8.0)
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--screenshot", metavar="FILE.png")
    ap.add_argument("--hmp", action="append", default=[], metavar="CMD")
    ap.add_argument("--expect", metavar="FILE")
    ap.add_argument("--quiet", action="store_true", help="only print the verdict lines")
    a = ap.parse_args()

    steps = load_expect(a.expect) if a.expect else []

    tmp = tempfile.mkdtemp(prefix="lumen-qmp-")
    sock = os.path.join(tmp, "qmp.sock")
    machine = "q35,hpet=off" if a.no_hpet else "q35"
    # KVM when available (nested virtualisation inside WSL2 works): faster tests
    # and hardware-accurate timers. LUMEN_QEMU_ACCEL=tcg forces emulation.
    if os.environ.get("LUMEN_QEMU_ACCEL", "kvm") == "kvm" and os.access("/dev/kvm", os.R_OK | os.W_OK):
        accel = ["-accel", "kvm", "-cpu", "host"]
    else:
        accel = ["-cpu", "qemu64,+pdpe1gb"]
    cmd = ["qemu-system-x86_64", "-machine", machine] + accel + [
           "-smp", str(a.smp), "-m", "512M", "-cdrom", a.iso, "-boot", "d",
           "-serial", "stdio", "-display", "none", "-d", "guest_errors",
           "-no-reboot", "-no-shutdown", "-qmp", f"unix:{sock},server,nowait"]
    if a.uefi:
        cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={a.uefi}"]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    time.sleep(a.wait)
    q = Qmp(sock)
    for kind, arg in steps:
        if kind == "send":
            proc.stdin.write((arg + "\n").encode()); proc.stdin.flush()
            time.sleep(0.3)
        elif kind == "key":
            type_keys(q, arg)
            time.sleep(0.3)
        elif kind == "wait":
            time.sleep(arg)

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
        for kind, want in steps:
            if kind != "expect":
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
