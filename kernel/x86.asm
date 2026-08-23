; Routines to let C code use special x86 instructions, using the SysV
; AMD64 calling convention (arguments in rdi, rsi, rdx, rcx, r8, r9,
; not the stack). A couple of instructions - lgdt/lidt, rcr2/lcr3 -
; genuinely behave differently in 64-bit mode (a 10-byte pseudo-
; descriptor instead of 6; a 64-bit CR2/CR3 instead of 32-bit), not
; just differently-sized.
;
; A "$" - prefixed - label is used for a handful of these names since
; they're themselves x86 instruction mnemonics that NASM's parser
; would otherwise swallow.

BITS 64
section .text

; uchar inb(ushort port)
global inb
inb:
  mov dx, di
  in al, dx
  movzx eax, al
  ret

; void insl(int port, void *addr, int cnt)
global insl
insl:
  mov r10, rdi    ; stash port (rdi is about to become the destination)
  mov rdi, rsi    ; addr
  mov rcx, rdx    ; cnt
  mov dx, r10w    ; port
  cld
  rep insd
  ret

; void outb(ushort port, uchar data)
global outb
outb:
  mov dx, di
  mov al, sil
  out dx, al
  ret

; void outw(ushort port, ushort data)
global outw
outw:
  mov dx, di
  mov ax, si
  out dx, ax
  ret

; void outsl(int port, const void *addr, int cnt)
global outsl
outsl:
  mov r10, rdi    ; stash port
  mov rcx, rdx    ; cnt
  mov dx, r10w    ; port
  cld
  rep outsd       ; source is [rsi], which already holds addr (arg 2)
  ret

; void stosb(void *addr, int data, int cnt)
global $stosb
$stosb:
  mov rcx, rdx    ; cnt
  mov eax, esi    ; data
  cld
  rep stosb       ; dest is [rdi], which already holds addr (arg 1)
  ret

; void stosl(void *addr, int data, int cnt)
global stosl
stosl:
  mov rcx, rdx    ; cnt
  mov eax, esi    ; data
  cld
  rep stosd       ; dest is [rdi], which already holds addr (arg 1)
  ret

; void lgdt(struct segdesc *p, int size)
; Builds the 10-byte {limit:16, base:64} pseudo-descriptor LGDT expects
; in 64-bit mode (wider than the 32-bit build's 6-byte {limit:16,
; base:32} - see x86.asm) in a small scratch area on the stack, then
; loads it.
global $lgdt
$lgdt:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  mov eax, esi      ; size
  dec eax
  mov [rbp-16], ax   ; pd.limit = size-1
  mov [rbp-14], rdi  ; pd.base = p
  lgdt [rbp-16]
  mov rsp, rbp
  pop rbp
  ret

; void lidt(struct gatedesc *p, int size)
; Same layout trick as lgdt, above, but for the IDT register.
global $lidt
$lidt:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  mov eax, esi
  dec eax
  mov [rbp-16], ax
  mov [rbp-14], rdi
  lidt [rbp-16]
  mov rsp, rbp
  pop rbp
  ret

; void ltr(ushort sel)
global $ltr
$ltr:
  mov ax, di
  ltr ax
  ret

; void reloadseg(ushort cs_sel, ushort ds_sel)
; Loads ds/es/fs/gs/ss with ds_sel directly (an ordinary mov), and
; reloads cs with cs_sel via a far return - the only way to change cs
; in long mode short of a far call/jmp or an interrupt return. Needed
; right after lgdt() replaces the GDT (seginit(), kernel/vm.c): unlike
; a real segment-limit-checking mode, the CPU doesn't re-validate cs/ss
; against whatever GDT is currently loaded just because lgdt ran - it
; keeps using the *already-cached* descriptor state from whichever
; selector was active before, which under Limine is a selector from
; Limine's own (now unloaded) GDT, not this kernel's. That stays
; invisible for perfectly ordinary code the entire time, since nothing
; about plain execution re-checks it - until the first iretq, which
; *does* re-validate cs/ss (a trap return) against the GDT actually
; loaded now, and #GPs since Limine's old selector value doesn't
; correspond to a valid descriptor in this kernel's own gdt[]. Found
; the hard way (a VirtualBox VM), the first interrupt to ever return
; on the boot cpu after seginit() ran.
global reloadseg
reloadseg:
  mov ax, si
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  push rdi
  lea rax, [rel .reloaded]
  push rax
  o64 retf
.reloaded:
  ret

; uint readeflags(void)
global readeflags
readeflags:
  pushfq
  pop rax
  ret

; void loadgs(ushort v)
global loadgs
loadgs:
  mov ax, di
  mov gs, ax
  ret

; void cli(void)
global $cli
$cli:
  cli
  ret

