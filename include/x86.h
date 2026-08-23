// Routines to let C code use special x86 instructions.
//
// These are implemented in kernel/x86.asm (NASM), not as inline asm here,
// so they are ordinary extern function declarations. See kernel/x86.asm
// for the actual instruction sequences and calling-convention notes.

uchar inb(ushort port);
void insl(int port, void *addr, int cnt);
void outb(ushort port, uchar data);
void outw(ushort port, ushort data);
void outsl(int port, const void *addr, int cnt);
void stosb(void *addr, int data, int cnt);
void stosl(void *addr, int data, int cnt);

struct segdesc;
void lgdt(struct segdesc *p, int size);

struct gatedesc;
void lidt(struct gatedesc *p, int size);

void ltr(ushort sel);
void reloadseg(ushort cs_sel, ushort ds_sel);
uint readeflags(void);
void loadgs(ushort v);
void cli(void);
void sti(void);
uint xchg(volatile uint *addr, uint newval);
void clearlock(volatile uint *p);
// CR2 (the faulting address on a page fault) and the argument to lcr3
// (a page table's physical address) are pointer-width, not fixed 32-bit -
// on the 64-bit build a plain uint here would silently truncate a
// canonical high address.
uintp rcr2(void);
void lcr3(uintp val);
uint64 rcr0(void);
void lcr0(uint64 val);
uint64 rcr4(void);
void lcr4(uint64 val);
// FXSAVE/FXRSTOR: area must be a 16-byte-aligned 512-byte buffer (see
// kernel/proc.c's fpu_state, allocated a whole kalloc()'d page for
// exactly this reason).
void fpu_save(void *area);
void fpu_restore(void *area);
void fpu_clean_template(void *area);

// WRMSR is only ever used here for MSR_FS_BASE (see include/mmu.h),
// which takes a full 64-bit value.
void wrmsr(uint msr, uint64 val);
uint64 rdmsr(uint msr);

// Invalidate one page's TLB entry - needed after modifying a live PTE
// still cached in the CPU's TLB (see kernel/vm.c's copyuvm() and
// vm_handle_pagefault()).
void invlpg(void *va);

//PAGEBREAK: 36
// Layout of the trap frame built on the stack by the hardware and by
// kernel/trapasm.asm, and passed to trap(). Long mode has no pusha, so
// trapasm.asm pushes registers one at a time in a chosen order (see the
// comment there) - the fields below are listed in reverse of that push
// order (last pushed = lowest address = first field). ds/es/fs/gs are
// dropped entirely: once segmentation is flat, as it is here, reloading
// them on every trap has no effect and saving them is pointless.
//
// eip/esp/eflags/eax keep their 32-bit-conventional names (rather than
// rip/rsp/rflags/rax) even though they're uint64 - trap.c, proc.c,
// exec.c, and syscall.c reference these fields by name; only their
// width matters, and callers that cast into/out of them already use
// the arch-neutral uintp type for exactly this reason.
//
// There's no ds/es/fs/gs here at all - not even as inert padding.
// That was tried and is exactly wrong: sizeof(this struct) has to
// equal precisely what trapasm.asm's alltraps actually pushes (this
// struct's layout is its contract), because whole-struct copies of
// *p->tf (fork()'s `*np->tf = *curproc->tf`, in particular) read/
// write exactly sizeof(struct trapframe) bytes starting at p->tf -
// which for a real (not proc.c's userinit()-constructed fake initial
// one) trapframe sits flush against the top of the process's one-page
// kernel stack. Padding this struct out with unused trailing bytes
// made every such copy read past the end of that page.
struct trapframe {
  uint64 r15;
  uint64 r14;
  uint64 r13;
  uint64 r12;
  uint64 r11;
  uint64 r10;
  uint64 r9;
  uint64 r8;
  uint64 rbp;
  uint64 rdi;
  uint64 rsi;
  uint64 rdx;
  uint64 rcx;
  uint64 rbx;
  uint64 eax;

  uint64 trapno;

  // below here defined by x86-64 hardware
  uint64 err;
  uint64 eip;
  uint64 cs;
  uint64 eflags;

  // below here only when crossing rings, such as from user to kernel
  uint64 esp;
  uint64 ss;
};
