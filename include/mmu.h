// This file contains definitions for the
// x86 memory management unit (MMU).

// Eflags register
#define FL_IF           0x00000200      // Interrupt Enable

// Control Register flags
#define CR0_PE          0x00000001      // Protection Enable
#define CR0_WP          0x00010000      // Write Protect
#define CR0_PG          0x80000000      // Paging

#define CR4_PSE         0x00000010      // Page size extension
#define CR4_PAE         0x00000020      // Physical Address Extension - must
                                         // be set before long mode can be
                                         // entered (see kernel/entry64.asm)

// IA32_EFER model-specific register: controls long mode. Read/written with
// rdmsr/wrmsr, not a plain control register, so these live here as plain
// constants rather than a CR4_*-style bit of a named register.
#define MSR_EFER         0xC0000080
#define EFER_LME         0x00000100     // Long Mode Enable

// IA32_FS_BASE: the hidden base address backing the %fs segment in long
// mode, where TLS lives (musl's thread pointer is just "the address
// %fs:0 reads back", see musl/arch/x86_64/pthread_arch.h's __get_tp).
// Applied per-process in kernel/trap.c, from struct proc's tls_base,
// on every return to user mode - unlike CR3, it isn't swapped by
// swtch()/context switching, so it has to be reasserted there rather
// than only when a process's tls_base first changes.
#define MSR_FS_BASE      0xC0000100

// various segment selectors.
#define SEG_KCODE 1  // kernel code
#define SEG_KDATA 2  // kernel data+stack
#define SEG_UCODE 3  // user code
#define SEG_UDATA 4  // user data+stack
#define SEG_TSS   5  // this process's task state

// cpu->gdt[NSEGS] holds the above segments. The TSS descriptor is 16
// bytes (two slots) instead of 8, since it must carry a full 64-bit
// base address - see struct segdesc64 below - so NSEGS is one more
// than a legacy 8-byte-TSS-descriptor GDT would need.
#define NSEGS     7

#ifndef __ASSEMBLER__
// Segment Descriptor. This 8-byte shape is unchanged between legacy and
// long mode - what differs is how the CPU interprets it. In particular
// the bit legacy software calls "rsv1" (bit 21) is the "L" (long) bit in
// long mode: L=1,db=0 marks a 64-bit code segment. Base/limit are checked
// for legacy code/data segments but ignored for data-segment access once
// the CPU is actually running in 64-bit mode, so the existing SEG() macro
// below is reused as-is for 64-bit data segments; SEGL() is the
// L-bit-setting variant used for the 64-bit code segment.
struct segdesc {
  uint lim_15_0 : 16;  // Low bits of segment limit
  uint base_15_0 : 16; // Low bits of segment base address
  uint base_23_16 : 8; // Middle bits of segment base address
  uint type : 4;       // Segment type (see STS_ constants)
  uint s : 1;          // 0 = system, 1 = application
  uint dpl : 2;        // Descriptor Privilege Level
  uint p : 1;          // Present
  uint lim_19_16 : 4;  // High bits of segment limit
  uint avl : 1;        // Unused (available for software use)
  uint rsv1 : 1;       // Reserved (the "L" bit in a 64-bit code segment)
  uint db : 1;         // 0 = 16-bit segment, 1 = 32-bit segment
  uint g : 1;          // Granularity: limit scaled by 4K when set
  uint base_31_24 : 8; // High bits of segment base address
};

// Normal segment
#define SEG(type, base, lim, dpl) (struct segdesc)    \
{ ((lim) >> 12) & 0xffff, (uint)(base) & 0xffff,      \
  ((uint)(base) >> 16) & 0xff, type, 1, dpl, 1,       \
  (uint)(lim) >> 28, 0, 0, 1, 1, (uint)(base) >> 24 }
#define SEG16(type, base, lim, dpl) (struct segdesc)  \
{ (lim) & 0xffff, (uint)(base) & 0xffff,              \
  ((uint)(base) >> 16) & 0xff, type, 1, dpl, 1,       \
  (uint)(lim) >> 16, 0, 0, 1, 0, (uint)(base) >> 24 }
