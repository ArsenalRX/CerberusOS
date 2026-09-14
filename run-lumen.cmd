@echo off
rem Boots the latest Lumen ISO in QEMU (KVM-accelerated inside WSL2) with a
rem window on the Windows desktop via WSLg. Serial output goes to logs\qemu-serial.log.
rem Usage: run-lumen.cmd            (BIOS boot, default)
rem        run-lumen.cmd uefi       (boot through OVMF)
rem        run-lumen.cmd build      (rebuild the ISO first, then boot)
setlocal
set TARGET=run
if /I "%~1"=="uefi"  set TARGET=run-uefi
if /I "%~1"=="build" set TARGET=iso run
if not exist "%~dp0logs" mkdir "%~dp0logs"
echo Starting Lumen in QEMU (close the QEMU window to stop)...
wsl -d Ubuntu-24.04 -u root -- bash -c "cd /mnt/d/Programs/OS && make %TARGET% QEMU_DISPLAY='-display gtk,zoom-to-fit=on' 2>&1 | tee logs/qemu-serial.log"
echo.
echo Serial log saved to %~dp0logs\qemu-serial.log
pause
