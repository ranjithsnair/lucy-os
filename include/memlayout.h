// Memory layout

#define EXTMEM  0x100000            // Start of extended memory
#define PHYSTOP 0xE000000           // Top physical memory
#define DEVSPACE 0xFE000000         // Other devices are at high addresses

// The root filesystem (fs.img) and the linear framebuffer used to be
// found at fixed physical addresses (RAMDISK_PADDR, VBE_INFO_PADDR) the
// old hand-written BIOS boot loader (boot/boot2_bios.asm, removed)
// hardcoded and the kernel just trusted. Limine picks both locations
// itself instead - see kernel/limine.c, which discovers them at boot
// time (ramdisk_paddr/ramdisk_size globals, and the framebuffer request
// kernel/vbe.c's vbeinit() reads) rather than a boot-loader/kernel
// compile-time contract.

// Key addresses for address space layout (see kmap in vm.c for layout)
//
// KERNBASE is a canonical higher-half address (top -2GB, the same
// shape Linux uses) - PHYSTOP is modest (~224MB, above) so it, plus
// DEVSPACE, fits comfortably in the 2GB above KERNBASE without needing
// a separate physical direct-map region.
#define KERNBASE 0xFFFFFFFF80000000
#define KERNLINK (KERNBASE+EXTMEM)  // Address where kernel is linked

// kernbase_paddr: the physical address VA=KERNBASE itself maps to -
// kernel/limine.c's limine_entry_init() sets this once, from Limine's
// executable-address response, before anything below is ever used.
// Under the old BIOS boot loader this kernel used to have (removed),
// the kernel was always loaded at a fixed physical address (EXTMEM)
// low enough that "physical == virtual - KERNBASE" (a bare, compile-
// time-constant offset) was true for the kernel's own image *and* for
// every kalloc()'d page in the managed [0,PHYSTOP) pool alike, since
// both lived in the same low, contiguous region. Limine instead picks
// the kernel's physical load address itself - observed landing near
// the *top* of RAM, not near 0 - so V2P/P2V below now offset by this
// runtime-discovered value instead: this kernel's own text/data/bss
// and the general kalloc() pool (kernel/kalloc.c, kernel/vm.c's kmap[])
// stay one contiguous physical region exactly as before, just anchored
// at kernbase_paddr instead of a compile-time-assumed 0. Found the hard
// way (a still-different boot hang, chased down with VirtualBox's
// built-in debugger) once boot got far enough to reach kernel/vm.c's
// kvmalloc(), which - unlike kernel/entry.asm's own bootstrap table,
// fixed earlier - still assumed the old fixed-physical-address
// contract via this same V2P()/P2V() pair.
#ifndef __ASSEMBLER__
extern uintp kernbase_paddr;

// pool_end: how far past kernbase_paddr real, backed RAM actually
// extends (capped at kernbase_paddr+PHYSTOP) - kernel/limine.c's
// limine_early_init() computes this from Limine's own memory map.
// PHYSTOP alone (a fixed, compile-time ~224MB) is NOT a safe assumption
// here: found the hard way (a boot hang reproducing only on a 2GiB VM)
// that Limine can place the kernel close enough to the true top of RAM
// that nowhere near 224MB of real memory follows it - kalloc.c's
// kfree() and kernel/vm.c's kmap[]/kvmalloc() all need this actual
// limit, not the aspirational one.
extern uintp pool_end;

