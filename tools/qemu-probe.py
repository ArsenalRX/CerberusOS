#!/usr/bin/env python3
"""Boot the Cerberus ISO in QEMU headless, drive it over serial, inspect it via QMP.

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
                  !kill          SIGKILL QEMU at once (a power cut)
                  !poweredoff    the guest must have powered itself off
                  (also !key, !mouse, !mouseto, !button, !wheel, !screenshot)
                Directives execute after the initial --wait, in file order.
Exit status: 0 if RIP is in the kernel's higher half and all expectations hold.
"""
import argparse, json, os, re, socket, subprocess, sys, tempfile, threading, time

KERNEL_BASE = 0xFFFFFFFF80000000
BUILD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")
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

def load_setup(path):
    """'#!' lines at the top of an expect file: machine, disks, host checks.

      #!machine pc                  QEMU machine type
      #!disk new <size> <name>      a fresh cerfs image build/disk-<name>.img (mkfs.cerfs)
      #!disk blank <size> <name>    an all-zero image (nothing on it)
      #!disk keep <name>            reuse that image as left by an earlier test
      #!bus ide|nvme|virtio         how disks are attached (default ide: AHCI on
                                    q35, the legacy IDE controller on pc)
      #!hostcheck <name>            after the run: mkfs.cerfs --check must pass
      #!hostcheck-replay <name>     after the run: replay the journal on a copy, then check
    """
    setup = {"machine": None, "disks": [], "checks": [], "bus": "ide"}
    for raw in open(path, encoding="utf-8"):
        line = raw.strip()
        if not line.startswith("#!"):
            continue
        words = line[2:].split()
        if words[:1] == ["machine"] and len(words) == 2:
            setup["machine"] = words[1]
        elif words[:1] == ["bus"] and len(words) == 2 and words[1] in ("ide", "nvme", "virtio"):
            setup["bus"] = words[1]
        elif words[:2] == ["disk", "new"] and len(words) == 4:
            setup["disks"].append(("new", words[3], words[2]))
        elif words[:2] == ["disk", "blank"] and len(words) == 4:
            setup["disks"].append(("blank", words[3], words[2]))
        elif words[:2] == ["disk", "keep"] and len(words) == 3:
            setup["disks"].append(("keep", words[2], None))
        elif words[:1] in (["hostcheck"], ["hostcheck-replay"]) and len(words) == 2:
            setup["checks"].append((words[0], words[1]))
    return setup

def disk_path(name):
    return os.path.join(BUILD_DIR, f"disk-{name}.img")

def mkfs_tool():
    return os.path.join(BUILD_DIR, "tools", "mkfs.cerfs")

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
        elif line.startswith("!mouseto "):          # !mouseto <x> <y>  absolute (guest clamps at 0,0)
            x, y = line[9:].split()
            steps.append(("mouseto", (int(x), int(y))))
        elif line.startswith("!mouse "):            # !mouse <dx> <dy>  relative motion
            dx, dy = line[7:].split()
            steps.append(("mouse", (int(dx), int(dy))))
        elif line.startswith("!button "):           # !button <left|right|middle> <down|up>
            btn, state = line[8:].split()
            steps.append(("button", (btn, state == "down")))
        elif line.startswith("!wheel "):            # !wheel <notches>  positive = up
            steps.append(("wheel", int(line[7:])))
        elif line.startswith("!screenshot "):
            steps.append(("screenshot", line[12:]))
        elif line.strip() == "!kill":                # pull the plug: SIGKILL, no shutdown
            steps.append(("kill", None))
        elif line.strip() == "!poweredoff":          # the guest must have powered itself off (ACPI S5)
            steps.append(("poweredoff", None))
        else:
            steps.append(("expect", line))
    return steps

def mouse_move(q, dx, dy):
    """Relative motion through the PS/2 mouse (moves in small steps for the guest)."""
    while dx or dy:
        sx = max(-40, min(40, dx)); sy = max(-40, min(40, dy))
        q.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": sx}},
            {"type": "rel", "data": {"axis": "y", "value": sy}}])
        dx -= sx; dy -= sy
        time.sleep(0.03)

