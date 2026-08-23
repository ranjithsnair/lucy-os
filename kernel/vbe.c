// Linear-framebuffer info: the kernel-global `vbe` every other
// framebuffer-facing piece (kernel/sysproc.c's sys_mmap()/sys_ioctl()
// FRAMEBUFFER paths) reads from - unchanged since the old real-mode
// VBE-probe days (include/vbe.h), so none of those consumers needed to
// change. Filled in by kernel/limine.c's limine_early_init(), not here:
// that has to happen while Limine's own page tables (the only ones
// framebuffer_request.response is valid to dereference under) are still
// active, well before vbeinit() below runs. This function just reports
// it - kept as a separate, later init step (called after uartinit(),
// unlike limine_early_init()) purely so the report below reaches serial.
#include "types.h"
#include "defs.h"
#include "memlayout.h"
#include "vbe.h"

struct vbeinfo vbe;

void
vbeinit(void)
{
  // Graceful degrade to text-only console, same as the old VBE probe's
  // own failure path - not every machine/emulator Limine boots on has a
  // usable linear framebuffer, and that's not fatal to booting poc-os
  // itself (unlike the ramdisk - see kernel/limine.c's
  // limine_early_init()).
  if(vbe.magic != VBE_INFO_MAGIC){
    cprintf("vbeinit: no usable framebuffer - text console only\n");
    return;
  }

  cprintf("vbeinit: %dx%d %dbpp framebuffer at 0x%x (pitch %d)\n",
          vbe.xres, vbe.yres, vbe.bpp, vbe.phys_base, vbe.pitch);
}