// dmap_end: how much of physical memory, starting at 0, is covered by
// the direct map (kernel/vm.c's build_dmap()) - [0, dmap_end). Lets
// kernel/kalloc.c manage RAM elsewhere in Limine's memory map besides
// the [kernbase_paddr, pool_end) run immediately following the kernel
// above: Limine can - and on real multi-gigabyte machines, does -
// place the kernel close enough to a reserved-region boundary that
// only a few MB of real RAM follows it, leaving the other several GB
// of perfectly usable RAM elsewhere in the map completely unmanaged
// otherwise. Found the hard way (a VirtualBox VM with 11GB of RAM):
// every allocuvm() past the first few hundred KB failed with "out of
// memory" despite the machine having far more RAM than that, once the
// interrupt-frame/GDT bugs that used to crash boot long before any of
// this mattered were fixed.
//
// Modeled on ToaruOS's own approach (kernel/arch/x86_64/mmu.c's
// direct_map_pml/mmu_map_from_physical()), not kmap[]'s per-kmap-entry
// scheme: DMAP_VBASE (below) sits in its own PML4 slot, entirely
// separate from KERNBASE's (see PML4X's own comment) - build_dmap()
// builds this exactly once, into kpgdir, and setupkvm() (kernel/vm.c)
// makes it visible to every process's own pgdir by copying that one
// PML4 entry, not by rebuilding a whole page-table's worth of PD/PT
// pages fresh every time the way every other kmap[] entry does. A
// first attempt at this (kmap[]-based, capped at 1.5GB, reusing
// RAMDISK_VBASE's per-kmap-entry pattern) hit a real bug on the way
// to being reverted: the physical base of *that* region was only
// known at boot time and could land unaligned relative to its own
// fixed virtual base, which - since mappages()'s 2MB-page fast path
// only fires when the virtual and physical addresses are 2MB-aligned
// at the very same step - silently forced the entire region through
// its 4KB-PTE fallback, hundreds of extra PT pages that exhausted the
// tiny boot-time pool before ever mapping one real page. DMAP_VBASE
// doesn't have that problem: it identity-offsets from physical 0, so
// it and every physical address are always 2MB-aligned in lockstep.
extern uintp dmap_end;
#endif

// For fixed, absolute low physical addresses that exist at that same
// spot on every x86 machine regardless of where Limine loaded this
// kernel - the BIOS Data Area, the legacy CGA text buffer, the MP
// table/warm-reset-vector locations (kernel/acpi.c, kernel/console.c,
// kernel/lapic.c, kernel/mp.c) - unlike V2P/P2V above, which are
// relative to wherever this kernel's own image actually is. Backed by
// kernel/vm.c's kmap[] "I/O space" entry, mapping VA[KERNBASE,
// KERNBASE+EXTMEM) identically to PA[0,EXTMEM) - that entry, unlike
// kmap[]'s other ones, never depended on the kernel's own physical
// location in the first place, so it's untouched by kernbase_paddr.
// A plain compile-time constant (valid in a static initializer, unlike
// V2P/P2V above) since it only ever needs the fixed KERNBASE offset.
#define HW_P2V(a) ((void *)(((uintp) (a)) + KERNBASE))
#define HW_V2P(a) (((uintp) (a)) - KERNBASE)

// True iff pa is a genuine kalloc()'d pool page (kernel/kalloc.c) -
// the pool now lives at [kernbase_paddr, pool_end), not [0,PHYSTOP), so
// kernel/vm.c's copyuvm()/deallocuvm() (the only callers - see their
// own comments) need both bounds, not just an upper one, to keep
// telling a real pool page apart from something like framebuffer VRAM
// mapped in by sys_mmap()'s FRAMEBUFFER path, which can just as easily
// sit below kernbase_paddr as above it.
#define ISPOOLPA(pa) ((pa) >= kernbase_paddr && (pa) < pool_end)

// Fixed virtual base for the ramdisk (kernel/ide.c) - NOT HW_P2V(
// ramdisk_paddr): that macro is only safe for physical addresses under
// 2GB (KERNBASE + a >= 2GB wraps out of the canonical-high range
// entirely, right back down into low/user-space-shaped addresses,
// since KERNBASE itself already sits exactly 2GB below the 64-bit
// wraparound point) - fine for the fixed low addresses HW_P2V exists
// for (the BIOS Data Area, CGA buffer, MP tables), but Limine is free
// to place a boot module anywhere in physical memory, including well
// above 2GB (observed: ~3.7GB on an 11GB VM). Found the hard way (a
// page fault deep inside a syscall on a VirtualBox VM with enough RAM
// to place the ramdisk past that boundary) - kernel/vm.c's kmap[] adds
// a dedicated entry mapping the ramdisk here instead, sized to
// FSSIZE*BSIZE (a compile-time constant - see kernel/ide.c's own
// ramdisk_size cross-check), in every process's page table exactly
// like kmap[]'s other entries, patched at boot from ramdisk_paddr the
// same way kmap[1]/kmap[2] are patched from kernbase_paddr.
#define RAMDISK_VBASE (KERNBASE + PHYSTOP)

