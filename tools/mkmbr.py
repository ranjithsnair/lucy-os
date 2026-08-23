#!/usr/bin/env python3
# Writes a single-partition MBR partition table (offset 0x1BE, standard
# 16-byte entry, 0x55AA signature at 0x1FE) into an existing raw disk
# image in place - used by the Makefile's poc-os.hdd/poc-os.iso rules to
# carve out the one FAT partition `mformat`/`mcopy` then populate with
# the kernel, fs.img, and limine.conf, and that `limine bios-install`
# then makes bootable (writing its own stage 2 into the MBR gap before
# that partition). CHS fields use the standard 0xFE/0xFF/0xFF "overflow,
# use the LBA fields instead" convention, same as boot/bootasm_bios.asm
# (removed) used to for the same reason: this partition doesn't fit any
# small, exact CHS geometry.
import struct
import sys

path, start_lba, num_sectors = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

entry = (
    bytes([0x80])              # boot indicator: active/bootable
    + bytes([0xFE, 0xFF, 0xFF])  # start CHS: overflow marker
    + bytes([0x0C])             # partition type: FAT32 LBA
    + bytes([0xFE, 0xFF, 0xFF])  # end CHS: overflow marker
    + struct.pack("<I", start_lba)
    + struct.pack("<I", num_sectors)
)

with open(path, "r+b") as f:
    f.seek(0x1BE)
    f.write(entry)
    f.write(b"\x00" * (16 * 3))  # three remaining (empty) partition entries
    f.seek(0x1FE)
    f.write(bytes([0x55, 0xAA]))
