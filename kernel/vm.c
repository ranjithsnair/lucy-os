// Virtual memory: building and tearing down four-level page tables
// (see walkpgdir), the fixed kernel mapping shared by every process
// (see kmap[] below), and the user-memory helpers (allocuvm/
// deallocuvm/copyuvm/copyout/...) used by exec(), fork(), sbrk(), and
// system calls that read or write user-supplied pointers. The
// user-memory helpers only ever call through walkpgdir/mappages, never
// touch a page table level directly.

#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors, and load the (temporary,
// bootstrap) IDT-less GDT built by entry.asm - run once on entry on
// each CPU. Segmentation is flat/ignored for data access in long mode,
// so there's no separate user-vs-kernel data segment distinction that
// matters; SEG_UCODE/SEG_UDATA still exist so user mode has its own
// DPL=3 code segment to run with.
void
seginit(void)
{
  struct cpu *c;

  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEGL(STA_X|STA_R, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEGL(STA_X|STA_R, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
  // cs/ss aren't re-validated against the GDT just because a new one
  // was loaded - explicitly reload both to this table's own SEG_KCODE/
  // SEG_KDATA now, or they silently keep referencing whatever selector
  // was active before (Limine's own GDT, on the boot cpu) until the
  // first trap return #GPs trying to restore it - see reloadseg()'s
  // own comment (kernel/x86.asm).
  reloadseg(SEG_KCODE<<3, SEG_KDATA<<3);
}

// Enables FPU/SSE instructions on this CPU - run once per CPU (see
// kernel/main.c's mpmain(), reached by both the boot CPU and every AP),
// same "once per core" scope as seginit() above. CR0.EM=0 lets x87/SSE
// instructions execute instead of raising #UD/#NM; CR0.MP=1 makes a
// task switch actually able to trap FPU use (unused by this kernel's
// eager save/restore, kernel/proc.c's sched(), but the documented
// correct pairing with EM=0 regardless). CR4.OSFXSR=1 is required for
// FXSAVE/FXRSTOR and any SSE instruction to be legal at all; OSXMMEXCPT
// =1 routes SSE floating-point exceptions through #XM rather than #UD.
// Needed at all because gui/libgui/ttf.c (GUI roadmap ToaruOS-style
// rewrite's real antialiased TrueType text, vendor/stb_truetype.h) is
// the first floating-point code this kernel has ever had to run in
// user space - see kernel/proc.c's fpu_state/fpu_save()/fpu_restore()
// for the other half (per-process state survives a context switch).
#define CR0_EM (1 << 2)
#define CR0_MP (1 << 1)
#define CR4_OSFXSR    (1 << 9)
#define CR4_OSXMMEXCPT (1 << 10)

// Shared clean FXSAVE-image template (kernel/x86.asm's
// fpu_clean_template()) - kernel/proc.c's allocproc() memcpy()s this
// into every new process's own fpu_state, rather than leaving it
// zeroed (see fpu_clean_template's own comment for why that would be
// unsafe). Rebuilt redundantly-but-harmlessly by every CPU's fpuinit()
// call (the values are architecturally identical on every core) rather
// than gated to the boot CPU only, to sidestep any ordering dependency
// on exactly when each AP finishes coming up relative to the BSP.
uchar fpu_template[512] __attribute__((aligned(16)));

void
fpuinit(void)
{
  uint64 cr0, cr4;

  cr0 = rcr0();
  cr0 &= ~(uint64)CR0_EM;
  cr0 |= CR0_MP;
  lcr0(cr0);

  cr4 = rcr4();
  cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
  lcr4(cr4);

  fpu_clean_template(fpu_template);
}

// Reprograms PAT7 (bits 56-63 of the IA32_PAT MSR) to the
// Write-Combining memory type (encoding 1) - read-modify-write, not a
// blind overwrite, so whatever a real BIOS/QEMU firmware already left
// in the other 7 entries survives untouched. Must run on every CPU
// (IA32_PAT is per-core state, like fpuinit()'s CR0/CR4 bits) before
// any page is ever mapped PTE_WC (kernel/sysproc.c's sys_mmap()
// FRAMEBUFFER branch) - otherwise PAT index 7 would still mean
// whatever its power-up default is (UC), and a WC-requested mapping
// would silently map uncached instead. Same PAT7=WC encoding ToaruOS's
// own pat_initialize() uses.
#define MSR_IA32_PAT 0x277

void
patinit(void)
{
  uint64 pat = rdmsr(MSR_IA32_PAT);

  pat |= (uint64)1 << 56;         // set bit 56
  pat &= ~(((uint64)3) << 57);    // clear bits 57-58
  wrmsr(MSR_IA32_PAT, pat);
}

// Descend one level of a multi-level page table: return the next-level
// table that table[idx] refers to, allocating and zeroing a fresh page
// for it first if none exists yet and alloc is set.
static pte_t *
walknext(pte_t *table, uint idx, int alloc)
{
  pte_t *pgtab;

  if(table[idx] & PTE_P)
    return (pte_t*)P2V(PTE_ADDR(table[idx]));
  if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
    return 0;
  memset(pgtab, 0, PGSIZE);
  // The permissions here are overly generous, but they can be further
  // restricted by the permissions in the leaf page table entry, if
  // necessary.
  table[idx] = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  return pgtab;
}

// Return the address of the PD entry for va (walking PML4->PDPT->PD,
// allocating missing intermediate pages if alloc), without descending
// to a PT. Used both by walkpgdir below (one more level, to a 4KB PTE)
// and directly by mappages for a 2MB PS-bit leaf, which stops here.
static pte_t *
walkpd(pde_t *pgdir, const void *va, int alloc)
{
  pte_t *pdpt, *pd;

  if((pdpt = walknext((pte_t*)pgdir, PML4X(va), alloc)) == 0)
    return 0;
  if((pd = walknext(pdpt, PDPTX(va), alloc)) == 0)
    return 0;
  return &pd[PDX(va)];
}

// Return the address of the PTE in page table pgdir (really its PML4)
// that corresponds to virtual address va, walking all four levels and
// allocating any missing intermediate table page along the way (if
// alloc).
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pte_t *pde, *pt;

  if((pde = walkpd(pgdir, va, alloc)) == 0)
    return 0;
  if(*pde & PTE_P){
    pt = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pt = (pte_t*)kalloc()) == 0)
      return 0;
    memset(pt, 0, PGSIZE);
    *pde = V2P(pt) | PTE_P | PTE_W | PTE_U;
  }
  return &pt[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not be
// page-aligned.
//
// Uses a 2MB (PS-bit) PD-level leaf instead of a 4KB PTE wherever va,
// pa, and the remaining range are all 2MB-aligned, needing no PT page
// at all for that chunk - falling back to 4KB pages for any unaligned
// leading/trailing remainder. This matters: setupkvm() below installs
// kmap[]'s ~224MB kernel-memory range and ~32MB DEVSPACE range in
// *every* process's page table, so without it each fork() would cost
// on the order of a hundred extra kalloc()'d PT pages just for those
// two entries alone - with NPROC(param.h)-scale fork/exit churn, that
// exhausts physical memory long before anything else does. (allocuvm's
// user-memory calls are always exactly PGSIZE at a time, so they never
// qualify for the 2MB path regardless - this only ever fires for the
// large, naturally-aligned kernel ranges.)
static int
mappages(pde_t *pgdir, void *va, uintp size, uintp pa, int perm)
{
  char *a, *last, *end;
  pte_t *pde, *pte;
  // PTE_WC (include/mmu.h) is a transient request, never a real
  // hardware bit: strip it out of what actually gets OR'd into every
  // leaf entry below, and instead OR in whichever hardware PWT/PCD/PAT
  // combination selects PAT index 7 (patinit() above sets that entry
  // to Write-Combining) - a 4KB PTE and a 2MB PS-bit PDE disagree on
  // where the PAT bit lives (bit 7 vs bit 12), so each leaf path below
  // computes its own hw_wc.
  int wc = perm & PTE_WC;

  perm &= ~PTE_WC;

  a = (char*)PGROUNDDOWN((uintp)va);
  last = (char*)PGROUNDDOWN(((uintp)va) + size - 1);
  end = (char*)((uintp)va + size);
  for(;;){
    if((uintp)a % PGSIZE2M == 0 && pa % PGSIZE2M == 0 &&
       (uintp)a + PGSIZE2M <= (uintp)end){
      int hw_wc = wc ? (PTE_PWT | PTE_PCD | (1 << 12)) : 0;

      if((pde = walkpd(pgdir, a, 1)) == 0)
        return -1;
      if(*pde & PTE_P)
        panic("remap");
      *pde = pa | perm | hw_wc | PTE_P | PTE_PS;
      a += PGSIZE2M;
      pa += PGSIZE2M;
      if((uintp)a > (uintp)last)
        break;
      continue;
    }
    {
      int hw_wc = wc ? (PTE_PWT | PTE_PCD | (1 << 7)) : 0;

      if((pte = walkpgdir(pgdir, a, 1)) == 0)
        return -1;
      if(*pte & PTE_P)
        panic("remap");
      *pte = pa | perm | hw_wc | PTE_P;
    }
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uintp phys_start;
  uintp phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 // kmap[1]/kmap[2]'s phys_start/phys_end can no longer be filled in
 // here: V2P() (memlayout.h) depends on kernbase_paddr, a value only
 // known at boot time (Limine picks it, unlike the old, removed BIOS
 // boot loader's fixed EXTMEM), so it isn't a compile-time constant a
 // static initializer can use any more - kvmalloc() below patches
 // these two entries' physical bounds in before the first setupkvm()
 // call ever reads them.
 { (void*)KERNLINK, 0, 0, 0},     // kern text+rodata
 { (void*)data,     0, 0, PTE_W}, // kern data+memory
 // "up to the top of 32-bit physical address space" - uintp doesn't
 // wrap at 32 bits, so this end has to be written out explicitly
 // rather than relying on a phys_end=0/wraparound trick.
 { (void*)DEVSPACE, DEVSPACE,      0x100000000, PTE_W}, // more devices
 // ramdisk - phys_start/phys_end patched from ramdisk_paddr/
 // ramdisk_size below, the same way kmap[1]/kmap[2] are patched from
 // kernbase_paddr (neither is known at compile time - Limine picks
 // both). RAMDISK_VBASE (memlayout.h), not HW_P2V(ramdisk_paddr): see
 // its own comment for why that wraps for a module Limine places
 // above 2GB physical, and this entry - like every other kmap[] one,
 // unlike the old, boot-time-only kmapphys() call this replaces - is
 // installed in *every* process's page table, not just whichever one
 // happened to be active when ideinit() ran.
 { (void*)RAMDISK_VBASE, 0, 0, PTE_W},
};

// Map [va, va+size) in a *user* process's own page table directly to
// physical range [pa, pa+size) - a thin exported wrapper around the
// file-static mappages() above, the same idea as kmapphys() below for
// the kernel's own page table. The one caller today is kernel/
// sysproc.c's sys_mmap() FRAMEBUFFER path: the one case mmap() needs
// to hand a process a specific physical address (real VRAM) it doesn't
// otherwise control, rather than a fresh kalloc()'d page or a file's
// own content.
int
mapuvm_phys(pde_t *pgdir, uintp va, uintp size, uintp pa, int perm)
{
  return mappages(pgdir, (void*)va, size, pa, perm);
}

// The direct map's own PML4 entry (DMAP_VBASE's own comment,
// memlayout.h), cached after the first pgdir ever builds it - 0 (an
// impossible value for a present PTE, PTE_P is bit 0) means "not built
// yet". Every later setupkvm() call just copies this one 8-byte entry
// instead of re-walking/re-allocating PDPT/PD pages, which is what
// keeps a fork()-heavy workload from re-paying the direct map's cost
// on every single process.
static pde_t dmap_pml4e = 0;

// Install the direct map (dmap_end/DMAP_VBASE, memlayout.h) into pgdir:
// build it for real the first time this is ever called (from whichever
// pgdir happens to be first - kvmalloc() below makes that kpgdir), then
// just copy the cached PML4 entry into every pgdir after that. pa starts
// at 0 and va starts at DMAP_VBASE, both already 2MB-aligned (DMAP_VBASE
// sits on a PML4-slot boundary, far coarser than 2MB), and PGROUNDUP2M
// keeps the size a 2MB multiple too - so mappages()'s fast PS-bit path
// covers the whole range with just PDPT+PD pages, never any 4KB PTEs
// (see mappages()'s and dmap_end's own comments for why that mattered).
static void
ensure_dmap(pde_t *pgdir)
{
  if(dmap_pml4e){
    pgdir[PML4X(DMAP_VBASE)] = dmap_pml4e;
    return;
  }
  if(dmap_end == 0)
    return; // limine_early_init() found no usable memory - nothing to map
  if(mappages(pgdir, (void*)DMAP_VBASE, PGROUNDUP2M(dmap_end), 0, PTE_W) < 0)
    panic("ensure_dmap: out of memory");
  dmap_pml4e = pgdir[PML4X(DMAP_VBASE)];
}

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  // No PHYSTOP-vs-DEVSPACE overlap check here: under this kernel's
  // canonical-high KERNBASE, P2V(PHYSTOP) and DEVSPACE are separated
  // by most of the 64-bit address space and can never collide (unlike
  // a modest 32-bit-style KERNBASE offset, where P2V(PHYSTOP) could
  // plausibly grow into DEVSPACE's range).
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  ensure_dmap(pgdir);
  return pgdir;
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  // The TSS descriptor is 16 bytes (two gdt[] slots) in long mode -
  // see struct segdesc64 and SEGTSS in mmu.h.
  *(struct segdesc64*)&mycpu()->gdt[SEG_TSS] =
    SEGTSS(STS_T64A, (uintp)&mycpu()->ts, sizeof(mycpu()->ts)-1, 0);
  mycpu()->ts.rsp0 = (uintp)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in rflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Free a page table and all the physical memory pages in the user
// part - and every intermediate PML4/PDPT/PD/PT page this specific
// pgdir owns, including the ones backing kmap[]'s kernel-shared
// mappings (setupkvm() rebuilds them fresh, via kalloc(), for every
// process's pgdir - see the comment on mappages' 2MB-page path - so
// they're this pgdir's own to free, not shared with any other
// process's). That means walking every one of the 512 PML4 slots, not
// just the low ones user memory and DEVSPACE populate: KERNBASE's own
// slot (PML4X(KERNBASE)=511) holds the "I/O space"/"kern text+rodata"/
// "kern data+memory" kmap[] entries.
void
freevm(pde_t *pgdir)
{
  uint i, j, k;
  pte_t *pdpt, *pd, *pt;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  // Bounded by DEVSPACE, not KERNBASE: DEVSPACE (0xFE000000, see
  // memlayout.h) is an *identity* mapping - kmap[]'s virt address for
  // it equals its own phys_start, unlike every other kmap[] entry,
  // which are KERNBASE-relative - and, numerically, it's low enough to
  // land in the same PML4 slot as ordinary user addresses (PML4X of
  // anything under 512GB is 0), not the high slot KERNBASE-relative
  // mappings use. deallocuvm() unconditionally assumes every present
  // leaf PTE it finds is KERNBASE-relative and P2V()-translates
  // accordingly; walking as far as raw KERNBASE would run it straight
  // into DEVSPACE's identity-mapped PTEs and hand kfree() a bogus,
  // wrapped-around "virtual" address computed from what's actually a
  // physical one.
  deallocuvm(pgdir, DEVSPACE, 0);

  for(i = 0; i < NPML4ENTRIES; i++){
    if(!(pgdir[i] & PTE_P))
      continue;
    pdpt = (pte_t*)P2V(PTE_ADDR(pgdir[i]));
    for(j = 0; j < NPDPTENTRIES; j++){
      if(!(pdpt[j] & PTE_P))
        continue;
      pd = (pte_t*)P2V(PTE_ADDR(pdpt[j]));
      for(k = 0; k < NPDENTRIES; k++){
        if(!(pd[k] & PTE_P))
          continue;
        // A 2MB PS-bit leaf (see mappages) - the physical memory it
        // describes isn't a kalloc()'d PT page, so there's nothing to
        // free at this level for it.
        if(pd[k] & PTE_PS)
          continue;
        pt = (pte_t*)P2V(PTE_ADDR(pd[k]));
        kfree((char*)pt);
      }
      kfree((char*)pd);
    }
    kfree((char*)pdpt);
  }
  kfree((char*)pgdir);
}

// Deallocate user pages to bring the process size from oldsz to
// newsz. Doesn't bother skipping over whole unmapped page-table
// regions in one jump - it just walks a page at a time, which costs a
// few extra (cheap) walkpgdir calls on a mostly-unmapped range but
// stays correct with no jump-shortcut logic to keep in sync with the
// real (4-level) page table structure.
int
deallocuvm(pde_t *pgdir, uintp oldsz, uintp newsz)
{
  pte_t *pte;
  uintp a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(pte && (*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      // !ISPOOLPA(pa) (memlayout.h) means this PTE was never a
      // kalloc()'d page in the first place - the framebuffer's real
      // physical VRAM, mapped in by kernel/sysproc.c's sys_mmap()
      // FRAMEBUFFER path (kernel/vm.c's mapuvm_phys()), is the one
      // thing that lands here today. kfree() itself panics on any
      // address outside the pool (kernel/kalloc.c) - calling it on
      // device memory that was never part of the free pool would
      // either corrupt that pool or hit that exact panic the moment
      // any process holding such a mapping exits/execs/shrinks. Just
      // drop the mapping; there is nothing to return to the allocator.
      //
      // PTE_SHM (include/mmu.h) is the same idea for a different
      // reason: kernel/shm.c's shared-memory pages *are* ordinary
      // kalloc()'d RAM (always ISPOOLPA()), but - unlike every other
      // anonymous page reaching this loop - may still be mapped into a
      // second process's page table too. Freeing one process's mapping
      // here would yank memory out from under whoever else has it
      // mapped; the actual free happens once, in shmclose(), only
      // after the shm object's own struct file ref count (shared
      // across every fd/process referencing it) reaches zero.
      if(ISPOOLPA(pa) && !(*pte & PTE_SHM)){
        char *v = P2V(pa);
        kfree(v);
      }
      *pte = 0;
    }
  }
  return newsz;
}


// Make sure [pa, pa+size) is readable/writable at its own HW_P2V(pa)
// virtual address in kpgdir, leaving any page that's already mapped
// there alone (unlike mappages(), which panics on a remap - callers
// like kernel/acpi.c's table walk map the same table's address twice,
// once by a conservative guess and once by its real length once
// that's known, and the two can overlap). HW_P2V, not P2V: pa here is
// some arbitrary physical address unrelated to kernbase_paddr (ACPI
// tables are placed by firmware whether or not there's even a kernel
// mapped nearby; kernel/ide.c's ramdisk_paddr is a Limine module,
// independently placed from this kernel's own image too) - see
// memlayout.h's own comment on HW_P2V/V2P vs the kernbase_paddr-
// relative pair for why these need the fixed-KERNBASE-offset one.
//
// Needed because kmap[]'s "kern data+memory" entry (see kmap[] above)
// only identity-maps the managed pool, [kernbase_paddr,
// kernbase_paddr+PHYSTOP): real hardware and QEMU alike are free to
// place ACPI's tables anywhere in RAM, and Limine is free to place a
// module (kernel/ide.c's ramdisk_paddr) anywhere too - neither is
// guaranteed to land inside that pool window.
void
kmapphys(uintp pa, uintp size)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uintp)HW_P2V(pa));
  last = (char*)PGROUNDDOWN((uintp)HW_P2V(pa) + size - 1);
  for(;; a += PGSIZE){
    if((pte = walkpgdir(kpgdir, a, 1)) == 0)
      panic("kmapphys: out of memory");
    if(!(*pte & PTE_P))
      *pte = HW_V2P(a) | PTE_W | PTE_P;
    if(a == last)
      break;
  }
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  // See kmap[]'s own comment: these two entries' physical bounds need
  // kernbase_paddr, not known until runtime, so they're patched in
  // here rather than in kmap[]'s own static initializer.
  kmap[1].phys_start = V2P(KERNLINK);
  kmap[1].phys_end   = V2P(data);
  kmap[2].phys_start = V2P(data);
  kmap[2].phys_end   = pool_end;
  kmap[4].phys_start = ramdisk_paddr;
  kmap[4].phys_end   = ramdisk_paddr + ramdisk_size;

  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir. The pages from PGROUNDDOWN(addr)
