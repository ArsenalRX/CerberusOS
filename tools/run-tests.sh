#!/bin/bash
# Runs the named integration tests (tests/integration/<name>.expect) against
# build/cerberus.iso and prints the interesting lines of each log. For quick
# checks during development; `make test` runs them all.
#   tools/run-tests.sh cerfs cerfs-ahci
cd "$(dirname "$0")/.."
for t in "$@"; do
    python3 tools/qemu-probe.py build/cerberus.iso --wait 6 --expect tests/integration/$t.expect > build/test-$t.log 2>&1
    echo "== $t rc=$?"
    grep -a -E "expect (OK|FAILED)|test: |killed|PANIC|panic|ASSERT|hostcheck|consistent|DAMAGED|damage:|replayed|fstest: (pass|the|wrote|verified|MISMATCH|churn done)|cerfs:|ahci:|ata:|cannot|error" build/test-$t.log | head -24
done
