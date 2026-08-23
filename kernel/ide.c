// Ramdisk-backed block "device" - stands in for a real disk driver.
//
// poc-os has no real disk driver of any kind: not the raw ATA PIO this
// file used to be (only ever worked against a real/emulated legacy IDE
// controller - not AHCI-mode SATA, not a USB Mass Storage device, and
// not the ATAPI protocol a virtual/real CD or DVD drive actually
// speaks), and not a real AHCI or USB stack either (both genuinely
// large undertakings - PCI enumeration and command-queue management
// for AHCI, a whole USB stack for Mass Storage - that would still only
// cover *some* of real hardware/VirtualBox/QEMU, not all of them
// uniformly).
//
// Instead, Limine loads the *entire* root filesystem image (fs.img) as
// a boot module (limine.conf's module_path, kernel/limine.c's
// limine_early_init() - which sets ramdisk_paddr/ramdisk_size below)
// into RAM before the kernel ever starts running, using whichever
// disk-reading mechanism its own BIOS/UEFI boot stage has. Once the
// kernel is running, it never touches disk hardware again: every
// iderw() call below is just a memmove() to/from that already-loaded
// RAM image. Writes (mkdir, rm, mv, ...) modify the RAM copy only -
// like any live-boot/initrd-style system, changes don't persist across
// a reboot unless something explicitly writes the RAM image back out,
// which nothing here does.
//
// ideintr() is kept as a no-op stub, not removed outright, so
// kernel/trap.c doesn't need a call-site change.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

uintp ramdisk_paddr;
uintp ramdisk_size;

void
ideinit(void)
{
  // ramdisk_paddr/ramdisk_size (kernel/limine.c's limine_early_init(),
  // called from main() well before this) must already be set - checked
  // here, not there, since FSSIZE (param.h/fs.h) isn't limine.c's
  // concern. A mismatch means limine.conf's module and mkfs/mkfs.c's
  // fs.img have drifted apart - a real build-time bug, not something to
  // limp past.
  if(ramdisk_size != (uintp)FSSIZE * BSIZE)
    panic("ideinit: ramdisk module size doesn't match FSSIZE*BSIZE - "
          "limine.conf's fs.img module and mkfs disagree");

  // No kmapphys() call needed here any more: kernel/vm.c's kmap[] has
  // its own dedicated ramdisk entry now, at the fixed RAMDISK_VBASE
  // (memlayout.h) rather than HW_P2V(ramdisk_paddr) - kvmalloc() (also
  // called from main(), before this) already patched that entry's
  // phys_start/phys_end from ramdisk_paddr/ramdisk_size and built
  // kpgdir with it included, and every process's own pgdir gets it too
  // from here on (setupkvm() installs every kmap[] entry into every
  // process's page table - unlike the old boot-time-only kmapphys()
  // call this replaced, which only ever reached whichever single pgdir
  // happened to be active at this exact call site).
}

void
ideintr(void)
{
  // The ramdisk never raises an interrupt; iderw() below is already
  // synchronous. Kept only because kernel/trap.c's IRQ_IDE case still
  // calls it - a no-op body is the correct response to an interrupt
  // that (on real hardware particularly) should never actually fire
  // for a device this kernel never programs.
}

// Sync buf with the ramdisk. If B_DIRTY is set, copy buf->data into
// the ramdisk and clear B_DIRTY; else if B_VALID is not set, copy from
// the ramdisk into buf->data and set B_VALID - the exact same
// contract the real iderw() this replaces had, so kernel/bio.c's
// bread()/bwrite() (the only callers) needed no changes at all.
void
iderw(struct buf *b)
{
  char *disk;

  if(!holdingsleep(&b->lock))
    panic("iderw: buf not locked");
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  if(b->blockno >= FSSIZE)
    panic("iderw: blockno out of range");

  disk = (char*)RAMDISK_VBASE + (uintp)b->blockno * BSIZE;
  if(b->flags & B_DIRTY){
    memmove(disk, b->data, BSIZE);
    b->flags &= ~B_DIRTY;
  } else {
    memmove(b->data, disk, BSIZE);
  }
  b->flags |= B_VALID;
}

// Reads nblocks contiguous blocks (blockno..blockno+nblocks) straight
// out of the ramdisk into dst, bypassing the buffer cache entirely -
// kernel/fs.c's readi() is the only caller, for a bulk run it has
// already confirmed (kernel/bio.c's bio_range_clean()) has no dirty,
// not-yet-written-back buffer anywhere in it, so this and the cache
// are guaranteed to agree. One memmove() instead of nblocks separate
// bget()/bread()/brelse() cycles - see readi()'s own comment for why
// that distinction is what actually matters here.
void
ide_bulk_read(uint blockno, char *dst, uint nblocks)
{
  if((uintp)blockno + nblocks > FSSIZE)
    panic("ide_bulk_read: out of range");
  memmove(dst, (char*)RAMDISK_VBASE + (uintp)blockno * BSIZE, (uintp)nblocks * BSIZE);
}