// to addr+sz must already be mapped - addr itself need not be page-
// aligned (every existing caller's addr always has been, so this is a
// strict generalization: with addr page-aligned, off below is always
// 0 and boundary always PGSIZE, so the loop is byte-for-byte what it
// always was). A non-page-aligned addr is exactly what a shared
// object's second-and-later PT_LOAD segment usually has (see
// execve()'s interpreter-loading loop in kernel/exec.c): ELF only
// requires p_vaddr and p_offset to be congruent mod the segment's
// alignment, not that p_vaddr itself lands on a page boundary, and
// two consecutive PT_LOAD segments legitimately share the one page
// straddling their boundary.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uintp i, pa, n, off, boundary;
  pte_t *pte;

  for(i = 0; i < sz; i += n){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    off = ((uintp)addr + i) % PGSIZE;
    boundary = PGSIZE - off;
    n = (sz - i < boundary) ? sz - i : boundary;
    if(readi(ip, P2V(pa) + off, offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Zero out len bytes starting at addr, exactly like loaduvm above but
// filling with zeros instead of reading from an inode - the fd == -1
// (anonymous) counterpart to loaduvm, for sys_mmap()'s MAP_FIXED
// overlay path (kernel/sysproc.c): the range must already be mapped
// (typically by an earlier growproc()/allocuvm() reservation, which
// zero-fills on its own - this is for reusing that same reservation a
// second time, e.g. because a shared library's segments no longer
// cover a stale byte range a previous overlay of the same reservation
// left non-zero).
void
uvmzero(pde_t *pgdir, uintp addr, uintp len)
{
  uintp i, pa, n;
  pte_t *pte;

  if(addr % PGSIZE != 0)
    panic("uvmzero: addr must be page aligned");
  for(i = 0; i < len; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (char*)(addr+i), 0)) == 0)
      panic("uvmzero: address should exist");
    pa = PTE_ADDR(*pte);
    n = (len - i < PGSIZE) ? len - i : PGSIZE;
    memset(P2V(pa), 0, n);
  }
}

