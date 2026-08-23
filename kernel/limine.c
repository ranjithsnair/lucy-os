// Limine boot protocol glue: the requests Limine reads out of this
// binary's own .limine_requests section (see kernel/kernel.ld) and
// answers before ever jumping to kernel/entry.asm, plus the handful of
// derived values the rest of the kernel needs from those answers -
// physical framebuffer/ramdisk info other code here used to get from
// boot/boot2_bios.asm's real-mode probes instead.
//
// Both the framebuffer and module responses hand back pointers already
// valid to dereference under Limine's own (still-active at this point -
// see kernel/entry.asm) page tables, but expressed HHDM-relative, not as
// physical addresses: this kernel's own P2V/V2P (memlayout.h) instead
// use a fixed KERNBASE offset, and kernel/sysproc.c's sys_mmap()
// framebuffer path in particular needs a genuine physical address (it
// builds PTEs from vbe.phys_base directly) - so every such pointer this
// file exposes has hhdm_offset subtracted back out first.
//
// ramdisk_paddr/ramdisk_size themselves are defined in kernel/ide.c
// (the ramdisk's real owner) - this file only fills them in, via the
// extern declarations in defs.h.

#include "types.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "vbe.h"
#include "limine.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64 limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
  .id = LIMINE_HHDM_REQUEST_ID,
  .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
  .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
  .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
  .id = LIMINE_MODULE_REQUEST_ID,
  .revision = 0,
};

// Needed to size the managed kalloc() pool (kernel/kalloc.c, kernel/
// vm.c's kmap[]) correctly - found the hard way (yet another boot
// hang, this one only reproducing on a 2GiB VM, chased down with
// VirtualBox's own "info phys" physical-memory-map dump) after this
// kernel assumed a full, fixed PHYSTOP (memlayout.h, ~224MB) worth of
// real RAM always follows wherever Limine placed the kernel - Limine
// is free to place the kernel right up against the *actual* top of
// RAM (observed: ~2MB of real headroom left on a 2GiB VM, nowhere
// near 224MB), and blindly mapping/using memory past that reads back
// as all-ones and silently drops every write, like any other
// unbacked/reserved physical range.
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
  .id = LIMINE_MEMMAP_REQUEST_ID,
  .revision = 0,
};

// Where Limine actually put this kernel physically - unlike the removed
// BIOS boot loader, which always loaded the kernel at a fixed physical
// address (EXTMEM, memlayout.h) so entry.asm could just hardcode it,
// Limine picks the physical load address itself and only guarantees the
// *virtual* mapping matches this ELF's link address (kernel/kernel.ld).
// limine_entry_init() below needs the real value: found the hard way
// (a silent boot hang, chased down with VirtualBox's built-in debugger)
// after this kernel's entry.asm first assumed physical == virtual -
// KERNBASE the same way the old boot loader let it - Limine had in fact
// placed the kernel around 128MB in, not at EXTMEM, so that assumption
// produced a boot-time page table mapping the wrong physical range
// entirely.
__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
  .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
  .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64 limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64 limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

uintp limine_hhdm_offset;
uintp kernbase_paddr; // memlayout.h's V2P()/P2V() - see its own comment
uintp pool_end; // memlayout.h's ISPOOLPA() - the managed kalloc() pool
                 // is [kernbase_paddr, pool_end), not always a full
                 // PHYSTOP past kernbase_paddr - see memmap_request's
                 // own comment.
uintp dmap_end; // memlayout.h's own comment on dmap_end/DMAP_VBASE.
extern struct vbeinfo vbe; // kernel/vbe.c

