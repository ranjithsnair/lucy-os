// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

// A free page needs no metadata of its own: kfree() writes this little
// linked-list node directly into the first bytes of the page being
// freed, so the free list costs no memory beyond the free pages
// themselves.
struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// Per-physical-page reference count, indexed by (pa-kernbase_paddr)/
// PGSIZE - the basis for copy-on-write fork (kernel/vm.c's copyuvm()/
// vm_handle_pagefault()). BSS-zeroed at boot: every entry starts at 0,
// which kfree() below treats as "never kalloc()'d before" (the
// boot-time freerange() case) rather than a real reference dropping to
// a negative count. Sized to cover every physical page in the managed
// pool, [kernbase_paddr, kernbase_paddr+PHYSTOP) - memlayout.h's own
// comment on kernbase_paddr explains why the pool floats there instead
// of a fixed, compile-time-known 0 - the only range kalloc() ever hands
// out (device memory like the real framebuffer is mapped directly and
// never touches this table - see memlayout.h's ISPOOLPA()).
//
// This is the *main* pool's own refcount table only, not the direct
// map's (dmap_pageref below) - two separate tables, not one sized to
// cover all of dmap_end, because dmap_end is a runtime-discovered value
// that can be many GB on a real machine: a single compile-time array
// sized to its worst case (dmap_end's own 64GB safety cap) would cost
// up to 32MB of kernel .bss on *every* machine regardless of how much
// RAM it actually has. See kdmapreserve()'s own comment for how the
// dmap table avoids that instead.
static ushort pageref[PHYSTOP / PGSIZE];

// The direct map's own per-page reference count (pageref[]'s
// counterpart for pages kernel/limine.c's dmap_init_pool() donates to
// this allocator beyond the main pool above) - NULL/0 until
// kdmapreserve() runs, which dmap_init_pool() calls exactly once, after
// finding real, USABLE, not-already-spoken-for physical memory to carve
// this table's own backing storage out of. Indexed directly by
// pa/PGSIZE (not offset from any base): the direct map identity-offsets
// from physical 0 (DMAP_VBASE's own comment, memlayout.h), so every
// physical page below dmap_pageref_count*PGSIZE has exactly one slot
// here regardless of where it actually sits in the (possibly
// discontiguous - Limine's memmap can have gaps) real memory map.
static ushort *dmap_pageref;
static uintp dmap_pageref_count;

// Called once by kernel/limine.c's dmap_init_pool(), which - unlike
// this file - actually knows where real, USABLE physical memory is
// (Limine's memmap, entry by entry): paddr/count is wherever it found
// enough contiguous room to hold this table, outside the main pool and
// the ramdisk. Zeroed here, not left to BSS zero-init like pageref[]
// above, because this storage is ordinary donated RAM discovered at
// runtime, not part of this kernel's own static image.
void
kdmapreserve(uintp paddr, uintp count)
{
  dmap_pageref = (ushort*)DMAP_P2V(paddr);
  dmap_pageref_count = count;
  memset(dmap_pageref, 0, count * sizeof(ushort));
}

// Return this pa's refcount slot, in whichever of the two tables above
// actually covers it - the only thing kalloc()/kfree()/kaddref()/
// kgetref() need to know to treat the main pool and the direct-map-
// donated pool as one seamless allocator despite their separate
// bookkeeping. Guarded by kmem.lock (already held across every call
// site below) rather than a second lock - refcounts only ever change
// from inside those four functions, so one lock covering all of them is
// enough and avoids any lock-ordering question between two locks.
static ushort *
pagerefslot(uintp pa)
{
  if(pa >= kernbase_paddr && pa < pool_end)
    return &pageref[(pa - kernbase_paddr) / PGSIZE];
  if(dmap_pageref != 0 && pa < dmap_pageref_count * (uintp)PGSIZE)
    return &dmap_pageref[pa / PGSIZE];
  panic("pagerefslot");
}

// True iff pa is a kalloc()-managed page - the main pool (ISPOOLPA,
// memlayout.h) or the direct-map-donated one above (dmap_pageref) -
// as opposed to genuine external device memory (e.g. the real
// framebuffer's VRAM, mapped in by kernel/sysproc.c's sys_mmap()
// FRAMEBUFFER path) that just happens to have a user PTE pointing at
// it. kernel/vm.c's copyuvm()/deallocuvm() need this, not plain
// ISPOOLPA(pa) (which only ever covered the original, smaller pool),
// to correctly recognize a dmap-donated user page as COW/refcount-
// eligible ordinary RAM. Found the hard way: every user page copyuvm()
// misjudged this way got mapped straight into a fork()'d child with no
// COW protection and no added reference - both processes silently
// shared and could each write the same physical page, and whichever
// exited first freed it out from under the other, corrupting it a
// moment later (deallocuvm() has the identical blind spot for freeing
// - a leak, not a corruption, but the same root cause) - reliably
// crashing every dynamically-linked child's own ld.so startup within
// a few instructions, while a fresh exec with no fork() in its history
// (e.g. this kernel's very first process) was never affected.
int
kmanaged(uintp pa)
{
  if(pa >= kernbase_paddr && pa < pool_end)
    return 1;
  return dmap_pageref != 0 && pa < dmap_pageref_count * (uintp)PGSIZE;
}