// Set or clear PTE_W on every already-mapped page in [addr, addr+len)
// (both must be page-aligned; callers round). Leaves PTE_P/PTE_U
// alone - poc-os's page tables track no NX-style execute-disable bit
// at all, so this is the only permission bit mprotect()/mmap() can
// meaningfully change. See sys_mprotect() and sys_mmap()'s MAP_FIXED
// path in kernel/sysproc.c.
void
uvmsetperm(pde_t *pgdir, uintp addr, uintp len, int writable)
{
  uintp a;
  pte_t *pte;

  for(a = addr; a < addr + len; a += PGSIZE){
    if((pte = walkpgdir(pgdir, (char*)a, 0)) == 0)
      panic("uvmsetperm: address should exist");
    if(writable)
      *pte |= PTE_W;
    else
      *pte &= ~PTE_W;
  }
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uintp oldsz, uintp newsz)
{
  char *mem;
  uintp a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uintp sz)
{
  pde_t *d;
  pte_t *pte;
  uintp pa, i, flags;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    // Device memory (the framebuffer's real VRAM, mapped by
    // kernel/sysproc.c's sys_mmap() FRAMEBUFFER branch - always
    // !ISPOOLPA(pa) (memlayout.h), the same test deallocuvm() below
    // already uses to recognize "not ordinary kalloc()'d RAM, don't
    // kfree() it") is shared with the child directly, by mapping the
    // same physical page again, rather than copied: real mmap()'d
    // device memory stays shared across fork() on a real OS (the child
    // sees the same live VRAM, not a frozen snapshot from fork() time),
    // and copying it here isn't just semantically wrong but was
    // impossible outright - P2V(pa) (include/memlayout.h) for a VRAM
    // address like this (e.g. a real BIOS/QEMU VBE framebuffer at
    // 0xfd000000) is nowhere near kernbase_paddr, so the result is a
    // bogus source pointer for the memmove() below and panics the
    // kernel on the very next fork() any process with the framebuffer
    // mapped ever made - found via a real GDB backtrace (no prior
    // test program had ever both mmap()'d the framebuffer and called
    // fork(), so this was never exercised before GUI roadmap phase 7
    // (libguitest.c) did both).
    if(!ISPOOLPA(pa)){
      if(mappages(d, (void*)i, PGSIZE, pa, PTE_FLAGS(*pte)) < 0)
        goto bad;
      continue;
    }
    // PTE_SHM (include/mmu.h) is stripped here: an shm mapping stays
    // shared through its own shmobj refcount (kernel/shm.c), not this
    // page's own pageref[] - a child that inherited one this way must
    // go back to being an ordinary COW-eligible page, not doubly
    // shared through two different mechanisms at once.
    //
    // Ordinary anonymous RAM: real copy-on-write. Mark the *parent's*
    // own live PTE read-only+COW (invlpg it - the parent keeps running
    // under its already-loaded CR3 once fork() returns, and without
    // this its TLB could still serve the old writable translation,
    // silently bypassing the new protection on its very next write),
    // then map the same physical page into the child with the same
    // flags and add its reference. vm_handle_pagefault() (called from
    // kernel/trap.c's T_PGFLT case) resolves the eventual write fault
    // by either handing back sole ownership (last remaining owner) or
    // actually copying now that there's more than one.
    flags = (PTE_FLAGS(*pte) & ~(PTE_SHM | PTE_W)) | PTE_COW;
    *pte = pa | flags;
    invlpg((void*)i);
    if(mappages(d, (void*)i, PGSIZE, pa, flags) < 0)
      goto bad;
    kaddref(pa);
  }
  return d;