// Boot-time PML4/PDPT/PD/PT, built and switched to by limine_entry_init()
// below (called from kernel/entry.asm before main()). Two mappings:
//   - high (VA [KERNBASE,KERNBASE+4MB) -> PA [kernel_paddr,+4MB)):
//     kinit1() (kernel/kalloc.c, called right after this returns, from
//     main()) walks this whole VA range before kvmalloc()'s real page
//     table (kpgdir) takes over - Limine's own higher-half mapping only
//     covers this kernel's actual ELF image extent, not the full 4MB
//     kinit1() needs. 4KB pages, not 2MB ones: kernel_paddr is only
//     guaranteed 4KB-aligned (ELF PT_LOAD alignment), not 2MB-aligned -
//     unlike the old BIOS boot loader, which always placed the kernel
//     at a fixed, 2MB-aligned physical address (EXTMEM), Limine picks
//     kernel_paddr itself and it need not land on a 2MB boundary.
//   - identity (VA [0,4MB) -> PA [0,4MB)): kernel/main.c's
//     startothers() hands entrypml4 to each AP via kernel/
//     entryother.asm, which starts in real mode at a low physical
//     address (0x7000) and needs an identity mapping at the exact
//     instant it turns paging on itself - entirely unrelated to
//     wherever Limine put this kernel, so still safe to build as 2MB
//     pages (physical 0 and 0x200000 are always 2MB-aligned).
// Two separate PDs, not one shared between both mappings the way the
// old fixed-physical-address version got away with: the kernel's own
// physical location and the AP trampoline's (always low, near 0) are
// unrelated addresses now.
__attribute__((aligned(4096))) pde_t entrypml4[512];
__attribute__((aligned(4096))) static pde_t entrypdpt_low[512];
__attribute__((aligned(4096))) static pde_t entrypdpt_high[512];
__attribute__((aligned(4096))) static pde_t entrypd_low[512];
__attribute__((aligned(4096))) static pde_t entrypd_high[512];
__attribute__((aligned(4096))) static pte_t entrypt_high0[512];
__attribute__((aligned(4096))) static pte_t entrypt_high1[512];

// Called from kernel/entry.asm before main(), with a valid stack but
// nothing else set up yet - Limine's own page tables (whatever they
// actually are; see this file's own comment on why they can't just be
// assumed to identity-map this kernel's physical location) are still
// active on entry. Builds the boot-time page table above and switches
// to it immediately, so main()'s first calls (kinit1()) have the
// mapping they need.
void
limine_entry_init(void)
{
  // kernbase_paddr itself is computed in limine_early_init(), not here
  // - see that function's own comment on why: this kernel's pool_end
  // (also computed there) needs it too, and early_init() runs first.
  uintp kernel_paddr = kernbase_paddr + EXTMEM;

  // Physical address of a symbol *within this kernel's own image*
  // (entrypml4 and friends, all below, are - they're kernel globals).
  // memlayout.h's plain V2P() (a fixed KERNBASE subtraction) is wrong
  // for these: it assumes the kernel is loaded at EXTMEM (KERNLINK -
  // KERNBASE), which is what the old, removed BIOS boot loader always
  // guaranteed but Limine does not - found the hard way (a boot hang,
  // chased down with VirtualBox's built-in debugger and a couple of
  // core-dump byte searches) after this file first computed physical
  // addresses straight off of kernel_paddr as if it corresponded to
  // KERNBASE itself - it actually corresponds to KERNLINK
  // (executable_address_response.physical_base is defined relative to
  // this ELF's lowest PT_LOAD p_vaddr, which is KERNLINK, not KERNBASE -
  // kernel/kernel.ld links .limine_requests starting at KERNLINK) - so
  // every physical address this function computed, including the high
  // mapping's own PTEs below, was off by EXTMEM (1MB).
  #define KV2P(x) (kernel_paddr + ((uintp)(x) - KERNLINK))

  for(int i = 0; i < 512; i++){
    entrypt_high0[i] = (kernbase_paddr + (uintp)i * PGSIZE) | PTE_P | PTE_W;
    entrypt_high1[i] = (kernbase_paddr + PGSIZE2M + (uintp)i * PGSIZE) | PTE_P | PTE_W;
  }
  entrypd_high[0] = KV2P(entrypt_high0) | PTE_P | PTE_W;
  entrypd_high[1] = KV2P(entrypt_high1) | PTE_P | PTE_W;

  entrypd_low[0] = PTE_P | PTE_W | PTE_PS;
  entrypd_low[1] = PGSIZE2M | PTE_P | PTE_W | PTE_PS;

  entrypdpt_low[0] = KV2P(entrypd_low) | PTE_P | PTE_W;
  entrypdpt_high[PDPTX(KERNBASE)] = KV2P(entrypd_high) | PTE_P | PTE_W;

  entrypml4[0] = KV2P(entrypdpt_low) | PTE_P | PTE_W;
  entrypml4[PML4X(KERNBASE)] = KV2P(entrypdpt_high) | PTE_P | PTE_W;

  lcr3(KV2P(entrypml4));

  #undef KV2P
}

