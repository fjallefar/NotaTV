#!/bin/sh
# NotaTV post-image: build the EFI system file and a hybrid ISO (USB + CD, UEFI)
set -e
BIN=$BINARIES_DIR
HOST=$HOST_DIR/bin
EFI=$BIN/bzImage
[ -f "$EFI" ] || { echo "bzImage mangler"; exit 1; }
cp "$EFI" $BIN/notatv.efi

# FAT image for the EFI partition (size = kernel + 8 MB)
SZ_KB=$(( ($(stat -c %s "$EFI") / 1024) + 8192 ))
rm -f $BIN/efiboot.img
$HOST_DIR/sbin/mkfs.vfat -C -n NOTATV_EFI $BIN/efiboot.img $SZ_KB >/dev/null
$HOST/mmd -i $BIN/efiboot.img ::/EFI ::/EFI/BOOT
$HOST/mcopy -i $BIN/efiboot.img "$EFI" ::/EFI/BOOT/BOOTX64.EFI

# ISO contents: the system file (for installing/updating) + version info
ISO=$BIN/iso-root
rm -rf $ISO
mkdir -p $ISO
cp "$EFI" $ISO/notatv.efi
cp $TARGET_DIR/etc/notatv-release $ISO/notatv-release
cat > $ISO/LÆSMIG.txt <<EOR
NotaTV - bootbar ISO

Skriv ISO'en til en USB-nøgle (fx med dd, balenaEtcher eller Rufus i DD-tilstand),
start computeren fra USB i UEFI-tilstand (Secure Boot slået fra).
Installer på intern disk under Indstillinger -> System.
EOR

VER=$(sed -n 's/^BUILD=//p' $TARGET_DIR/etc/notatv-release)
OUT=$BIN/notatv.iso
rm -f $OUT
$HOST/xorriso -as mkisofs \
	-iso-level 3 -full-iso9660-filenames -joliet -rational-rock \
	-volid "NOTATV" -appid "NotaTV $VER" -publisher "NotaTV" \
	-append_partition 2 C12A7328-F81F-11D2-BA4B-00A0C93EC93B $BIN/efiboot.img \
	-appended_part_as_gpt \
	-e --interval:appended_partition_2:all:: -no-emul-boot \
	-isohybrid-gpt-basdat \
	-o $OUT $ISO 2>&1 | grep -v "^xorriso : UPDATE" || true
[ -f $OUT ] || { echo "ISO blev ikke lavet"; exit 1; }
cp $OUT $BR2_EXTERNAL_NOTATV_PATH/../notatv.iso 2>/dev/null || true
echo "NotaTV ISO: $OUT ($(du -h $OUT | cut -f1))"