// freerange(), minus whatever part of [vstart,vend) falls inside
// [xpstart,xpstart+xpsize) (physical addresses) - used by kinit1()/
// kinit2() below to keep from ever handing out the ramdisk's own pages
// (kernel/ide.c's ramdisk_paddr/ramdisk_size, discovered at boot by
// kernel/limine.c's limine_early_init() rather than fixed at a known
// compile-time offset the way the old BIOS boot loader's RAMDISK_PADDR
// was - so unlike that fixed constant, this can land anywhere Limine
// chose to put fs.img, including possibly splitting the middle out of
// either kinit1()'s or kinit2()'s own range).
static void
freerange_except(void *vstart, void *vend, uintp xpstart, uintp xpsize)
{
  uintp s = V2P(vstart), e = V2P(vend);
  uintp xs = xpstart, xe = xpstart + xpsize;

  if(xe <= s || xs >= e){
    freerange(vstart, vend);
    return;
  }
  if(xs > s)
    freerange(vstart, P2V(xs));
  if(xe < e)
    freerange(P2V(xe), vend);
}

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using the boot-time page table
// kernel/entry.asm built (entrypml4/entrypdpt_low/entrypdpt_high/
// entrypd) to place just the pages that table maps on the free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend, uintp ramdisk_paddr, uintp ramdisk_size)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange_except(vstart, vend, ramdisk_paddr, ramdisk_size);
}

void
kinit2(void *vstart, void *vend, uintp ramdisk_paddr, uintp ramdisk_size)
{
  freerange_except(vstart, vend, ramdisk_paddr, ramdisk_size);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uintp)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
//
// Copy-on-write fork (kernel/vm.c's copyuvm()) can leave one physical
// page mapped into more than one process's page table, each with its
// own kfree() call as it exits/execs/shrinks (kernel/vm.c's
// deallocuvm()) - so this only actually returns the page to the
// freelist once every such caller has dropped its share. A refcount
// slot that's still 0 means this exact page has never been through
// kalloc() since boot (freerange()'s/dmap_init_pool()'s own initial
// seeding of the free list) - free it unconditionally, the same as
// before refcounting existed, rather than underflowing a count that was
// never incremented in the first place.
//
// v can be either a main-pool (KERNBASE-based) or direct-map
// (DMAP_VBASE-based) pointer - V2P() (memlayout.h) tells them apart and
// translates accordingly. The `v < end` bound only makes sense for the
// former (a DMAP pointer is numerically far below `end`, an address near
// KERNBASE), hence the separate branch.
void
kfree(char *v)
{
  struct run *r;
  ushort *slot;
  int dofree, fresh;
  uintp pa;

  if((uintp)v % PGSIZE)
    panic("kfree");
  pa = V2P(v);
  if(ISDMAPVA(v)){
    if(pa >= dmap_end)
      panic("kfree");
  } else {
    if(v < end || pa >= pool_end)
      panic("kfree");
  }
  slot = pagerefslot(pa);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  fresh = (*slot == 0);
  if(fresh)
    dofree = 1;
  else if(--*slot == 0)
    dofree = 1;
  else
    dofree = 0;
  if(dofree){
    // Fill with junk to catch dangling refs - only safe now that we
    // know no other mapping still shares this physical page. Skipped
    // for a fresh page (never through kalloc() before - freerange()'s/
    // dmap_init_pool()'s own initial seeding): nothing could hold a
    // dangling reference to a page that's never been handed out, and
    // writing it unconditionally here would mean touching - and, on a
    // hypervisor, forcing the host to actually back - every single byte
    // of however much RAM dmap_init_pool() donates, which can be most
    // of a real machine's memory. Found the hard way (a boot that
    // looked hung for minutes on an 11GB VM, actually just the host
    // thrashing under the memory pressure of dirtying all of it at once).
    if(!fresh)
      memset(v, 1, PGSIZE);
    r = (struct run*)v;
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Add one reference to an already-kalloc()'d page - called by
// kernel/vm.c's copyuvm() when a fork shares a page COW instead of
// copying it, and by vm_handle_pagefault() when a COW fault resolves
// without needing a fresh copy.
void
kaddref(uintp pa)
{
  if(kmem.use_lock)
    acquire(&kmem.lock);
  (*pagerefslot(pa))++;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Current reference count of an already-kalloc()'d page - used by
// kernel/vm.c's vm_handle_pagefault() to tell "I'm the last owner,
// just reclaim this page in place" from "still shared, must copy".
int
kgetref(uintp pa)
{
  int n;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  n = *pagerefslot(pa);
  if(kmem.use_lock)
    release(&kmem.lock);
  return n;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    // A freshly-handed-out page always starts single-owner, regardless
    // of what it was last used for (kfree() only ever leaves a page on
    // the freelist once its refcount already reached 0) - set, not
    // increment. r can be either a main-pool or direct-map pointer
    // (kfree() pushes both kinds onto the same freelist) - V2P()
    // (memlayout.h) tells them apart the same way kfree() does.
    *pagerefslot(V2P((char*)r)) = 1;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

