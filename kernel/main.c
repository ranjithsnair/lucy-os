// Kernel entry point (reached from kernel/entry.asm) and multiprocessor
// bring-up: main() runs on the boot CPU and initializes every subsystem
// in dependency order, then wakes the other CPUs (startothers()), each
// of which lands in mpenter() below and runs its own much shorter
// per-CPU setup (mpmain()).

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void startothers(void);
static void mpmain(void)  __attribute__((noreturn));
extern pde_t *kpgdir;
extern char end[]; // first address after kernel loaded from ELF file
extern char data[]; // defined by kernel.ld

// Bootstrap processor starts running C code here.
// Allocate a real stack and switch to it, first
// doing some setup required for memory allocator to work.
int
main(void)
{
  // limine_early_init()/limine_entry_init() (kernel/limine.c) have
  // already run by this point - see kernel/entry.asm - unpacking
  // Limine's boot-time answers (including ramdisk_paddr/ramdisk_size,
  // kernel/ide.c, which kinit1()/kinit2() below need to keep from
  // handing the ramdisk's own pages out as free memory) and switching
  // to this kernel's own page table, both of which have to happen
  // before any of this runs.
  // The managed pool is anchored at kernbase_paddr now, not physical 0
  // - see memlayout.h's own comment on kernbase_paddr/V2P/P2V for why -
  // and kernel/entry.asm's bootstrap page table (still active here;
  // kvmalloc() below hasn't installed the real one yet) maps the first
  // 4MB past kernbase_paddr. But pool_end (kernel/limine.c's own
  // comment) can be *less* than a full 4MB past kernbase_paddr - Limine
  // is free to place the kernel close enough to the true top of RAM
  // that there isn't a full 4MB of real memory left, and kinit1() must
  // never freerange() past wherever real RAM actually ends.
  uintp kinit1_end = kernbase_paddr + 4*1024*1024;
  if(kinit1_end > pool_end)
    kinit1_end = pool_end;
  kinit1(end, P2V(kinit1_end), ramdisk_paddr, ramdisk_size); // phys page allocator
  kvmalloc();      // kernel page table
  mpinit();        // detect other processors
  lapicinit();     // interrupt controller
  seginit();       // segment descriptors
  fpuinit();       // FPU/SSE (must come before userinit() creates the
                    // first process - see kernel/proc.c's allocproc())
  patinit();       // PAT MSR: PAT7 = write-combining, for the real
                    // framebuffer (kernel/sysproc.c's sys_mmap())
  picinit();       // disable pic
  ioapicinit();    // another interrupt controller
  consoleinit();   // console hardware
  uartinit();      // serial port
  vbeinit();       // linear framebuffer, if Limine found one - after
                   // uartinit() so its result line reaches serial too
  mouseinit();     // PS/2 mouse (GUI roadmap phase 3)
  pinit();         // process table
  tvinit();        // trap vectors
  binit();         // buffer cache
  fileinit();      // file table
  sockinit();      // AF_UNIX socket table (GUI roadmap phase 3)
  shminit();       // shared-memory object table (GUI roadmap phase 3)
  ideinit();       // disk
  startothers();   // start other processors
  // Spans the rest of physical memory kinit1() didn't already cover
  // (unlike the original xv6 kinit2 call this replaces, which just
  // started right past a *fixed* low ramdisk address) - freerange_except()
  // (kernel/kalloc.c) excludes wherever ramdisk_paddr/ramdisk_size
  // actually landed, since Limine (unlike the old BIOS boot loader) picks
  // that address itself: fs.img's pages have to stay put for the
  // ramdisk's entire lifetime, never handed out as ordinary free memory.
  kinit2(P2V(kinit1_end), P2V(pool_end), ramdisk_paddr, ramdisk_size); // must come after startothers()
  // Extends the pool with whatever other USABLE RAM Limine's memmap
  // reports beyond the main pool above (kernel/limine.c's own comment) -
  // must come after kinit2() (the main pool has to already own its own
  // range before this treats anything else as free) and after kvmalloc()
  // (already true by now - this needs the direct map it installs).
  dmap_init_pool();
  userinit();      // first user process
  mpmain();        // finish this processor's setup
}

// Common CPU setup code.
static void
mpmain(void)
{
  cprintf("cpu%d: starting %d\n", cpuid(), cpuid());
  idtinit();       // load idt register
  xchg(&(mycpu()->started), 1); // tell startothers() we're up
  scheduler();     // start running processes
}

// Other CPUs jump here (in 64-bit mode, courtesy of kernel/entryother.asm)
// once startothers() below wakes them up.
static void
mpenter(void)
{
  switchkvm();
  seginit();
  fpuinit();       // this AP's own CR0/CR4 - see main()'s own call site
  patinit();       // this AP's own PAT MSR - see main()'s own call site
  lapicinit();
  mpmain();
}

// The boot-time page table kernel/entry.asm built, for use in
// startothers() below.
extern pde_t entrypml4[];

// Start the non-boot (AP) processors.
static void
startothers(void)
{
  extern uchar _binary_build_entryother_start[], _binary_build_entryother_size[];
  uchar *code;
  struct cpu *c;
  char *stack;

  // Write entry code to unused memory at 0x7000. The linker has placed
  // the image of entryother.asm in _binary_build_entryother_start (the
  // symbol name is derived from the build/entryother path given to
  // ld's -b binary option).
  code = HW_P2V(0x7000);
  memmove(code, _binary_build_entryother_start, (uint64)_binary_build_entryother_size);

  for(c = cpus; c < cpus+ncpu; c++){
    if(c == mycpu())  // We've started already.
      continue;

    // Tell entryother.asm what stack to use, where to enter, and what
    // pgdir to use. We cannot use kpgdir yet, because the AP processor
    // is running in low memory, so we use entrypml4 for the APs too -
    // see the comment atop kernel/entryother.asm.
    stack = kalloc();
    *(uint64*)(code-8)  = (uint64)stack + KSTACKSIZE;
    *(uint64*)(code-16) = (uint64)mpenter;
    *(uint64*)(code-24) = (uint64)V2P(entrypml4);

    lapicstartap(c->apicid, (uint)HW_V2P(code));

    // wait for cpu to finish mpmain()
    while(c->started == 0)
      ;
  }
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

