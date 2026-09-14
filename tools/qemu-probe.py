#!/usr/bin/env python3
"""Boot the Lumen ISO in QEMU headless, wait, then report the guest CPU state via QMP.

Used by phase-0/1 checks and the integration harness to prove the kernel was
actually entered (RIP inside the kernel image) rather than trusting silence.

Usage: qemu-probe.py <iso> [--uefi OVMF_CODE.fd] [--wait SECONDS] [--smp N]
Exit status: 0 if RIP is in the kernel's higher half, 1 otherwise.
"""
import argparse, json, os, socket, subprocess, sys, tempfile, time

KERNEL_BASE = 0xFFFFFFFF80000000

def qmp(sock_path, cmd, **args):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    f = s.makefile("rwb", buffering=0)
    f.readline()                                  # greeting
    f.write(b'{"execute":"qmp_capabilities"}\n'); f.readline()
    f.write((json.dumps({"execute": cmd, "arguments": args}) + "\n").encode())
    reply = json.loads(f.readline())
    s.close()
    return reply.get("return")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--uefi", metavar="OVMF_CODE")
    ap.add_argument("--wait", type=float, default=8.0)
    ap.add_argument("--smp", type=int, default=4)
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
    regs = qmp(sock, "human-monitor-command", **{"command-line": "info registers"})
    qmp(sock, "quit")
    try:
        out = proc.communicate(timeout=5)[0].decode(errors="replace")
    except subprocess.TimeoutExpired:
        proc.kill(); out = proc.communicate()[0].decode(errors="replace")

    if a.uefi:
        # OVMF probes TPM/flash addresses that -d guest_errors reports; drop that noise.
        out = "\n".join(l for l in out.splitlines() if not l.startswith("Invalid "))
    print("---- serial ----"); print(out.rstrip()); print("---- cpu ----")
    rip = None
    for line in regs.splitlines():
        if line.startswith("RIP="):
            rip = int(line.split()[0][4:], 16)
            print(line.strip()); break
    ok = rip is not None and rip >= KERNEL_BASE
    print("kernel entered: " + ("YES (RIP in higher half)" if ok else f"NO (RIP={rip:#x})" if rip else "NO"))
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