// Flat 64-bit code segment: base/limit are irrelevant in long mode (kept
// as a full 4K-granular flat segment for well-formedness), L=1 (rsv1),
// db=0 - the combination that means "64-bit code segment".
#define SEGL(type, dpl) (struct segdesc) \
{ 0xffff, 0, 0, type, 1, dpl, 1, 0xf, 0, 1, 0, 1, 0 }
#endif

#define DPL_USER    0x3     // User DPL

// Application segment type bits
#define STA_X       0x8     // Executable segment
#define STA_W       0x2     // Writeable (non-executable segments)
#define STA_R       0x2     // Readable (executable segments)

// System segment type bits
#define STS_T64A    0x9     // Available 64-bit TSS
#define STS_IG64    0xE     // 64-bit Interrupt Gate
#define STS_TG64    0xF     // 64-bit Trap Gate

// A 64-bit virtual address 'la' has a five-part structure:
//
// +--9--+--9--+--9--+--9--+------12------+
// |PML4X|PDPTX| PDX | PTX | Page offset  |
// +-----+-----+-----+-----+--------------+
//
// Each level has 512 entries (9 bits), since entries are 8 bytes wide -
// a page table page is still exactly one 4096-byte page.
#define PML4SHIFT       39
#define PDPTSHIFT       30
#define PDXSHIFT        21      // offset of PDX in a linear address
#define PTXSHIFT        12      // offset of PTX in a linear address

#define PML4X(va)       (((uintp)(va) >> PML4SHIFT) & 0x1FF)
#define PDPTX(va)       (((uintp)(va) >> PDPTSHIFT) & 0x1FF)
#define PDX(va)         (((uintp)(va) >> PDXSHIFT) & 0x1FF)
#define PTX(va)         (((uintp)(va) >> PTXSHIFT) & 0x1FF)

#define NPML4ENTRIES    512
#define NPDPTENTRIES    512
#define NPDENTRIES      512     // # directory entries per page directory
#define NPTENTRIES      512     // # PTEs per page table
#define PGSIZE          4096    // bytes mapped by a page
#define PGSIZE2M        0x200000  // bytes mapped by a 2MB (PS-bit) PD entry

#define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE-1))
#define PGROUNDUP2M(sz) (((sz)+PGSIZE2M-1) & ~(PGSIZE2M-1))

// Page table/directory entry flags.
#define PTE_P           0x001   // Present
#define PTE_W           0x002   // Writeable
#define PTE_U           0x004   // User
#define PTE_PS           0x080   // Page Size
// Bit 9: one of the x86 PTE format's "available for OS use" bits
// (9-11), otherwise unused by hardware or by this kernel. Marks a leaf
// PTE as pointing at a refcounted, possibly multiply-mapped shm page
// (kernel/shm.c) rather than a normal single-owner anonymous page -
// see kernel/vm.c's deallocuvm(), which must skip kfree()ing these
// (freeing happens instead in shmclose() once the object's own refcnt
// hits 0 - the same reason phase 2's framebuffer mapping needed a
// similar deallocuvm() skip, just via a PHYSTOP check there since VRAM
// is never < PHYSTOP to begin with, unlike shm's ordinary kalloc()'d
// pages).
#define PTE_SHM          0x200

// Hardware cache-control bits (Page Write-Through / Page Cache-Disable) -
// combined with a leaf entry's PAT bit (a different position for a 4KB
// PTE vs a 2MB PS-bit PDE - see mappages()' PTE_WC handling in
// kernel/vm.c) to select one of the 8 PAT-MSR-defined memory types.
#define PTE_PWT          0x008
#define PTE_PCD          0x010

// Bit 10: another "available for OS use" bit, distinct from PTE_SHM
// above. Transient only - passed into mappages()'s perm argument to
// request the write-combining memory type (kernel/vm.c's patinit()
// sets up PAT7 = WC for exactly this), and translated into the real
// hardware PWT/PCD/PAT bits by mappages() before the PTE is ever
// stored, so it never appears in a live PTE. See kernel/sysproc.c's
// sys_mmap() FRAMEBUFFER branch, the one caller.
#define PTE_WC           0x400

// Bit 11: persistent marker for a copy-on-write page (kernel/vm.c's
// copyuvm()/vm_handle_pagefault()) - present alongside PTE_P but
// without PTE_W on a page a fork() shared (refcounted, see
// kernel/kalloc.c's pageref[]) rather than copied. A write fault on
// such a page is what vm_handle_pagefault() (kernel/trap.c's T_PGFLT
// case) resolves, either by granting sole ownership back or by
// actually copying now that a second owner exists.
#define PTE_COW          0x800

