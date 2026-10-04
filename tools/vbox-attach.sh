#!/bin/bash
# Points every VirtualBox VM that boots a Cerberus ISO from dist/ at the given
# ISO, and makes sure such a VM can run a 64-bit guest. Run by `make dist`.
#
# Why: dist/ holds one ISO whose name changes with each release, and a VM
# created with VirtualBox's wizard gets OS type "Other/Unknown", which is
# 32-bit and hides 64-bit mode; the bootloader then refuses to start Cerberus.
# Only powered-off VMs whose DVD drive already points into dist/ are touched.
#
# usage: vbox-attach.sh <path-to-VBoxManage.exe> <new-iso (WSL path)>
VBOX="$1"
ISO="$2"
[ -x "$VBOX" ] || { echo "VirtualBox not found; no VM updated"; exit 0; }
ISO_WIN=$(wslpath -w "$ISO")
found=0
while IFS= read -r line; do
    name=$(printf '%s' "$line" | sed -n 's/^"\(.*\)" {.*}$/\1/p')
    [ -n "$name" ] || continue
    # Every VBoxManage call gets </dev/null: otherwise it inherits this
    # loop's input and swallows the rest of the VM list.
    info=$("$VBOX" showvminfo "$name" --machinereadable 2>/dev/null </dev/null | tr -d '\r')
    # A DVD attachment looks like: "IDE-1-0"="D:\\Programs\\OS\\dist\\cerberus-0.6.0.iso"
    # (VMs still pointing at the old name, dist\\lumen.iso, are re-pointed too.)
    slot=$(printf '%s\n' "$info" | grep -i -E 'dist\\\\(cerberus|lumen)[^"]*\.iso"$' | head -1)
    [ -n "$slot" ] || continue
    found=1
    if ! printf '%s\n' "$info" | grep -q '^VMState="poweroff"'; then
        echo "VirtualBox VM '$name' is running or saved; attach $(basename "$ISO") by hand"
        continue
    fi
    key=$(printf '%s' "$slot" | sed 's/^"\([^"]*\)"=.*/\1/')      # e.g. IDE-1-0
    ctl=${key%-*-*}
    rest=${key#"$ctl"-}
    port=${rest%-*}
    dev=${rest#*-}
    "$VBOX" storageattach "$name" --storagectl "$ctl" --port "$port" --device "$dev" --type dvddrive \
        --medium "$ISO_WIN" >/dev/null 2>&1 </dev/null \
        && echo "VirtualBox VM '$name' now boots $(basename "$ISO")" \
        || echo "VirtualBox VM '$name': could not attach $(basename "$ISO")"
    if ! printf '%s\n' "$info" | grep -q '^longmode="on"'; then
        "$VBOX" modifyvm "$name" --ostype Other_64 >/dev/null 2>&1 </dev/null
        "$VBOX" modifyvm "$name" --x86-long-mode on --x86-pae on --ioapic on --hpet on >/dev/null 2>&1 </dev/null \
            && echo "VirtualBox VM '$name' was 32-bit; switched to 64-bit so Cerberus can boot"
    fi
done < <("$VBOX" list vms 2>/dev/null </dev/null | tr -d '\r')
[ "$found" = 1 ] || echo "no VirtualBox VM boots an ISO from dist/; attach $(basename "$ISO") to a 64-bit VM by hand"
exit 0