; void sti(void)
global $sti
$sti:
  sti
  ret

; uint xchg(volatile uint *addr, uint newval)
; Atomically swaps *addr with newval and returns the old value; used by
; the spinlock implementation.
global $xchg
$xchg:
  mov eax, esi
  lock xchg [rdi], eax
  ret

; void clearlock(volatile uint *p)
; Sets *p to 0 with a single plain store. Used by spinlock.c's release()
; instead of a C assignment: the call itself (like the inline asm it
; replaces) acts as a compiler barrier, so the store can't be reordered
; or elided relative to the __sync_synchronize() release fence just
; before it.
global clearlock
clearlock:
  mov dword [rdi], 0
  ret

; uintp rcr2(void)
; CR2 (the faulting address on a page fault) is a full 64-bit register
; in long mode - unlike the 32-bit build's rcr2, this returns the whole
; thing in rax, not just eax.
global rcr2
rcr2:
  mov rax, cr2
  ret

; void lcr3(uintp val)
global lcr3
lcr3:
  mov cr3, rdi
  ret

; uint64 rcr0(void) / void lcr0(uint64 val) - CR0.EM/CR0.MP (bits 2/1),
; cleared/set at boot (kernel/main.c's fpuinit(), one per CPU) so FPU/
; SSE instructions execute instead of faulting - see kernel/proc.c's
; fpu_save()/fpu_restore() for why user-space floating point (GUI
; roadmap ToaruOS-style rewrite's TrueType text) needed this at all.
global rcr0
rcr0:
  mov rax, cr0
  ret

global lcr0
lcr0:
  mov cr0, rdi
  ret

; uint64 rcr4(void) / void lcr4(uint64 val) - CR4.OSFXSR/OSXMMEXCPT
; (bits 9/10), set alongside CR0 above.
global rcr4
rcr4:
  mov rax, cr4
  ret

global lcr4
lcr4:
  mov cr4, rdi
  ret

; void fpu_save(void *area) / void fpu_restore(void *area) - legacy
; FXSAVE/FXRSTOR, a 512-byte area the caller must 16-byte-align (see
; kernel/proc.c's allocproc(), which kalloc()s a whole page for it).
global fpu_save
fpu_save:
  fxsave [rdi]
  ret

global fpu_restore
fpu_restore:
  fxrstor [rdi]
  ret

section .data
align 16
default_mxcsr: dd 0x1F80   ; power-up-default MXCSR (all SSE FP exceptions masked)

section .text
; void fpu_clean_template(void *area) - resets the real FPU/SSE state to
; its standard power-up-equivalent values (FNINIT alone doesn't touch
; MXCSR, hence the explicit LDMXCSR) and FXSAVEs that clean state into
; area (16-byte-aligned, 512 bytes). kernel/vm.c's fpuinit() calls this
; once into a shared template; kernel/proc.c's allocproc() memcpy()s it
; into every new process's own fpu_state - an all-zero FXSAVE image
; would leave MXCSR=0, unmasking every SSE FP exception and trapping
; (#XM, unhandled) on the first ordinary precision-loss condition.
global fpu_clean_template
fpu_clean_template:
  fninit
  ldmxcsr [rel default_mxcsr]
  fxsave [rdi]
  ret

; void wrmsr(uint msr, uint64 val)
; wrmsr itself takes the MSR index in ecx and the 64-bit value split as
; edx:eax (high:low), not as one 64-bit register - the "$" prefix is the
; same reserved-mnemonic dodge as $lgdt/$ltr/etc above.
global $wrmsr
$wrmsr:
  mov ecx, edi   ; msr
  mov rax, rsi   ; val (also sets eax = val's low 32 bits)
  mov rdx, rax
  shr rdx, 32    ; edx = val's high 32 bits
  wrmsr
  ret

; uint64 rdmsr(uint msr)
; Inverse of $wrmsr above: rdmsr reads the MSR indexed by ecx into
; edx:eax (high:low); reassembled into one 64-bit rax for the caller
; (kernel/vm.c's patinit(), which needs the PAT MSR's current value to
; modify just one 8-bit PAT-entry field rather than blindly overwriting
; entries a BIOS/firmware may already have set up).
global $rdmsr
$rdmsr:
  mov ecx, edi   ; msr
  rdmsr
  shl rdx, 32
  or rax, rdx
  ret

; void invlpg(void *va)
; Invalidates the TLB entry for one page, so a PTE just modified in
; memory (kernel/vm.c's copyuvm()/vm_handle_pagefault(), downgrading or
; upgrading a page's COW/writable state) takes effect immediately
; instead of possibly still being served by a stale cached translation.
global $invlpg
$invlpg:
  invlpg [rdi]
  ret
