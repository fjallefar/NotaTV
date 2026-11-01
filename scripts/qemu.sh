#!/bin/sh
# Start the NotaTV ISO in QEMU (UEFI, ISO as a USB stick, 8 GB virtual disk for install tests)
# Usage: scripts/qemu.sh [iso] [outdir]     - controlled via scripts/qmp.py
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ISO=${1:-$ROOT/notatv.iso}
OUT=${2:-/tmp/notatv-qemu}
mkdir -p "$OUT"
[ -f "$OUT/vars.fd" ] || cp /usr/share/edk2/x64/OVMF_VARS.4m.fd "$OUT/vars.fd"
[ -f "$OUT/disk.img" ] || truncate -s 8G "$OUT/disk.img"
rm -f "$OUT/qmp.sock" "$OUT/serial.sock"
BOOT_ISO=${BOOT_ISO:-1}
if [ "$BOOT_ISO" = 1 ]; then
	USB="-device qemu-xhci -drive id=usb,file=$ISO,format=raw,if=none,readonly=on -device usb-storage,drive=usb,bootindex=1"
else
	USB="-device qemu-xhci"
fi
exec qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 2048 -machine q35 \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
	-drive if=pflash,format=raw,file="$OUT/vars.fd" \
	$USB \
	-drive file="$OUT/disk.img",format=raw,if=virtio \
	-vga none ${GPU:--device bochs-display,edid=on,xres=1920,yres=1080} -display none \
	-device virtio-keyboard-pci \
	-nic user,model=virtio-net-pci,hostfwd=tcp::8080-:80,hostfwd=tcp::2222-:22 \
	-audiodev none,id=snd -device intel-hda -device hda-duplex,audiodev=snd \
	-chardev socket,id=s0,path="$OUT/serial.sock",server=on,wait=off,logfile="$OUT/serial.log" -serial chardev:s0 \
	-qmp unix:"$OUT/qmp.sock",server,nowait
