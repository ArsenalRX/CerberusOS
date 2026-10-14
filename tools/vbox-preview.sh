#!/bin/bash
# Boots an ISO in a temporary VirtualBox VM (4 CPUs, 2 GiB, VBoxVGA, like the
# owner's), drives the desktop with scancodes, takes screenshots and deletes
# the VM again. The pictures are what the GUI review in the Desktop list is
# written from (docs/WORKING_NOTES.md, "Preview every build yourself").
#
# usage: vbox-preview.sh <iso (Windows or Git Bash path)> <out-dir> [vm-base-folder]
#
# Runs from Git Bash on Windows (VBoxManage.exe). The mouse cannot be driven,
# so only keyboard-reachable screens are captured: the desktop after boot,
# the launcher, Settings, the System Monitor, the terminal.
set -u
ISO="$1"; OUT="$2"; BASE="${3:-$OUT/vm}"
VBOX="/c/Program Files/Oracle/VirtualBox/VBoxManage.exe"
NAME="cerberus-preview"
mkdir -p "$OUT" "$BASE"
w() { "$VBOX" "$@" </dev/null; }
win() { cygpath -w "$1"; }
shot() { sleep "${2:-1}"; w controlvm "$NAME" screenshotpng "$(win "$OUT/$1.png")" >/dev/null 2>&1 && echo "shot: $1.png"; }
# keys: set-1 make codes; a release is make|0x80. Extended keys carry e0.
key() { w controlvm "$NAME" keyboardputscancode "$@" >/dev/null 2>&1; sleep 0.15; }
press() { for c in "$@"; do key "$c" "$(printf '%02x' $((0x$c | 0x80)))"; done; }
super() { key e0 5b e0 db; }
typetext() {  # lower-case letters, digits, space, enter
    local s="$1" i ch code
    for ((i = 0; i < ${#s}; i++)); do
        ch="${s:i:1}"
        case "$ch" in
            a) code=1e;; b) code=30;; c) code=2e;; d) code=20;; e) code=12;; f) code=21;; g) code=22;;
            h) code=23;; i) code=17;; j) code=24;; k) code=25;; l) code=26;; m) code=32;; n) code=31;;
            o) code=18;; p) code=19;; q) code=10;; r) code=13;; s) code=1f;; t) code=14;; u) code=16;;
            v) code=2f;; w) code=11;; x) code=2d;; y) code=15;; z) code=2c;; ' ') code=39;;
            1) code=02;; 2) code=03;; 3) code=04;; 4) code=05;; 5) code=06;; 6) code=07;; 7) code=08;;
            8) code=09;; 9) code=0a;; 0) code=0b;; *) continue;;
        esac
        press "$code"
    done
}
enter() { press 1c; }
escape() { press 01; }
altf4() { key 38 3e be b8; }

# a leftover VM from an interrupted run
if w showvminfo "$NAME" >/dev/null 2>&1; then
    w controlvm "$NAME" poweroff >/dev/null 2>&1; sleep 2
    w unregistervm "$NAME" --delete >/dev/null 2>&1
fi
w createvm --name "$NAME" --ostype Other_64 --register --basefolder "$(win "$BASE")" >/dev/null || exit 1
w modifyvm "$NAME" --memory 2048 --cpus 4 --ioapic on --hpet on --x86-long-mode on --x86-pae on \
    --graphicscontroller vboxvga --vram 64 --boot1 dvd --boot2 none --mouse ps2 --keyboard ps2 \
    --audio-enabled off --uart1 0x3F8 4 --uartmode1 file "$(win "$OUT/serial.log")" >/dev/null
w storagectl "$NAME" --name SATA --add sata --portcount 2 >/dev/null
w storageattach "$NAME" --storagectl SATA --port 0 --device 0 --type dvddrive --medium "$(win "$ISO")" >/dev/null
w startvm "$NAME" --type headless >/dev/null || { echo "preview: VM did not start"; exit 1; }

echo "preview: booting"
sleep 30
shot 1-desktop 0
super;                       shot 2-launcher 1
typetext "settings"; enter;  shot 3-settings 2
altf4
super; typetext "monitor"; enter; shot 4-monitor 3
altf4
super; typetext "files"; enter;   shot 5-files 2
altf4
key e0 5b 14 94 e0 db;       sleep 1      # Super+T: a terminal
typetext "help"; enter;      shot 6-terminal 1
altf4
super; typetext "about"; enter;   shot 7-about 2
altf4
w controlvm "$NAME" poweroff >/dev/null 2>&1
sleep 2
w unregistervm "$NAME" --delete >/dev/null 2>&1
grep -a -m1 "boot: OK\|vmmdev:" "$OUT/serial.log" 2>/dev/null | head -2
echo "preview: done ($OUT)"