// Direct map base (dmap_end's own comment above) - PML4X(DMAP_VBASE)
// (kernel/mmu.h) is 256, the very first high-canonical PML4 slot,
// deliberately far from PML4X(KERNBASE)=511: keeping the direct map
// in its own, otherwise-unused PML4 slot is what lets setupkvm()
// share it across every process with one 8-byte PML4 entry copy
// instead of walking/rebuilding PDPT/PD/PT levels. A simple identity
// offset from physical 0 (unlike RAMDISK_VBASE/HW_P2V, which offset
// from the kernel's own load address or a small fixed base) - every
// physical address maps here at the same relative position, always
// 2MB-aligned together, and nothing about the kernel's own location
// affects it, so it can safely cover a large, mostly-empty span of
// physical address space without needing to know in advance how much
// of it will actually turn out to be backed by real RAM.
#define DMAP_VBASE 0xFFFF800000000000ULL
#define DMAP_P2V(pa) ((void *)(((uintp)(pa)) + DMAP_VBASE))
#define DMAP_V2P(va) (((uintp)(va)) - DMAP_VBASE)
#define ISDMAPVA(va) ((uintp)(va) >= DMAP_VBASE && (uintp)(va) < DMAP_VBASE + dmap_end)

// V2P/P2V: kalloc() (kernel/kalloc.c) now transparently hands out pages
// from two disjoint pools sharing one freelist - the original KERNBASE-
// relative one and the direct-map-backed one kernel/limine.c's
// dmap_init_pool() donates (dmap_pageref/kdmapreserve's own comments) -
// so *every* existing caller that turns a kalloc()'d kernel pointer
// into a physical address (or back) needs to know which VA scheme it's
// looking at, not just apply the KERNBASE-relative formula unconditio-
// nally. Rather than hunt down and fix every such call site (walknext()/
// walkpgdir() alone use both directions on whatever physical page a PTE
// happens to name), these two macros - already used everywhere in the
// kernel for exactly this purpose - just do the right thing themselves.
// Found the hard way: kernel/vm.c's switchuvm() calling the old,
// KERNBASE-only V2P() on a process's pgdir silently produced a garbage
// physical address (a huge, wildly wrong value from underflowing the
// KERNBASE subtraction against a DMAP_VBASE-relative pointer) whenever
// that pgdir happened to be a dmap-donated page - kalloc()'s freelist
// is LIFO, so a freshly-donated pool this large dominates every kalloc()
// call for a long stretch after boot - which reached lcr3() as an
// invalid CR3 value and #GP'd on every attempt to actually run the
// first user process.
//
// P2V(pa) prefers the traditional KERNBASE-relative alias for any pa
// that's ALSO within the main pool's own range rather than always
// taking the dmap alias: build_dmap() maps all of [0,dmap_end), which is
// a superset of [kernbase_paddr,pool_end) - both aliases reach the same
// physical page equally validly, but staying on the original alias here
// preserves every existing assumption elsewhere in the kernel that
// P2V()'s output for a main-pool page is KERNBASE-relative (e.g. bounds
// checks against kernbase_paddr/pool_end). Deliberately NOT ISPOOLPA
// (memlayout.h, above) here: that macro's strict "< pool_end" is right
// for asking "is this a real page in the pool," but callers like
// kernel/main.c's kinit1()/kinit2() call P2V(pool_end) itself - the
// pool's own exclusive end, one-past-the-last-real-page, used only for
// pointer arithmetic/comparison in freerange()'s loop, never
// dereferenced there. ISPOOLPA(pool_end) is false, which would have
// routed that call through DMAP_P2V() instead - a virtual address the
// tiny entry.asm bootstrap page table (still active at kinit1()'s call
// site, before kvmalloc() ever builds the direct map) doesn't map at
// all. Found the hard way: an unhandled page fault before idtinit() has
// even run yet, silently triple-faulting the machine back to the BIOS
// on every boot attempt.
#define V2P(a) (ISDMAPVA(a) ? DMAP_V2P(a) : ((((uintp) (a)) - KERNBASE) + kernbase_paddr))
#define P2V(a) ((uintp)(a) >= kernbase_paddr && (uintp)(a) <= pool_end ? \
                ((void *)((((uintp) (a)) - kernbase_paddr) + KERNBASE)) : DMAP_P2V(a))