// Called first thing in main(), before kinit1() - kalloc.c's kinit1()/
// kinit2() need ramdisk_paddr/ramdisk_size (both set here) to exclude
// the ramdisk's actual range from the free-page pool, and nothing here
// depends on any other subsystem being initialized yet (these requests
// were already answered by Limine before it ever jumped to kernel/
// entry.asm - this function just validates and unpacks the answers).
void
limine_early_init(void)
{
  if(!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
    panic("limine_early_init: bootloader does not support our base revision");

  if(hhdm_request.response == 0)
    panic("limine_early_init: no HHDM response");
  limine_hhdm_offset = hhdm_request.response->offset;

  if(module_request.response == 0 || module_request.response->module_count < 1)
    panic("limine_early_init: no ramdisk module (fs.img) - check limine.conf");

  struct limine_file *m = module_request.response->modules[0];
  ramdisk_paddr = (uintp)m->address - limine_hhdm_offset;
  ramdisk_size = m->size;

  // kernbase_paddr has to be known before the memmap scan just below
  // (it looks up the entry *containing* kernbase_paddr), so it's
  // computed here rather than in limine_entry_init() as it used to be
  // - found the hard way (a triple fault before the IDT was even
  // loaded, console.c writing to an unmapped VGA buffer) after this
  // scan ran with kernbase_paddr still at its zero-initialized default
  // (limine_entry_init(), which used to compute it, runs strictly
  // after limine_early_init() - see kernel/entry.asm), so it matched
  // no memmap entry and silently left pool_end at its "no headroom
  // found at all" default of 0, which made every physical page look
  // out of pool range and kmap[]'s I/O-space mapping never get built.
  // executable_address_request.response, like every other Limine
  // response here, is only valid to dereference under Limine's own
  // page tables - still active at this point, same as module_request/
  // memmap_request above.
  if(executable_address_request.response == 0)
    panic("limine_early_init: no executable-address response");
  kernbase_paddr = executable_address_request.response->physical_base - EXTMEM;

  // See memmap_request's own comment: find how far past kernbase_paddr
  // real, backed RAM actually extends, capped at PHYSTOP (this kernel's
  // own ceiling on how much it's willing to manage regardless). Starts
  // from the memmap entry containing kernbase_paddr itself (normally
  // type EXECUTABLE_AND_MODULES, sized tightly around just the
  // kernel+modules Limine loaded, not the rest of RAM) and merges
  // forward through however many further entries are both contiguous
  // (no gap) and a type this kernel can treat as ordinary free RAM -
  // stopping at the first gap, reserved-type entry, or the PHYSTOP cap.
  if(memmap_request.response == 0)
    panic("limine_early_init: no memmap response");
  struct limine_memmap_entry **entries = memmap_request.response->entries;
  uint64 nentries = memmap_request.response->entry_count;
  pool_end = kernbase_paddr; // default: no headroom found at all
  for(uint64 i = 0; i < nentries; i++){
    if(kernbase_paddr >= entries[i]->base &&
       kernbase_paddr < entries[i]->base + entries[i]->length){
      uintp end = entries[i]->base + entries[i]->length;
      for(uint64 j = i + 1; j < nentries; j++){
        if(entries[j]->base != end)
          break;
        if(entries[j]->type != LIMINE_MEMMAP_USABLE &&
           entries[j]->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES &&
           entries[j]->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
          break;
        end = entries[j]->base + entries[j]->length;
      }
      pool_end = end;
      break;
    }
  }
  if(pool_end > kernbase_paddr + PHYSTOP)
    pool_end = kernbase_paddr + PHYSTOP;
  // kernel/main.c's kinit1_end already clamps to min(kernbase_paddr+4MB,
  // pool_end), so a full 4MB past the kernel isn't actually required here
  // - found the hard way (this exact panic firing on a VM with the kernel
  // loaded close enough to the top of RAM that only ~2.3MB followed it,
  // long before main() or its own diagnostics ever got a chance to run).
  // Still guard against a truly degenerate memory map with essentially no
  // headroom at all, which kinit1() couldn't do anything useful with.
  if(pool_end < kernbase_paddr + 0x100000)
    panic("limine_early_init: less than 1MB of usable RAM past the kernel");

  // dmap_end (memlayout.h's own comment): how far the direct map
  // (kernel/vm.c's build_dmap()) needs to reach to cover every USABLE
  // byte Limine's memory map reports, not just the run immediately
  // following the kernel that kernbase_paddr/pool_end above already
  // cover. Capped well above any real test machine's RAM (64GB) purely
  // as a safety bound on how many PD pages build_dmap() ever has to
  // allocate - each 1GB of dmap_end costs exactly one (2MB-page PD
  // entries, no PT pages needed - see DMAP_VBASE's own comment on why
  // alignment is never an issue here), so even the cap is cheap.
  dmap_end = 0;
  for(uint64 i = 0; i < nentries; i++){
    if(entries[i]->type != LIMINE_MEMMAP_USABLE)
      continue;
    uintp end = entries[i]->base + entries[i]->length;
    if(end > dmap_end)
      dmap_end = end;
  }
  if(dmap_end > 0x1000000000ULL) // 64GB
    dmap_end = 0x1000000000ULL;

  // Copy the framebuffer response's fields into the `vbe` global here
  // too, not in kernel/vbe.c's vbeinit() (called much later, from
  // main(), after this kernel has already switched off of Limine's own
  // page tables - see limine_entry_init()'s own comment on why) - like
  // every other Limine response pointer, framebuffer_request.response
  // is only valid to dereference under Limine's page tables, which are
  // still active here (limine_early_init() runs before
  // limine_entry_init()'s CR3 switch - see kernel/entry.asm). vbeinit()
  // itself now just reports what's already here; it's still the one
  // that prints (once uartinit() has run so the report reaches serial).
  struct limine_framebuffer *fb = 0;
  if(framebuffer_request.response != 0 &&
     framebuffer_request.response->framebuffer_count >= 1)
    fb = framebuffer_request.response->framebuffers[0];

  if(fb == 0 || fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32){
    vbe.magic = 0;
  } else {
    vbe.magic = VBE_INFO_MAGIC;
    vbe.phys_base = (uint)((uintp)fb->address - limine_hhdm_offset);
    vbe.pitch = (uint)fb->pitch;
    vbe.xres = (uint)fb->width;
    vbe.yres = (uint)fb->height;
    vbe.bpp = (uchar)fb->bpp;
    vbe.red_mask_size = fb->red_mask_size;
    vbe.red_field_pos = fb->red_mask_shift;
    vbe.green_mask_size = fb->green_mask_size;
    vbe.green_field_pos = fb->green_mask_shift;
    vbe.blue_mask_size = fb->blue_mask_size;
    vbe.blue_field_pos = fb->blue_mask_shift;
  }
}

// True iff physical page p falls inside some LIMINE_MEMMAP_USABLE
// memmap entry - dmap_end (this file's own comment on it) is only the
// *highest* such entry's end address, not a guarantee everything below
// it is real backed RAM (there can be large reserved/MMIO gaps in
// between), so dmap_init_pool() below can't just treat all of
// [0,dmap_end) as free the way freerange() treats [kernbase_paddr,
// pool_end) - that range, unlike this one, was already trimmed to a
// single contiguous USABLE run by limine_early_init() above.
static int
dmap_page_usable(struct limine_memmap_entry **entries, uint64 nentries, uintp p)
{
  for(uint64 i = 0; i < nentries; i++){
    if(entries[i]->type == LIMINE_MEMMAP_USABLE &&
       p >= entries[i]->base && p + PGSIZE <= entries[i]->base + entries[i]->length)
      return 1;
  }
  return 0;
}

// True iff physical page p already belongs to some other, already-
// managed range - the main pool (kernel/kalloc.c's pageref[], set up by
// kinit1()/kinit2() before this function ever runs - see its own call
// site, kernel/main.c) or the ramdisk (kernel/ide.c) - and so must not
// be handed to kfree() a second time here.
static int
dmap_page_excluded(uintp p)
{
  if(p >= kernbase_paddr && p < pool_end)
    return 1;
  if(p >= ramdisk_paddr && p < ramdisk_paddr + ramdisk_size)
    return 1;
  return 0;
}

// Donates every USABLE physical page in [0,dmap_end) that the main pool
// and the ramdisk don't already own to kernel/kalloc.c's allocator, via
// the direct map (dmap_end/DMAP_VBASE, memlayout.h) kernel/vm.c's
// setupkvm() already installed in kpgdir (and, from here on, every
// process's pgdir) by the time this runs - see its own call site
// (kernel/main.c's main(), right after kinit2()) for why it has to be
// this late: kvmalloc() has to have built that mapping first, and
// kinit1()/kinit2() have to have already claimed the main pool's own
// range before this treats anything as free.
//
// Needs somewhere to keep a refcount per donated page (kernel/kalloc.c's
// dmap_pageref[], kdmapreserve()'s own comment explains why that can't
// just be a second PHYSTOP-sized static array) - so this runs in two
// passes: first hunting for enough contiguous, USABLE, not-already-
// owned physical memory to hold that table itself (a linear byte-by-
// byte scan, not entry-interval math, deliberately: simple and obviously
// correct beats clever here, and even a full 64GB's worth of page-sized
// steps is a trivial one-time boot cost), then a second pass handing
// every remaining such page to kfree().
void
dmap_init_pool(void)
{
  if(memmap_request.response == 0 || dmap_end == 0)
    return;
  struct limine_memmap_entry **entries = memmap_request.response->entries;
  uint64 nentries = memmap_request.response->entry_count;

  uintp pageref_bytes = PGROUNDUP((dmap_end / PGSIZE) * sizeof(ushort));
  uintp run_start = 0, run_len = 0, pageref_paddr = 0;

  for(uintp p = 0; p < dmap_end; p += PGSIZE){
    if(dmap_page_excluded(p) || !dmap_page_usable(entries, nentries, p)){
      run_len = 0;
      continue;
    }
    if(run_len == 0)
      run_start = p;
    run_len += PGSIZE;
    if(run_len >= pageref_bytes){
      pageref_paddr = run_start;
      break;
    }
  }
  if(pageref_paddr == 0)
    return; // no single run big enough - leave the extra RAM unmanaged

  kdmapreserve(pageref_paddr, dmap_end / PGSIZE);

  for(uintp p = 0; p < dmap_end; p += PGSIZE){
    if(dmap_page_excluded(p))
      continue;
    if(p >= pageref_paddr && p < pageref_paddr + pageref_bytes)
      continue;
    if(!dmap_page_usable(entries, nentries, p))
      continue;
    kfree(DMAP_P2V(p));
  }
}
