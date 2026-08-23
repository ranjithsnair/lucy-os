; The poc kernel starts executing in this file - reached directly at its
; real (already higher-half) virtual entry point, in 64-bit long mode,
; paging already on: Limine (kernel/kernel.ld's .limine_requests section,
; kernel/limine.c) has already done everything the old BIOS bootloader
; plus this file itself used to have to do by hand (real->protected->
; long mode, GDT, PAE/EFER/CR0) before ever handing control to the
; kernel - so unlike that old flow, execution here never needs to leave
; 64-bit mode, and there's no separate low/high addressing split to
; cross (no more V2P_WO() entry-point alias, no more far-jump-through-a-
; new-GDT, no more indirect low->high jump): every label in this file is
; already at its correct, already-mapped, linked virtual address from
; the very first instruction.
;
; The boot-time PML4/PDPT/PD this kernel still needs (entrypml4 and
; friends, used both by kinit1() before kvmalloc()'s real page table
; takes over, and by kernel/main.c's startothers() for AP bring-up) is
; built in C now, not here - see kernel/limine.c's limine_entry_init().
; The reason: unlike the old BIOS boot loader (which always placed the
; kernel at a fixed physical address, EXTMEM/memlayout.h), Limine
; chooses the kernel's physical load address itself (discovered at
; runtime via the executable-address request) - found the hard way
; (a boot hang chased down with VirtualBox's built-in debugger) after
; this file first tried to hardcode the old fixed-physical-address
; assumption the same way the removed BIOS loader's own entry.asm did,
; which produced page tables mapping the wrong physical range entirely
; once Limine placed this kernel somewhere other than EXTMEM. Building
; the table in C avoids re-deriving that runtime value by hand in NASM.
;
; limine_early_init() runs first, strictly before limine_entry_init():
; every Limine response pointer (module/framebuffer/HHDM/...) is only
; valid to dereference under Limine's own page tables, which
; limine_entry_init() replaces (the CR3 switch above) - found the hard
; way too, as a second boot hang right after fixing the first one, once
; execution reached main()'s own call into what used to be here.
; limine_early_init() unpacks everything this kernel needs from those
; responses into plain globals first, while Limine's tables are still
; active, so nothing later needs to dereference a Limine pointer again.

#include "param.h"

BITS 64

extern main
extern limine_early_init
extern limine_entry_init

global entry
entry:
  ; A real 64-bit stack, before calling into C - the same stack main()
  ; (jumped to below) itself runs on.
  mov rsp, (stack + KSTACKSIZE)
  call limine_early_init
  call limine_entry_init

  mov rax, main
  jmp rax

section .bss
common stack KSTACKSIZE