// Address in page table or page directory entry
#define PTE_ADDR(pte)   ((uintp)(pte) & ~0xFFF)
#define PTE_FLAGS(pte)  ((uintp)(pte) &  0xFFF)

#ifndef __ASSEMBLER__
typedef uintp pte_t;

// Task state segment format used by long-mode hardware to find the
// kernel stack on a privilege-level change (rsp0) - there's no separate
// stack per privilege level below 0 the way the legacy TSS had (esp1/2),
// since poc only ever runs kernel code at CPL 0. IST slots (interrupt
// stack table) are left unused (0 = "don't switch stacks"). The field
// offsets here are fixed by hardware, not just convention, hence packed:
// notably reserved0 (4 bytes) is immediately followed by rsp0 at byte
// offset 4, not the 8-byte-aligned offset a plain struct would place it.
struct taskstate {
  uint reserved0;
  uint64 rsp0;
  uint64 rsp1;
  uint64 rsp2;
  uint64 reserved1;
  uint64 ist1;
  uint64 ist2;
  uint64 ist3;
  uint64 ist4;
  uint64 ist5;
  uint64 ist6;
  uint64 ist7;
  uint64 reserved2;
  ushort reserved3;
  ushort iomb;       // I/O map base address
} __attribute__((packed));

// A 64-bit system-segment descriptor (used only for the TSS) is 16 bytes -
// twice the width of struct segdesc above, since it needs a full 64-bit
// base address - and so occupies two consecutive slots of cpu->gdt.
struct segdesc64 {
  uint lim_15_0 : 16;
  uint base_15_0 : 16;
  uint base_23_16 : 8;
  uint type : 4;
  uint s : 1;
  uint dpl : 2;
  uint p : 1;
  uint lim_19_16 : 4;
  uint avl : 1;
  uint rsv1 : 2;
  uint g : 1;
  uint base_31_24 : 8;
  uint base_63_32;
  uint rsv2;
} __attribute__((packed));

#define SEGTSS(type, base, lim, dpl) (struct segdesc64) \
{ (lim) & 0xffff, (uintp)(base) & 0xffff,                \
  ((uintp)(base) >> 16) & 0xff, type, 0, dpl, 1,          \
  ((lim) >> 16) & 0xf, 0, 0, 0, ((uintp)(base) >> 24) & 0xff, \
  (uint)((uintp)(base) >> 32), 0 }

// Gate descriptor for interrupts and traps in long mode: 16 bytes instead
// of 8, since - like segdesc64 above - it needs a full 64-bit handler
// offset. ist selects an alternate stack via the TSS's ist1..7 (0 = use
// the current stack / TSS.rsp0 on a privilege change, which is all poc
// needs).
struct gatedesc {
  uint off_15_0 : 16;   // low 16 bits of handler offset
  uint cs : 16;         // code segment selector
  uint ist : 3;         // interrupt stack table index, 0 = none
  uint rsv0 : 5;         // reserved
  uint type : 4;        // type(STS_{IG64,TG64})
  uint s : 1;           // must be 0 (system)
  uint dpl : 2;         // descriptor privilege level
  uint p : 1;           // Present
  uint off_31_16 : 16;  // middle bits of handler offset
  uint off_63_32;       // high bits of handler offset
  uint rsv1;            // reserved
};

#define SETGATE(gate, istrap, sel, off, d)                \
{                                                          \
  (gate).off_15_0 = (uintp)(off) & 0xffff;                 \
  (gate).cs = (sel);                                       \
  (gate).ist = 0;                                          \
  (gate).rsv0 = 0;                                         \
  (gate).type = (istrap) ? STS_TG64 : STS_IG64;            \
  (gate).s = 0;                                            \
  (gate).dpl = (d);                                        \
  (gate).p = 1;                                             \
  (gate).off_31_16 = ((uintp)(off) >> 16) & 0xffff;         \
  (gate).off_63_32 = (uint)((uintp)(off) >> 32);            \
  (gate).rsv1 = 0;                                          \
}

#endif