def mouse_button(q, button, down):
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"button": button, "down": down}}])

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
    ap.add_argument("--machine", default="q35", help="QEMU machine type (pc has a legacy IDE controller)")
    ap.add_argument("--disk", action="append", default=[], metavar="IMAGE",
                    help="attach a raw disk image as an IDE disk (repeatable: sda, sdb)")
    a = ap.parse_args()

    steps = load_expect(a.expect) if a.expect else []

    tmp = tempfile.mkdtemp(prefix="cerberus-qmp-")
    sock = os.path.join(tmp, "qmp.sock")
    setup = load_setup(a.expect) if a.expect else {"machine": None, "disks": [], "checks": [], "bus": "ide"}
    if setup["machine"]:
        a.machine = setup["machine"]
    for mode, name, size in setup["disks"]:
        path = disk_path(name)
        if mode == "new":
            if os.path.exists(path):
                os.remove(path)
            r = subprocess.run([mkfs_tool(), "-s", size, path], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"setup FAILED: mkfs.cerfs: {r.stdout}{r.stderr}")
                sys.exit(1)
        elif mode == "blank":
            if os.path.exists(path):
                os.remove(path)
            subprocess.run(["truncate", "-s", size, path], check=True)
        elif not os.path.exists(path):
            print(f"setup FAILED: {path} does not exist (run the test that creates it first)")
            sys.exit(1)
        a.disk.append(path)
    machine = a.machine + (",hpet=off" if a.no_hpet else "")
    # KVM when available (nested virtualisation inside WSL2 works): faster tests
    # and hardware-accurate timers. CERBERUS_QEMU_ACCEL=tcg forces emulation.
    if os.environ.get("CERBERUS_QEMU_ACCEL", "kvm") == "kvm" and os.access("/dev/kvm", os.R_OK | os.W_OK):
        accel = ["-accel", "kvm", "-cpu", "host"]
    else:
        accel = ["-cpu", "qemu64,+pdpe1gb"]
    cmd = ["qemu-system-x86_64", "-machine", machine] + accel + [
           "-smp", str(a.smp), "-m", "512M", "-cdrom", a.iso, "-boot", "d",
           "-serial", "stdio", "-display", "none", "-d", "guest_errors",
           "-no-reboot", "-no-shutdown", "-qmp", f"unix:{sock},server,nowait"]
    if a.uefi:
        cmd += ["-drive", f"if=pflash,format=raw,readonly=on,file={a.uefi}"]
    for i, d in enumerate(a.disk):
        if setup["bus"] == "nvme":
            cmd += ["-drive", f"file={d},format=raw,if=none,id=disk{i}",
                    "-device", f"nvme,drive=disk{i},serial=cerberus{i}"]
        elif setup["bus"] == "virtio":
            cmd += ["-drive", f"file={d},format=raw,if=none,id=disk{i}",
                    "-device", f"virtio-blk-pci,drive=disk{i}"]
        else:
            cmd += ["-drive", f"file={d},format=raw,if=ide,index={i},media=disk"]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)

    # Drain the serial pipe continuously. Left unread it fills up (firmware
    # output alone can do it), QEMU stops accepting serial bytes, and the
    # guest then blocks in its serial driver.
    captured = bytearray()
    def drain():
        while True:
            chunk = proc.stdout.read1(65536)
            if not chunk:
                return
            captured.extend(chunk)
    reader = threading.Thread(target=drain, daemon=True)
    reader.start()

    def seen(texts):
        """True once every string in texts has appeared, in order, in the serial output."""
        clean = ANSI.sub("", captured.decode(errors="replace"))
        pos = 0
        for t in texts:
            idx = clean.find(t, pos)
            if idx < 0:
                return False
            pos = idx + len(t)
        return True

    def expects_before(index):
        """The expect lines that precede step `index`, back to the start."""
        return [arg for kind, arg in steps[:index] if kind == "expect"]

    def expects_until_next_directive(index):
        """Every expect line up to the next directive after step `index`."""
        out = expects_before(index + 1)
        for kind, arg in steps[index + 1:]:
            if kind != "expect":
                break
            out.append(arg)
        return out

    def wait_until(texts, limit):
        deadline = time.time() + limit
        while time.time() < deadline and proc.poll() is None:
            if seen(texts):
                return True
            time.sleep(0.1)
        return seen(texts)

    # Wait for the guest to get as far as the test expects before the first
    # action, rather than for a fixed time: boot speed depends on host load,
    # and input typed too early lands in the bootloader's menu.
    first_action = next((i for i, (kind, _) in enumerate(steps) if kind != "expect"), None)
    boot_limit = max(90.0, a.wait * 10)
    if first_action is not None and expects_before(first_action):
        wait_until(expects_before(first_action), boot_limit)
        time.sleep(0.5)
    elif first_action is None and steps:
        wait_until([arg for _, arg in steps], boot_limit)   # output-only test: wait for all of it
    else:
        time.sleep(a.wait)
    q = Qmp(sock)
    killed = False
    power_failed = False
    for index, (kind, arg) in enumerate(steps):
        if kind == "kill":
            proc.kill()
            proc.wait()
            killed = True
            break
        if kind == "poweredoff":
            # With -no-shutdown QEMU stops in the "shutdown" state instead of exiting.
            deadline = time.time() + 15
            state = "?"
            while time.time() < deadline:
                state = (q.cmd("query-status") or {}).get("status", "?")
                if state == "shutdown":
                    break
                time.sleep(0.3)
            print(f"power state: {state}")
            power_failed = state != "shutdown"
            continue
        if kind == "send":
            proc.stdin.write((arg + "\n").encode()); proc.stdin.flush()
            time.sleep(0.3)
        elif kind == "key":
            type_keys(q, arg)
            time.sleep(0.3)
        elif kind == "mouse":
            mouse_move(q, *arg)
            time.sleep(0.2)
        elif kind == "mouseto":
            mouse_move(q, -4000, -4000)        # park at the top-left corner
            mouse_move(q, *arg)
            time.sleep(0.2)
        elif kind == "button":
            mouse_button(q, *arg)
            time.sleep(0.2)
        elif kind == "wheel":
            for _ in range(abs(arg)):
                b = "wheel-up" if arg > 0 else "wheel-down"
                mouse_button(q, b, True)
                mouse_button(q, b, False)
                time.sleep(0.05)
            time.sleep(0.2)
        elif kind == "screenshot":
            q.cmd("screendump", filename=os.path.abspath(arg), format="png")
            time.sleep(0.3)
        elif kind == "wait":
            # The stated time is how long the step normally takes; on a busy
            # host allow up to four times that for the expected output.
            time.sleep(arg)
            wait_until(expects_until_next_directive(index), arg * 3)

    regs, extra = "", []
    if not killed:
        time.sleep(0.2)
        regs = q.hmp("info registers")
        extra = [(h, q.hmp(h)) for h in a.hmp]
        if a.screenshot:
            q.cmd("screendump", filename=os.path.abspath(a.screenshot), format="png")
            time.sleep(0.5)
        q.cmd("quit")
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait()
    reader.join(timeout=5)
    out = captured.decode(errors="replace")

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
    if power_failed:
        print("poweredoff FAILED: the machine is still running")
        ok = False
    elif any(kind == "poweredoff" for kind, _ in steps):
        ok = True       # a powered-off CPU is not "in the kernel"; that is the point
        print("kernel entered: YES (machine powered itself off)")
    elif killed:
        ok = True
        print("kernel entered: YES (machine killed on purpose by !kill)")
    else:
        print("kernel entered: " + ("YES (RIP in higher half)" if ok else f"NO (RIP={rip:#x})" if rip else "NO"))

    if a.expect:
        pos = 0
        for kind, want in steps:
            if kind == "kill":
                break
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
    for kind, name in setup["checks"]:
        path = disk_path(name)
        if kind == "hostcheck-replay":
            copy = path + ".replay"
            subprocess.run(["cp", path, copy])
            r = subprocess.run([mkfs_tool(), "--check", "--replay", copy], capture_output=True, text=True)
        else:
            r = subprocess.run([mkfs_tool(), "--check", path], capture_output=True, text=True)
        print((r.stdout + r.stderr).strip())
        if r.returncode != 0:
            print(f"hostcheck FAILED: {path}")
            ok = False
        else:
            print(f"hostcheck OK: {path}")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