bad:
  freevm(d);
  return 0;
}

// Resolve a write fault against a copy-on-write page (kernel/trap.c's
// T_PGFLT case, only for a fault from user mode). Returns -1 for
// anything that isn't ours to handle - no mapping at va at all, or a
// present-but-not-COW page (a real bug: writing to a genuinely
// read-only mapping) - leaving the caller to kill the process exactly
// as it already does for any other unhandled trap.
//
// A COW page can only ever be faulted on by a write: copyuvm() leaves
// PTE_P set, so an ordinary read already succeeds at the hardware
// level (PTE_W only gates writes) - there's no separate read-vs-write
// check needed here.
int
vm_handle_pagefault(pde_t *pgdir, uintp va)
{
  pte_t *pte;
  uintp pa;
  char *mem;

  if((pte = walkpgdir(pgdir, (void*)PGROUNDDOWN(va), 0)) == 0)
    return -1;
  if(!(*pte & PTE_P) || !(*pte & PTE_COW))
    return -1;

  pa = PTE_ADDR(*pte);

  if(kgetref(pa) == 1){
    // No other process still shares this page (a sibling already
    // dropped its own copy, or this was the only reference to begin
    // with e.g. after the partial-fork-failure case in copyuvm()) -
    // just reclaim it in place, no copy needed.
    *pte = (*pte & ~PTE_COW) | PTE_W;
    invlpg((void*)PGROUNDDOWN(va));
    return 0;
  }

  // Still shared: this process needs its own physically distinct copy
  // before it can write. Leave the fault unresolved (the process gets
  // killed on OOM, same as any other allocation failure reaching user
  // space) rather than panicking the kernel.
  if((mem = kalloc()) == 0)
    return -1;
  memmove(mem, (char*)P2V(pa), PGSIZE);
  *pte = V2P(mem) | (PTE_FLAGS(*pte) & ~PTE_COW) | PTE_W;
  invlpg((void*)PGROUNDDOWN(va));
  kfree((char*)P2V(pa)); // drop this mapping's own share of the old page
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uintp va, void *p, uintp len)
{
  char *buf, *pa0;
  uintp n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uintp)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
