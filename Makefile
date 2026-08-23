# The first rule in the file is normally make's default goal, but that's
# the build/-directory-creation rule below (needed as an order-only
# prerequisite elsewhere) rather than a real build target - pin the
# default explicitly so a bare `make` builds the OS image like `make all`.
.DEFAULT_GOAL := all

OBJS = \
	$(OBJDIR)/kernel/acpi.o\
	$(OBJDIR)/kernel/bio.o\
	$(OBJDIR)/kernel/console.o\
	$(OBJDIR)/kernel/epoll.o\
	$(OBJDIR)/kernel/exec.o\
	$(OBJDIR)/kernel/file.o\
	$(OBJDIR)/kernel/fs.o\
	$(OBJDIR)/kernel/ide.o\
	$(OBJDIR)/kernel/ioapic.o\
	$(OBJDIR)/kernel/kalloc.o\
	$(OBJDIR)/kernel/kbd.o\
	$(OBJDIR)/kernel/lapic.o\
	$(OBJDIR)/kernel/limine.o\
	$(OBJDIR)/kernel/log.o\
	$(OBJDIR)/kernel/main.o\
	$(OBJDIR)/kernel/mouse.o\
	$(OBJDIR)/kernel/mp.o\
	$(OBJDIR)/kernel/picirq.o\
	$(OBJDIR)/kernel/pipe.o\
	$(OBJDIR)/kernel/proc.o\
	$(OBJDIR)/kernel/shm.o\
	$(OBJDIR)/kernel/sleeplock.o\
	$(OBJDIR)/kernel/socket.o\
	$(OBJDIR)/kernel/spinlock.o\
	$(OBJDIR)/kernel/string.o\
	$(OBJDIR)/kernel/syscall.o\
	$(OBJDIR)/kernel/sysfile.o\
	$(OBJDIR)/kernel/sysnet.o\
	$(OBJDIR)/kernel/sysproc.o\
	$(OBJDIR)/kernel/trap.o\
	$(OBJDIR)/kernel/uart.o\
	$(OBJDIR)/kernel/vbe.o\
	$(OBJDIR)/kernel/vectors.o\
	$(OBJDIR)/kernel/vm.o\

ENTRYOBJ = $(OBJDIR)/kernel/entry.o
SWTCHOBJ = $(OBJDIR)/kernel/swtch.o
TRAPASMOBJ = $(OBJDIR)/kernel/trapasm.o
X86ASMOBJ = $(OBJDIR)/kernel/x86.o
KERNELLD = kernel/kernel.ld
INITCODEOBJ = $(OBJDIR)/user/initcode.o
OBJS += $(SWTCHOBJ) $(TRAPASMOBJ) $(X86ASMOBJ)

# Toolchain: x86_64-elf-gcc/ld (Homebrew, /usr/local/bin).
TOOLPREFIX = x86_64-elf-
CC = $(TOOLPREFIX)gcc
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

QEMU = qemu-system-x86_64

# NASM assembles all the .asm (Intel-syntax) sources. The shared C headers
# in include/ (constants, struct layouts) are still expanded into them with
# the C preprocessor before NASM ever sees the file - see the %.o: %.asm
# rule below.
NASM = nasm
NASMFLAGS = -f elf64 -g

# All headers live in include/, shared by kernel/, user/ and mkfs/.
CPPFLAGS = -Iinclude

CFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -O2 -Wall -MD -ggdb -Werror -fno-omit-frame-pointer -Wno-error=array-bounds -Wno-error=infinite-recursion -Wno-error=unused-but-set-variable -m64
# Nothing here ever sets CR0/CR4 to enable SSE (no build does FPU/SSE
# state management for kernel *or* user code) - but x86_64-elf-gcc's
# codegen baseline includes SSE2 even in -m64 mode, so GCC will happily
# autovectorize an ordinary scalar loop into SSE instructions at -O2
# with no other prompting. Executing one with SSE disabled raises
# #UD/#NM; this early in boot, with no IDT installed yet, that's an
# instant triple fault - so -mgeneral-regs-only rules out vector
# codegen entirely, for kernel and user code alike.
CFLAGS += -mgeneral-regs-only
CFLAGS += -fno-stack-protector -fno-pie -no-pie

# KCFLAGS adds flags needed only for kernel C code proper (not user
# programs): -mcmodel=kernel because KERNBASE
# (0xFFFFFFFF80000000, see memlayout.h) is in the top -2GB, which is
# exactly the address range GCC's "kernel" code model - as opposed to
# the default "small" model, which assumes symbols live in the *low*
# 2GB - is for; -mno-red-zone because an interrupt can land on the
# kernel stack at any instruction boundary and push a trap frame below
# the current %rsp, which would silently corrupt a leaf function's red
# zone.
KCFLAGS = -mcmodel=kernel -mno-red-zone

# ld's emulation name, hardcoded (confirmed via `x86_64-elf-ld -V` on
# this machine: elf_x86_64, not the FreeBSD elf_i386_fbsd variant).
LDFLAGS += -m elf_x86_64

# Every generated file lives under build/: final binaries, disk images,
# and disassembly/symbol dumps directly in build/, and every intermediate
# .o/.d/.i file under build/obj/, mirroring the source tree
# (build/obj/kernel/, build/obj/user/). Keeping the object tree at a
# distinct path from the final binaries matters, not just style:
# build/kernel (the linked kernel binary) and a hypothetical build/kernel/
# (a directory of kernel object files) can't both exist, so intermediates
# get their own build/obj/ subtree instead of colliding with the very
# product names they build towards - the same reason kernel/ and user/
# exist as directories one level up in the first place. kernel/, user/,
# include/, and mkfs/ contain only hand-written (or, for
# kernel/vectors.pl's output, generated-but-that's-the-point) source,
# never build output; `make clean` is just `rm -rf build`.
BUILD = build
OBJDIR = $(BUILD)/obj

$(BUILD) $(OBJDIR)/kernel $(OBJDIR)/user $(OBJDIR)/musl-test:
	mkdir -p $@

# Compile a C source into build/obj/<dir>/<name>.o, keeping the same
# kernel/user split the sources themselves use. kernel/ adds KCFLAGS on
# top of CFLAGS.
$(OBJDIR)/kernel/%.o: kernel/%.c | $(OBJDIR)/kernel
	$(CC) $(CFLAGS) $(KCFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJDIR)/user/%.o: user/%.c | $(OBJDIR)/user
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Assemble a NASM source the same way: first expand #include/#define from
# include/ with the C preprocessor (cpp doesn't care about NASM vs GAS
# mnemonics, it just does text substitution), then hand the result to nasm.
$(OBJDIR)/kernel/%.o: kernel/%.asm | $(OBJDIR)/kernel
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(@:.o=.i) $<
	$(NASM) $(NASMFLAGS) -o $@ $(@:.o=.i)

$(OBJDIR)/user/%.o: user/%.asm | $(OBJDIR)/user
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(@:.o=.i) $<
	$(NASM) $(NASMFLAGS) -o $@ $(@:.o=.i)

# ============================================================
# Limine boot: kernel/kernel.ld's .limine_requests section (kernel/
# limine.c) is what Limine itself reads to answer requests before ever
# jumping to kernel/entry.asm - see limine/ (the vendored bootloader
# itself) and limine.conf (the boot menu entry: kernel path + fs.img as
# a module). Two image flavors, both BIOS-bootable (no UEFI target -
# see the QEMUOPTS_BIOS comment below): a raw hard-disk image
# ($(IMAGE_NAME).hdd, dd-to-USB-bootable) and an ISO
# ($(IMAGE_NAME).iso) - both built the way limine-bootloader's own
# example templates do (one small FAT partition holding the kernel ELF,
# fs.img, and limine.conf, plus `limine bios-install` writing Limine's
# own MBR/stage-2 bootstrap into the image), and both ultimately hybrid
# (either can be dd'd raw *or* attached as a CD/DVD), unlike the old
# BIOS boot loader's split poc_bios.img/poc-os.iso pair.
# ============================================================

IMAGE_NAME = poc-os
LIMINE_DIR = limine

# The `limine` deploy-tool binary is a build product of this specific
# host (portable C source, but not portable machine code), so it's
# built here rather than committed - unlike everything else under
# limine/, which is the actual vendored bootloader release (see
# limine/.gitignore).
$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.c $(LIMINE_DIR)/Makefile
	$(MAKE) -C $(LIMINE_DIR)

# The raw disk image: a plain-MBR-partitioned (tools/mkmbr.py - not
# GPT: that's really an EFI-oriented convention, and this is a
# BIOS-only target, see limine.conf's own comment), FAT-formatted image
# `limine bios-install` can boot via legacy BIOS - Limine's own stage 2
# lives in the MBR gap (the unused sectors between the MBR itself and
# the partition, same spot GRUB's does), same as real hardware/USB-stick
# booting has worked this whole project via since boot/bootasm_bios.asm
# (removed). mtools (mformat/mmd/mcopy) is a Homebrew package on this
# machine, not part of the base toolchain the rest of this Makefile needs.
$(BUILD)/$(IMAGE_NAME).hdd: $(LIMINE_DIR)/limine $(BUILD)/kernel $(BUILD)/fs.img limine.conf tools/mkmbr.py | $(BUILD)
	rm -f $(BUILD)/$(IMAGE_NAME).hdd
	dd if=/dev/zero bs=1M count=64 of=$(BUILD)/$(IMAGE_NAME).hdd
	python3 tools/mkmbr.py $(BUILD)/$(IMAGE_NAME).hdd 2048 129024
	./$(LIMINE_DIR)/limine bios-install $(BUILD)/$(IMAGE_NAME).hdd
	mformat -i $(BUILD)/$(IMAGE_NAME).hdd@@1M
	mmd -i $(BUILD)/$(IMAGE_NAME).hdd@@1M ::/boot ::/boot/limine
	mcopy -i $(BUILD)/$(IMAGE_NAME).hdd@@1M $(BUILD)/kernel ::/boot/kernel
	mcopy -i $(BUILD)/$(IMAGE_NAME).hdd@@1M $(BUILD)/fs.img ::/boot/fs.img
	mcopy -i $(BUILD)/$(IMAGE_NAME).hdd@@1M limine.conf ::/boot/limine/limine.conf
	mcopy -i $(BUILD)/$(IMAGE_NAME).hdd@@1M $(LIMINE_DIR)/limine-bios.sys ::/boot/limine/limine-bios.sys

# The ISO: El-Torito-bootable (limine-bios-cd.bin), then also `limine
# bios-install`'d so the same .iso is raw-dd-to-USB-bootable too (unlike
# the old BIOS boot loader's own .iso, which explicitly wasn't - see the
# BOOT2_BIOS/poc_bios.img git history for that old comment).
$(BUILD)/$(IMAGE_NAME).iso: $(LIMINE_DIR)/limine $(BUILD)/kernel $(BUILD)/fs.img limine.conf | $(BUILD)
	rm -rf $(BUILD)/isoroot
	mkdir -p $(BUILD)/isoroot/boot/limine
	cp $(BUILD)/kernel $(BUILD)/isoroot/boot/kernel
	cp $(BUILD)/fs.img $(BUILD)/isoroot/boot/fs.img
	cp limine.conf $(BUILD)/isoroot/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(BUILD)/isoroot/boot/limine/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		$(BUILD)/isoroot -o $(BUILD)/$(IMAGE_NAME).iso
	./$(LIMINE_DIR)/limine bios-install $(BUILD)/$(IMAGE_NAME).iso


# entryother and initcode are raw binary blobs the kernel embeds with
# -b binary (see kernel/main.c's and kernel/proc.c's matching
# _binary_build_..._start symbols) rather than programs run standalone,
# so unlike everything else in $(OBJS)/ULIB they need their own two-step
# ld+objcopy recipe instead of just landing in a link line. They're
# final build products, not intermediates, so - like kernel and mkfs -
# they live directly in build/, not build/obj/. entryother is linked on
# its own below (it mixes 16/32/64-bit code in one file - see its own
# comment - so it needs the regular 64-bit LDFLAGS, just at its own
# fixed -Ttext 0x7000 rather than KERNLINK); initcode is $(INITCODEOBJ),
# which links with LDFLAGS like everything else in $(OBJS).
$(BUILD)/entryother: $(OBJDIR)/kernel/entryother.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7000 -o $(OBJDIR)/kernel/bootblockother.o $(OBJDIR)/kernel/entryother.o
	$(OBJCOPY) -S -O binary -j .text $(OBJDIR)/kernel/bootblockother.o $(BUILD)/entryother
	$(OBJDUMP) -S $(OBJDIR)/kernel/bootblockother.o > $(BUILD)/entryother.dis

$(BUILD)/initcode: $(INITCODEOBJ) | $(BUILD)
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $(OBJDIR)/user/initcode.out $(INITCODEOBJ)
	$(OBJCOPY) -S -O binary $(OBJDIR)/user/initcode.out $(BUILD)/initcode
	$(OBJDUMP) -S $(INITCODEOBJ) > $(BUILD)/initcode.dis

$(BUILD)/kernel: $(OBJS) $(ENTRYOBJ) $(BUILD)/entryother $(BUILD)/initcode $(KERNELLD) | $(BUILD)
	$(LD) $(LDFLAGS) -T $(KERNELLD) -o $(BUILD)/kernel $(ENTRYOBJ) $(OBJS) -b binary $(BUILD)/initcode $(BUILD)/entryother
	$(OBJDUMP) -S $(BUILD)/kernel > $(BUILD)/kernel.dis
	$(OBJDUMP) -t $(BUILD)/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(BUILD)/kernel.sym

tags:
	etags kernel/*.asm kernel/*.c user/*.c user/*.asm include/*.h mkfs/*.c

# Generated (not hand-written), so it lives under build/ like every other
# build product, even though NASM will treat it as a source file. Needs
# its own rule rather than the generic $(OBJDIR)/kernel/%.o: kernel/%.asm
# pattern above, since vectors.asm's "source" is itself a build product,
# not a file that exists under kernel/.
$(OBJDIR)/kernel/vectors.asm: kernel/vectors.pl | $(OBJDIR)/kernel
	./kernel/vectors.pl > $(OBJDIR)/kernel/vectors.asm

$(OBJDIR)/kernel/vectors.o: $(OBJDIR)/kernel/vectors.asm | $(OBJDIR)/kernel
	$(CC) $(CPPFLAGS) -E -x assembler-with-cpp -o $(OBJDIR)/kernel/vectors.i $(OBJDIR)/kernel/vectors.asm
	$(NASM) $(NASMFLAGS) -o $@ $(OBJDIR)/kernel/vectors.i

ULIB = $(OBJDIR)/user/ulib.o $(OBJDIR)/user/usys.o $(OBJDIR)/user/printf.o $(OBJDIR)/user/umalloc.o $(X86ASMOBJ)

# Debug info (-ggdb, in CFLAGS) is generated for every user binary, same
# as the kernel, but it's dead weight once mkfs packages the binary into
# the filesystem image: nothing in poc reads DWARF at runtime, and the
# generic $(OBJDUMP) -S/-t steps just above already captured everything
# it's useful for (source-interleaved disassembly, a symbol table) into
# build/$*.dis/.sym, which stay on the host, not in the image. Stripping
# it here (after those, so they still see it) routinely more than
# halves the linked binary's size - the difference between fitting
# under MAXFILE (include/fs.h, ~70KB - see the UPROGS comment) and not,
# for a binary like usertests with enough test code to approach it.
$(BUILD)/_%: $(OBJDIR)/user/%.o $(ULIB) | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/$*.dis
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(BUILD)/$*.sym
	$(OBJCOPY) --strip-debug $@

# musl-test/: home to runmusl.c (a manual launcher for a musl-crt1-style
# binary via raw SYS_execve - "runmusl <path> [args...]" - a real
# shell, bash, execve()s dynamic binaries directly and needs no such
# helper for the ordinary case, but this is still useful standalone,
# e.g. to pass a path bash itself can't reach yet during bring-up)
# and ldso_stubs.c (linked into libc.so itself - see MUSL_LDSO_OBJS).
# Unlike ordinary user/ programs these need neither ULIB nor an -e main convention shared
# with xv6's own ulib.c (they don't link ulib.c at all). -Iinclude is
# here too (not just -Imusl/arch/x86_64) since these reference poc-os
# kernel constants directly (e.g. ARCH_SET_FS from include/syscall.h)
# rather than only musl's own headers.
$(OBJDIR)/musl-test/%.o: musl/test/%.c | $(OBJDIR)/musl-test
	$(CC) $(CFLAGS) -Imusl/arch/x86_64 -Iinclude -c -o $@ $<

# musl/: real musl source (musl-test/ above is only raw syscall_arch.h
# smoke tests; this is actual crt1/__libc_start_main/malloc/etc object
# code). musl's own build system generates two headers from
# arch/x86_64/bits/*.in - obj/include/bits/alltypes.h and bits/
# syscall.h - normally by running its ./configure && make; the two
# recipes below just replicate those two exact commands (see musl/
# Makefile's own obj/include/bits/%.h rules) without depending on
# musl's build system or a prior ./configure, since all we want out of
# it is these two files.
$(OBJDIR)/musl/include/bits/alltypes.h: musl/arch/x86_64/bits/alltypes.h.in musl/include/alltypes.h.in musl/tools/mkalltypes.sed
	@mkdir -p $(dir $@)
	sed -f musl/tools/mkalltypes.sed musl/arch/x86_64/bits/alltypes.h.in musl/include/alltypes.h.in > $@

$(OBJDIR)/musl/include/bits/syscall.h: musl/arch/x86_64/bits/syscall.h.in
	@mkdir -p $(dir $@)
	cp $< $@
	sed -n -e s/__NR_/SYS_/p < $< >> $@

MUSL_GENH = $(OBJDIR)/musl/include/bits/alltypes.h $(OBJDIR)/musl/include/bits/syscall.h

# Mirrors musl's own CFLAGS_ALL/IFLAGS (see musl/Makefile) so its
# source compiles unmodified: -nostdinc/-ffreestanding because it's
# its own libc, not something built against poc-os's (or the host's)
# headers; the -I order matters (arch-specific bits before generic,
# musl's own src/internal/syscall.h etc before its public include/,
# $(OBJDIR)/musl/include last among musl's own so a real header always
# wins over the generated stand-in) since a couple of names exist at
# more than one of these paths on purpose (bits/syscall.h.in and its
# generated bits/syscall.h in particular).
MUSL_CFLAGS = -std=c99 -ffreestanding -nostdinc -D_XOPEN_SOURCE=700 -Os \
	-m64 -mgeneral-regs-only -fno-stack-protector -fno-pie -no-pie \
	-fno-omit-frame-pointer -g -Wall
MUSL_INC = -Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/internal \
	-Imusl/src/include -Imusl/src/internal -I$(OBJDIR)/musl/include -Imusl/include

# One rule for musl/**.c, however deep - unlike kernel/user's fixed
# one-level split, musl's source tree has real subdirectories
# (src/env/, src/thread/x86_64/, crt/, ...), so this mirrors each
# source's path under $(OBJDIR)/musl/ via $(dir $@) rather than
# pre-declaring every subdirectory as an order-only prerequisite the
# way the rest of this Makefile does.
$(OBJDIR)/musl/%.o: musl/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) $(MUSL_INC) -c -o $@ $<

# Ditto for musl's hand-written .s (AT&T syntax, assembled directly by
# $(CC) via binutils as - not one of poc-os's own NASM .asm sources).
$(OBJDIR)/musl/%.o: musl/%.s
	@mkdir -p $(dir $@)
	$(CC) -m64 -c -o $@ $<

# runmusl: generic launcher, "runmusl <path> [args...]" - see
# musl/test/runmusl.c. Entered the normal poc-os way (-e main, native
# SYS_exec) since it's poc-os code, not musl code; what it launches
# (SYS_execve on argv[1]) is what needs the musl-style stack.
$(BUILD)/_runmusl: $(OBJDIR)/musl-test/runmusl.o | $(BUILD)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(BUILD)/runmusl.dis
	$(OBJCOPY) --strip-debug $@

# musl's own version.h (musl/src/internal/version.c's #include
# "version.h"): normally generated by ./configure from musl/VERSION;
# this is the same one-line `#define VERSION "..."` musl's own
# Makefile produces, without needing to run ./configure at all - same
# rationale as MUSL_GENH's alltypes.h/syscall.h above.
$(OBJDIR)/musl/internal/version.h: musl/VERSION
	@mkdir -p $(dir $@)
	printf '#define VERSION "%s"\n' "$$(cat musl/VERSION)" > $@

# musl/ldso/ (real ld-musl-x86_64.so.1 - dlstart.c/dynlink.c, unmodified
# upstream - plus enough of real musl's own libc, also unmodified
# upstream, to link a genuine libc.so around them) and true/false/cat
# themselves (real dynamically-linked PIE ELF binaries importing libc
# via a real PT_INTERP/PLT/GOT, not musl-crt1-launched-by-runmusl like
# poc-os's own static binaries) live in their own $(OBJDIR)/musl-pic/
# tree, compiled -fPIC - shared-object code, unlike every other musl
# object in this Makefile (MUSL_CFLAGS above is -fno-pie/-no-pie, for
# poc-os's own always-static-at-0 binaries). A separate object tree
# rather than reusing $(OBJDIR)/musl/ so a source file needed by both
# a static MUSL_LIBC_OBJS-style binary (e.g. musl/src/string/memcpy.c)
# and libc.so doesn't fight over one non-PIC-vs-PIC .o.
MUSL_PIC_CFLAGS = $(MUSL_CFLAGS:-fno-pie=-fPIC)
MUSL_PIC_INC = -Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/internal \
	-Imusl/src/include -Imusl/src/internal -I$(OBJDIR)/musl/include -Imusl/include

$(OBJDIR)/musl-pic/%.o: musl/%.c $(MUSL_GENH) $(OBJDIR)/musl/internal/version.h
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_PIC_CFLAGS) $(MUSL_PIC_INC) -c -o $@ $<

$(OBJDIR)/musl-pic/%.o: musl/%.s
	@mkdir -p $(dir $@)
	$(CC) -m64 -fPIC -c -o $@ $<

# coreutils_shims.c (see its own comment) is poc-os C, not gnulib or
# musl - but it fills in real libc-shaped gaps (a translating fstat(),
# mbszero(), abort(), ...), so it belongs in libc.so itself, available
# to every dynamically-linked binary, not statically re-linked into
# each one individually. Compiled exactly like any other musl-pic
# source (MUSL_PIC_CFLAGS/INC - see musl/src/internal/stdio_impl.h's
# role there, spelled out in the file's own comment), just from a
# source path outside musl/ - hence its own rule rather than the
# generic $(OBJDIR)/musl-pic/%.o: musl/%.c pattern above.
$(OBJDIR)/musl-pic/coreutils-shims/coreutils_shims.o: coreutils/poc/coreutils_shims.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_PIC_CFLAGS) $(MUSL_PIC_INC) -c -o $@ $<

# bash_shims.c (see its own comment): the same idea as coreutils_shims.c
# just above (real libc-shaped gaps - dup2(), uname(), getrlimit()/
# setrlimit(), times() - poc-os has no syscall for at all), also part of
# libc.so rather than linked per-executable, so any future dynamically-
# linked program needing dup2()/uname()/etc gets it for free too.
$(OBJDIR)/musl-pic/bash-shims/bash_shims.o: bash/poc/bash_shims.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_PIC_CFLAGS) $(MUSL_PIC_INC) -c -o $@ $<

# The real musl objects libc.so needs beyond MUSL_LIBC_OBJS/
# MUSL_MALLOC_OBJS/MUSL_STDIO_OBJS's already-proven subset - grown the
# same file-by-file way those were (see this Makefile's own git log),
# just starting from "does musl/ldso/dynlink.c link" instead of "does
# a musl-crt1 hello world link". crt1.o is deliberately excluded (see
# musl/crt/Scrt1.c below): it defines _start for a *static* program,
# not something libc.so itself needs.
MUSL_LDSO_OBJS = \
	$(OBJDIR)/musl-pic/src/env/__libc_start_main.o \
	$(OBJDIR)/musl-pic/src/env/__init_tls.o \
	$(OBJDIR)/musl-pic/src/env/__environ.o \
	$(OBJDIR)/musl-pic/src/internal/libc.o \
	$(OBJDIR)/musl-pic/src/internal/defsysinfo.o \
	$(OBJDIR)/musl-pic/src/internal/syscall_ret.o \
	$(OBJDIR)/musl-pic/src/thread/__syscall_cp.o \
	$(OBJDIR)/musl-pic/src/thread/default_attr.o \
	$(OBJDIR)/musl-pic/src/thread/x86_64/__set_thread_area.o \
	$(OBJDIR)/musl-pic/src/string/memcpy.o \
	$(OBJDIR)/musl-pic/src/errno/__errno_location.o \
	$(OBJDIR)/musl-pic/src/unistd/write.o \
	$(OBJDIR)/musl-pic/src/exit/exit.o \
	$(OBJDIR)/musl-pic/src/exit/_Exit.o \
	$(OBJDIR)/musl-pic/src/malloc/oldmalloc/malloc.o \
	$(OBJDIR)/musl-pic/src/malloc/lite_malloc.o \
	$(OBJDIR)/musl-pic/src/malloc/calloc.o \
	$(OBJDIR)/musl-pic/src/malloc/realloc.o \
	$(OBJDIR)/musl-pic/src/malloc/free.o \
	$(OBJDIR)/musl-pic/src/malloc/replaced.o \
	$(OBJDIR)/musl-pic/src/mman/mmap.o \
	$(OBJDIR)/musl-pic/src/mman/munmap.o \
	$(OBJDIR)/musl-pic/src/mman/madvise.o \
	$(OBJDIR)/musl-pic/src/mman/mremap.o \
	$(OBJDIR)/musl-pic/src/mman/mprotect.o \
	$(OBJDIR)/musl-pic/src/string/memset.o \
	$(OBJDIR)/musl-pic/src/string/strlen.o \
	$(OBJDIR)/musl-pic/src/thread/__wait.o \
	$(OBJDIR)/musl-pic/src/thread/__lock.o \
	$(OBJDIR)/musl-pic/src/stdio/vfprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/printf.o \
	$(OBJDIR)/musl-pic/src/stdio/vprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/__towrite.o \
	$(OBJDIR)/musl-pic/src/stdio/__overflow.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_write.o \
	$(OBJDIR)/musl-pic/src/stdio/stdout.o \
	$(OBJDIR)/musl-pic/src/stdio/__lockfile.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_exit.o \
	$(OBJDIR)/musl-pic/src/stdio/ofl.o \
	$(OBJDIR)/musl-pic/src/stdio/ofl_add.o \
	$(OBJDIR)/musl-pic/src/stdio/fwrite.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_close.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdout_write.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_seek.o \
	$(OBJDIR)/musl-pic/src/stdio/snprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/vsnprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/dprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/vdprintf.o \
	$(OBJDIR)/musl-pic/src/errno/strerror.o \
	$(OBJDIR)/musl-pic/src/string/strnlen.o \
	$(OBJDIR)/musl-pic/src/string/memchr.o \
	$(OBJDIR)/musl-pic/src/string/strcpy.o \
	$(OBJDIR)/musl-pic/src/string/strrchr.o \
	$(OBJDIR)/musl-pic/src/string/strchr.o \
	$(OBJDIR)/musl-pic/src/string/strchrnul.o \
	$(OBJDIR)/musl-pic/src/string/stpcpy.o \
	$(OBJDIR)/musl-pic/src/string/memrchr.o \
	$(OBJDIR)/musl-pic/src/string/strcspn.o \
	$(OBJDIR)/musl-pic/src/string/strspn.o \
	$(OBJDIR)/musl-pic/src/string/strncmp.o \
	$(OBJDIR)/musl-pic/src/string/memcmp.o \
	$(OBJDIR)/musl-pic/src/multibyte/wctomb.o \
	$(OBJDIR)/musl-pic/src/multibyte/wcrtomb.o \
	$(OBJDIR)/musl-pic/src/locale/__lctrans.o \
	$(OBJDIR)/musl-pic/src/unistd/lseek.o \
	$(OBJDIR)/musl-pic/src/unistd/pread.o \
	$(OBJDIR)/musl-pic/src/unistd/_exit.o \
	$(OBJDIR)/musl-pic/src/fcntl/open.o \
	$(OBJDIR)/musl-pic/src/unistd/close.o \
	$(OBJDIR)/musl-pic/src/unistd/read.o \
	$(OBJDIR)/musl-pic/src/env/getenv.o \
	$(OBJDIR)/musl-pic/src/malloc/libc_calloc.o \
	$(OBJDIR)/musl-pic/src/ldso/dlerror.o \
	$(OBJDIR)/musl-pic/src/ldso/tlsdesc.o \
	$(OBJDIR)/musl-pic/src/internal/version.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_mutex_lock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_mutex_unlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_cond_wait.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_cond_broadcast.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_rdlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_wrlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_unlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_tryrdlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_trywrlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_timedrdlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_rwlock_timedwrlock.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_setcancelstate.o \
	$(OBJDIR)/musl-pic/src/thread/__tls_get_addr.o \
	$(OBJDIR)/musl-pic/src/thread/vmlock.o \
	$(OBJDIR)/musl-pic/src/setjmp/x86_64/setjmp.o \
	$(OBJDIR)/musl-pic/src/setjmp/x86_64/longjmp.o \
	$(OBJDIR)/musl-pic/ldso/dlstart.o \
	$(OBJDIR)/musl-pic/ldso/dynlink.o \
	$(OBJDIR)/musl-pic/test/ldso_stubs.o \
	$(OBJDIR)/musl-pic/src/ctype/__ctype_get_mb_cur_max.o \
	$(OBJDIR)/musl-pic/src/time/__map_file.o \
	$(OBJDIR)/musl-pic/src/locale/__mo_lookup.o \
	$(OBJDIR)/musl-pic/src/exit/atexit.o \
	$(OBJDIR)/musl-pic/src/locale/c_locale.o \
	$(OBJDIR)/musl-pic/src/stdio/fclose.o \
	$(OBJDIR)/musl-pic/src/fcntl/fcntl.o \
	$(OBJDIR)/musl-pic/src/stdio/ferror.o \
	$(OBJDIR)/musl-pic/src/stdio/fflush.o \
	$(OBJDIR)/musl-pic/src/stdio/fprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/fputs.o \
	$(OBJDIR)/musl-pic/src/stdio/putc.o \
	$(OBJDIR)/musl-pic/src/stdio/stderr.o \
	$(OBJDIR)/musl-pic/src/stdio/clearerr.o \
	$(OBJDIR)/musl-pic/src/stdio/fileno.o \
	$(OBJDIR)/musl-pic/src/multibyte/internal.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbrtoc32.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbrtowc.o \
	$(OBJDIR)/musl-pic/src/locale/langinfo.o \
	$(OBJDIR)/musl-pic/src/locale/locale_map.o \
	$(OBJDIR)/musl-pic/src/locale/setlocale.o \
	$(OBJDIR)/musl-pic/src/malloc/reallocarray.o \
	$(OBJDIR)/musl-pic/src/malloc/posix_memalign.o \
	$(OBJDIR)/musl-pic/src/malloc/oldmalloc/aligned_alloc.o \
	$(OBJDIR)/musl-pic/src/string/strcmp.o \
	$(OBJDIR)/musl-pic/src/string/strerror_r.o \
	$(OBJDIR)/musl-pic/src/string/memmove.o \
	$(OBJDIR)/musl-pic/src/misc/ioctl.o \
	$(OBJDIR)/musl-pic/src/legacy/getpagesize.o \
	$(OBJDIR)/musl-pic/src/unistd/unlink.o \
	$(OBJDIR)/musl-pic/src/unistd/link.o \
	$(OBJDIR)/musl-pic/src/stat/mkdir.o \
	$(OBJDIR)/musl-pic/src/stat/mknod.o \
	$(OBJDIR)/musl-pic/src/unistd/getpid.o \
	$(OBJDIR)/musl-pic/src/signal/kill.o \
	$(OBJDIR)/musl-pic/src/exit/assert.o \
	$(OBJDIR)/musl-pic/src/misc/getopt.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbtowc.o \
	$(OBJDIR)/musl-pic/src/stdio/putchar.o \
	$(OBJDIR)/musl-pic/src/stdio/ext2.o \
	$(OBJDIR)/musl-pic/src/stdio/fseek.o \
	$(OBJDIR)/musl-pic/src/stdio/stdin.o \
	$(OBJDIR)/musl-pic/src/stdio/fgetc.o \
	$(OBJDIR)/musl-pic/src/stdio/fread.o \
	$(OBJDIR)/musl-pic/src/stdio/__stdio_read.o \
	$(OBJDIR)/musl-pic/src/stdio/__toread.o \
	$(OBJDIR)/musl-pic/src/stdio/__uflow.o \
	$(OBJDIR)/musl-pic/coreutils-shims/coreutils_shims.o \
	$(OBJDIR)/musl-pic/src/conf/fpathconf.o \
	$(OBJDIR)/musl-pic/src/conf/pathconf.o \
	$(OBJDIR)/musl-pic/src/dirent/closedir.o \
	$(OBJDIR)/musl-pic/src/dirent/dirfd.o \
	$(OBJDIR)/musl-pic/src/dirent/fdopendir.o \
	$(OBJDIR)/musl-pic/src/dirent/opendir.o \
	$(OBJDIR)/musl-pic/src/dirent/readdir.o \
	$(OBJDIR)/musl-pic/src/dirent/rewinddir.o \
	$(OBJDIR)/musl-pic/src/exit/abort_lock.o \
	$(OBJDIR)/musl-pic/src/internal/procfdname.o \
	$(OBJDIR)/musl-pic/src/process/_Fork.o \
	$(OBJDIR)/musl-pic/src/process/fork.o \
	$(OBJDIR)/musl-pic/src/stat/mkfifoat.o \
	$(OBJDIR)/musl-pic/src/stdio/fputc.o \
	$(OBJDIR)/musl-pic/src/stdio/getchar.o \
	$(OBJDIR)/musl-pic/src/stdio/puts.o \
	$(OBJDIR)/musl-pic/src/stdlib/qsort.o \
	$(OBJDIR)/musl-pic/src/stdlib/qsort_nr.o \
	$(OBJDIR)/musl-pic/src/string/mempcpy.o \
	$(OBJDIR)/musl-pic/src/string/strdup.o \
	$(OBJDIR)/musl-pic/src/string/strstr.o \
	$(OBJDIR)/musl-pic/src/unistd/chdir.o \
	$(OBJDIR)/musl-pic/src/unistd/fchdir.o \
	$(OBJDIR)/musl-pic/src/unistd/isatty.o \
	$(OBJDIR)/musl-pic/src/unistd/ftruncate.o \
	$(OBJDIR)/musl-pic/src/stdio/rename.o \
	$(OBJDIR)/musl-pic/src/signal/sigemptyset.o \
	$(OBJDIR)/musl-pic/src/ctype/isblank.o \
	$(OBJDIR)/musl-pic/src/ctype/iscntrl.o \
	$(OBJDIR)/musl-pic/src/ctype/iswalnum.o \
	$(OBJDIR)/musl-pic/src/ctype/iswalpha.o \
	$(OBJDIR)/musl-pic/src/ctype/iswblank.o \
	$(OBJDIR)/musl-pic/src/ctype/iswcntrl.o \
	$(OBJDIR)/musl-pic/src/ctype/iswctype.o \
	$(OBJDIR)/musl-pic/src/ctype/iswgraph.o \
	$(OBJDIR)/musl-pic/src/ctype/iswlower.o \
	$(OBJDIR)/musl-pic/src/ctype/iswprint.o \
	$(OBJDIR)/musl-pic/src/ctype/iswpunct.o \
	$(OBJDIR)/musl-pic/src/ctype/iswspace.o \
	$(OBJDIR)/musl-pic/src/ctype/iswupper.o \
	$(OBJDIR)/musl-pic/src/ctype/iswxdigit.o \
	$(OBJDIR)/musl-pic/src/ctype/tolower.o \
	$(OBJDIR)/musl-pic/src/ctype/toupper.o \
	$(OBJDIR)/musl-pic/src/ctype/towctrans.o \
	$(OBJDIR)/musl-pic/src/env/putenv.o \
	$(OBJDIR)/musl-pic/src/env/setenv.o \
	$(OBJDIR)/musl-pic/src/env/unsetenv.o \
	$(OBJDIR)/musl-pic/src/internal/intscan.o \
	$(OBJDIR)/musl-pic/src/internal/shgetc.o \
	$(OBJDIR)/musl-pic/src/locale/strcoll.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbsinit.o \
	$(OBJDIR)/musl-pic/src/regex/fnmatch.o \
	$(OBJDIR)/musl-pic/src/signal/sigaddset.o \
	$(OBJDIR)/musl-pic/src/signal/sigismember.o \
	$(OBJDIR)/musl-pic/src/stdio/sprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/vsprintf.o \
	$(OBJDIR)/musl-pic/src/stdlib/strtol.o \
	$(OBJDIR)/musl-pic/src/string/wcschr.o \
	$(OBJDIR)/musl-pic/src/string/wcslen.o \
	$(OBJDIR)/musl-pic/src/time/__month_to_secs.o \
	$(OBJDIR)/musl-pic/src/time/__secs_to_tm.o \
	$(OBJDIR)/musl-pic/src/time/__tm_to_secs.o \
	$(OBJDIR)/musl-pic/src/time/__tz.o \
	$(OBJDIR)/musl-pic/src/time/__year_to_secs.o \
	$(OBJDIR)/musl-pic/src/time/gmtime_r.o \
	$(OBJDIR)/musl-pic/src/time/localtime_r.o \
	$(OBJDIR)/musl-pic/src/time/mktime.o \
	$(OBJDIR)/musl-pic/src/time/strftime.o \
	$(OBJDIR)/musl-pic/src/time/timegm.o \
	$(OBJDIR)/musl-pic/src/unistd/tcgetpgrp.o \
	$(OBJDIR)/musl-pic/src/process/execve.o \
	$(OBJDIR)/musl-pic/src/unistd/pipe.o \
	$(OBJDIR)/musl-pic/src/unistd/dup.o \
	$(OBJDIR)/musl-pic/src/termios/tcgetattr.o \
	$(OBJDIR)/musl-pic/src/termios/tcsetattr.o \
	$(OBJDIR)/musl-pic/src/stdlib/atoi.o \
	$(OBJDIR)/musl-pic/src/stdlib/imaxdiv.o \
	$(OBJDIR)/musl-pic/src/stdlib/abs.o \
	$(OBJDIR)/musl-pic/src/ctype/isxdigit.o \
	$(OBJDIR)/musl-pic/src/ctype/ispunct.o \
	$(OBJDIR)/musl-pic/src/ctype/isalnum.o \
	$(OBJDIR)/musl-pic/src/ctype/wcswidth.o \
	$(OBJDIR)/musl-pic/src/ctype/wcwidth.o \
	$(OBJDIR)/musl-pic/src/locale/localeconv.o \
	$(OBJDIR)/musl-pic/src/locale/wcscoll.o \
	$(OBJDIR)/musl-pic/src/regex/regexec.o \
	$(OBJDIR)/musl-pic/src/regex/regcomp.o \
	$(OBJDIR)/musl-pic/src/regex/tre-mem.o \
	$(OBJDIR)/musl-pic/src/stdio/vasprintf.o \
	$(OBJDIR)/musl-pic/src/string/stpncpy.o \
	$(OBJDIR)/musl-pic/src/time/gettimeofday.o \
	$(OBJDIR)/musl-pic/src/time/localtime.o \
	$(OBJDIR)/musl-pic/src/stdio/setvbuf.o \
	$(OBJDIR)/musl-pic/src/stdio/asprintf.o \
	$(OBJDIR)/musl-pic/src/stdio/__fdopen.o \
	$(OBJDIR)/musl-pic/src/string/wcscmp.o \
	$(OBJDIR)/musl-pic/src/string/wcscpy.o \
	$(OBJDIR)/musl-pic/src/string/strcasecmp.o \
	$(OBJDIR)/musl-pic/src/string/strncasecmp.o \
	$(OBJDIR)/musl-pic/src/string/strcat.o \
	$(OBJDIR)/musl-pic/src/string/strpbrk.o \
	$(OBJDIR)/musl-pic/src/string/wmemchr.o \
	$(OBJDIR)/musl-pic/src/string/wcsncmp.o \
	$(OBJDIR)/musl-pic/src/string/strncpy.o \
	$(OBJDIR)/musl-pic/src/string/bcopy.o \
	$(OBJDIR)/musl-pic/src/string/strsignal.o \
	$(OBJDIR)/musl-pic/src/multibyte/wcsrtombs.o \
	$(OBJDIR)/musl-pic/src/multibyte/wctob.o \
	$(OBJDIR)/musl-pic/src/multibyte/mblen.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbsrtowcs.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbstowcs.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbrlen.o \
	$(OBJDIR)/musl-pic/src/multibyte/mbsnrtowcs.o \
	$(OBJDIR)/musl-pic/src/conf/confstr.o \
	$(OBJDIR)/musl-pic/src/signal/siginterrupt.o \
	$(OBJDIR)/musl-pic/src/signal/sigdelset.o \
	$(OBJDIR)/musl-pic/src/temp/mktemp.o \
	$(OBJDIR)/musl-pic/src/temp/mkstemp.o \
	$(OBJDIR)/musl-pic/src/temp/mkostemps.o \
	$(OBJDIR)/musl-pic/src/temp/mkdtemp.o \
	$(OBJDIR)/musl-pic/src/signal/x86_64/sigsetjmp.o \
	$(OBJDIR)/musl-pic/src/misc/syscall.o \
	$(OBJDIR)/musl-pic/bash-shims/bash_shims.o \
	$(OBJDIR)/musl-pic/src/signal/sigfillset.o \
	$(OBJDIR)/musl-pic/src/misc/getopt_long.o \
	$(OBJDIR)/musl-pic/src/string/strcasestr.o \
	$(OBJDIR)/musl-pic/src/misc/dirname.o \
	$(OBJDIR)/musl-pic/src/misc/realpath.o \
	$(OBJDIR)/musl-pic/src/stdio/getc_unlocked.o \
	$(OBJDIR)/musl-pic/src/temp/mkstemps.o \
	$(OBJDIR)/musl-pic/src/multibyte/btowc.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_mutex_init.o \
	$(OBJDIR)/musl-pic/src/thread/pthread_mutex_destroy.o \
	$(OBJDIR)/musl-pic/src/network/socket.o \
	$(OBJDIR)/musl-pic/src/network/bind.o \
	$(OBJDIR)/musl-pic/src/network/listen.o \
	$(OBJDIR)/musl-pic/src/network/accept.o \
	$(OBJDIR)/musl-pic/src/network/connect.o \
	$(OBJDIR)/musl-pic/src/network/sendmsg.o \
	$(OBJDIR)/musl-pic/src/network/recvmsg.o \
	$(OBJDIR)/musl-pic/src/linux/epoll.o \

# The interpreter/libc.so itself: -shared -e _dlstart is exactly
# musl's own real link line for it (see musl/Makefile's $(LDSO)
# rule) - a real ET_DYN with a real .dynsym/.dynamic/PLT/GOT, not
# something poc-os invented a shape for. build/libc.so (this exact
# name, not build/_libc.so) is also what -lc below resolves against.
# -z defs (unlike a plain "ld -shared", which tolerates an unresolved
# symbol in a shared object and only fails later, at runtime, if
# something actually calls it) makes ld fail this build step itself
# the moment MUSL_LDSO_OBJS's own object set has any real internal gap
# (confirmed necessary the hard way: an earlier musl-pic/src/stat/
# mkfifoat.o addition silently left a real, unresolved "mknodat"
# reference sitting in a built-and-installed libc.so, invisible until
# something happened to exercise that exact code path).
# Debug info is stripped for the same MAXFILE-budget reason $(BUILD)/_%
# strips it (see that rule's comment) - unstripped this is >250KB,
# stripped it's ~55KB. Installed into the image at /usr/lib/libc.so -
# see MKFS_INSTALL and $(BUILD)/fs.img below - not through UPROGS's
# usual root-placed/underscore-stripped convention.
$(BUILD)/libc.so: $(MUSL_LDSO_OBJS) | $(BUILD)
	$(LD) -m elf_x86_64 -shared -e _dlstart -z defs -o $@ $^
	$(OBJCOPY) --strip-debug $@
	$(OBJCOPY) --strip-unneeded $@

# libvterm.so: vendored libvterm (MIT, gui/libvterm/ - see its own
# LICENSE), the real xterm-class VT100/ANSI parser + screen model that
# replaced gui/terminal.c's original ~200-line hand-rolled CSI/SGR
# parser. Built as its own DT_NEEDED shared object (not statically
# linked into _terminal) for the same reason libgui.so is one: normal
# ld.so DT_NEEDED resolution via -lvterm -L $(BUILD), found at runtime
# at /usr/lib/libvterm.so through ld.so's default search path exactly
# like libgui.so/libc.so already are (see MKFS_INSTALL below).
$(BUILD)/libvterm.so: $(OBJDIR)/gui-pic/libvterm/vterm.o $(OBJDIR)/gui-pic/libvterm/state.o \
                       $(OBJDIR)/gui-pic/libvterm/screen.o $(OBJDIR)/gui-pic/libvterm/parser.o \
                       $(OBJDIR)/gui-pic/libvterm/pen.o $(OBJDIR)/gui-pic/libvterm/mouse.o \
                       $(OBJDIR)/gui-pic/libvterm/keyboard.o $(OBJDIR)/gui-pic/libvterm/encoding.o \
                       $(OBJDIR)/gui-pic/libvterm/unicode.o \
                       $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -shared -z defs -o $@ \
		$(OBJDIR)/gui-pic/libvterm/vterm.o $(OBJDIR)/gui-pic/libvterm/state.o \
		$(OBJDIR)/gui-pic/libvterm/screen.o $(OBJDIR)/gui-pic/libvterm/parser.o \
		$(OBJDIR)/gui-pic/libvterm/pen.o $(OBJDIR)/gui-pic/libvterm/mouse.o \
		$(OBJDIR)/gui-pic/libvterm/keyboard.o $(OBJDIR)/gui-pic/libvterm/encoding.o \
		$(OBJDIR)/gui-pic/libvterm/unicode.o \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/libvterm.dis
	$(OBJCOPY) --strip-debug $@

# libvterm/src/%.c isn't under gui/ proper, so it needs its own pattern
# rule - same BASH_PIC_CFLAGS/BASH_INC environment as gui-pic/%.o below
# (proven fine for freestanding-but-real-musl userspace code), plus
# -Igui/libvterm/include so its *.c can #include "vterm.h"/
# "vterm_keycodes.h" and its own src/*.h find each other via bare quotes
# (same directory, no -I needed for those).
$(OBJDIR)/gui-pic/libvterm/%.o: gui/libvterm/src/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -Igui/libvterm/include -c -o $@ $<

# libgui.so (GUI roadmap phase 7): a real second shared object,
# DT_NEEDED-linked by every gui/ client instead of statically
# duplicating this code into each one (rasterizer + font rendering +
# the bespoke client wire protocol - see gui/libgui/'s own files).
# Unlike libc.so above, this is a normal shared library loaded via
# ld.so's own DT_NEEDED walk (musl/ldso/dynlink.c's load_library(),
# real unmodified upstream code, confirmed to already work for
# anything libc.so itself needs) - not an ELF interpreter, so no -e
# entry point, and no explicit -soname: consuming binaries link with
# -lgui -L $(BUILD), which records a plain "libgui.so" DT_NEEDED
# entry, found at runtime via ld.so's default /lib:/usr/local/lib:
# /usr/lib search path once installed at /usr/lib/libgui.so (see
# MKFS_INSTALL below) - exactly how /usr/lib/libc.so itself is
# already found today, just for a second .so for the first time.
$(BUILD)/libgui.so: $(OBJDIR)/gui-pic/libgui/gfx.o $(OBJDIR)/gui-pic/libgui/font.o \
                     $(OBJDIR)/gui-pic/libgui/wire.o $(OBJDIR)/gui-pic/libgui/client.o \
                     $(OBJDIR)/gui-pic/libgui/ttf.o \
                     $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -shared -z defs -o $@ \
		$(OBJDIR)/gui-pic/libgui/gfx.o $(OBJDIR)/gui-pic/libgui/font.o \
		$(OBJDIR)/gui-pic/libgui/wire.o $(OBJDIR)/gui-pic/libgui/client.o \
		$(OBJDIR)/gui-pic/libgui/ttf.o \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/libgui.dis
	$(OBJCOPY) --strip-debug $@

# Scrt1: musl/crt/Scrt1.c is just "#include crt1.c" (see musl/crt/
# crt1.c) - the *-fPIC compiled* variant of the same _start real musl
# uses for every dynamically-linked (non-static) executable, calling
# __libc_start_main() via the PLT like any other libc.so import
# rather than linking it in directly the way crt1.o did.

# coreutils/: real GNU coreutils 9.5 source (src/, lib/ - the latter is
# gnulib, coreutils' portability library) vendored the same way musl/
# is - unmodified upstream, except coreutils/lib/config.h and
# coreutils/lib/{error.h,stdckdint.h,configmake.h}/src/{version.h,
# version.c}, which are what a real ./configure + make run would
# normally *generate* (not source you'd hand-write) - vendored
# pre-generated instead, since this Makefile doesn't run autoconf/
# automake at all (the build machine may not even have them - see
# coreutils/poc/config.h's own comment for how these were produced:
# adapted from a native, non-cross ./configure run, not written from
# scratch). coreutils/poc/ is poc-os's own small addition on top - not
# vendored gnulib - the same idea as musl/test/ldso_stubs.c: a
# handful of functions real gnulib only ever declares inside its own
# header *replacements* (lib/wchar.in.h, lib/stdio.in.h, ...), which
# this build skips in favor of musl's real public headers directly
# (see COREUTILS_INC below), plus the handful gnulib has no
# implementation of at all for this libc (__fpending, a translating
# __fstat, a signal-free abort() - poc-os has no signal delivery of
# any kind - see coreutils/poc/coreutils_shims.c, now part of
# MUSL_LDSO_OBJS/libc.so itself rather than linked per-executable).
#
# Every coreutils/gnulib object is -fPIC (COREUTILS_PIC_CFLAGS, not a
# -fno-pie one) and linked into true/false/cat themselves as a PIE
# executable importing libc (open/printf/malloc/...) from libc.so via
# the real ELF dynamic linker - not statically linked, since that's
# what was actually asked for: real dynamic linking against /usr/lib.
#
# Mirrors MUSL_PIC_CFLAGS/INC in shape, not value - std=gnu11 (some
# coreutils source already assumes C23's nullptr/unreachable()),
# -DHAVE_CONFIG_H (every coreutils/gnulib source file starts with
# #include <config.h>), and coreutils/poc's three directories take
# priority in the -I order so its config.h/overrides are seen first.
# -include forces poc_prelude.h ahead of every source file's own first
# #include, the same role -DHAVE_CONFIG_H plays for config.h itself.
COREUTILS_PIC_CFLAGS = -std=gnu11 -ffreestanding -nostdinc -D_XOPEN_SOURCE=700 -DHAVE_CONFIG_H -Os \
	-m64 -mgeneral-regs-only -fno-stack-protector -fPIC \
	-fno-omit-frame-pointer -g -Wall
COREUTILS_INC = -include coreutils/poc/poc_prelude.h -Icoreutils/poc -Icoreutils/lib -Icoreutils/src \
	-Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/include -Imusl/include

$(OBJDIR)/coreutils-pic/%.o: coreutils/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(COREUTILS_PIC_CFLAGS) $(COREUTILS_INC) -c -o $@ $<

# The real gnulib/coreutils objects true/false need beyond libc
# itself - grown file-by-file exactly like MUSL_LIBC_OBJS/
# MUSL_LDSO_OBJS were (see their own comments): starting from "does
# coreutils/src/true.c compile" and adding whichever gnulib source
# each successive undefined reference pointed at. All of this stays
# statically linked into each executable (real gnulib isn't part of
# libc on any real system either) - only the true libc-side functions
# beyond that (COREUTILS_CAT_MUSL_OBJS's musl additions, folded into
# MUSL_LDSO_OBJS/libc.so itself instead - see that variable) come from
# the dynamic linker.
COREUTILS_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/c-ctype.o \
	$(OBJDIR)/coreutils-pic/lib/c-strcasecmp.o \
	$(OBJDIR)/coreutils-pic/lib/close-stream.o \
	$(OBJDIR)/coreutils-pic/lib/closeout.o \
	$(OBJDIR)/coreutils-pic/lib/error.o \
	$(OBJDIR)/coreutils-pic/lib/exitfail.o \
	$(OBJDIR)/coreutils-pic/lib/ialloc.o \
	$(OBJDIR)/coreutils-pic/lib/localcharset.o \
	$(OBJDIR)/coreutils-pic/lib/progname.o \
	$(OBJDIR)/coreutils-pic/lib/propername-lite.o \
	$(OBJDIR)/coreutils-pic/lib/quotearg.o \
	$(OBJDIR)/coreutils-pic/lib/version-etc.o \
	$(OBJDIR)/coreutils-pic/lib/version-etc-fsf.o \
	$(OBJDIR)/coreutils-pic/lib/xalloc-die.o \
	$(OBJDIR)/coreutils-pic/lib/xmalloc.o \
	$(OBJDIR)/coreutils-pic/src/version.o \

# true/false: PIE, -e _start via musl/crt/Scrt1.c - run as
# "runmusl true"/"runmusl false" like every other musl-crt1-style
# UPROGS entry, not directly by name.
$(BUILD)/_true: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/true.o \
		$(COREUTILS_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/true.o \
		$(COREUTILS_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/true.dis
	$(OBJCOPY) --strip-debug $@

# false.c is coreutils/src/false.c itself just "#define EXIT_STATUS
# EXIT_FAILURE" then "#include \"true.c\"" - same object set as true.
$(BUILD)/_false: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/false.o \
		$(COREUTILS_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/false.o \
		$(COREUTILS_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/false.dis
	$(OBJCOPY) --strip-debug $@

# The further gnulib objects cat.c needs beyond COREUTILS_GNULIB_OBJS's
# true/false-derived subset - grown the exact same file-by-file way,
# starting from "does coreutils/src/cat.c compile" this time. Real
# file I/O (open/read/close/fstat) is what actually distinguishes
# cat.c from true.c/false.c here, not anything cat-specific - the
# next utility with real file I/O should need few, if any, further
# gnulib additions beyond this set (any further libc-side needs go
# into MUSL_LDSO_OBJS/libc.so instead, not here).
COREUTILS_CAT_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/alignalloc.o \
	$(OBJDIR)/coreutils-pic/lib/copy-file-range.o \
	$(OBJDIR)/coreutils-pic/lib/fadvise.o \
	$(OBJDIR)/coreutils-pic/lib/full-write.o \
	$(OBJDIR)/coreutils-pic/lib/getopt.o \
	$(OBJDIR)/coreutils-pic/lib/getopt1.o \
	$(OBJDIR)/coreutils-pic/lib/safe-read.o \
	$(OBJDIR)/coreutils-pic/lib/safe-write.o \
	$(OBJDIR)/coreutils-pic/lib/xalignalloc.o \

$(BUILD)/_gcat: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/cat.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/cat.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/gcat.dis
	$(OBJCOPY) --strip-debug $@

# echo: like true/false/cat, every one of these utilities' main() parses
# --help/--version through gnulib's getopt_long (COREUTILS_CAT_GNULIB_OBJS'
# getopt.o/getopt1.o, despite the name - see that variable's own comment)
# even when it takes no other options, so every rule below needs it, not
# just cat's own real-file-I/O additions.
$(BUILD)/_gecho: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/echo.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/echo.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/gecho.dis
	$(OBJCOPY) --strip-debug $@

# basename/dirname: share basename-lgpl.o (the actual path-splitting
# logic); basename.c additionally needs stripslash.o (trailing-slash
# removal, which dirname doesn't do).
COREUTILS_BASENAME_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \

$(BUILD)/_basename: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/basename.o \
		$(OBJDIR)/coreutils-pic/lib/basename.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_BASENAME_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/basename.o \
		$(OBJDIR)/coreutils-pic/lib/basename.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_BASENAME_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/basename.dis
	$(OBJCOPY) --strip-debug $@

$(BUILD)/_dirname: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/dirname.o \
		$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_BASENAME_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/dirname.o \
		$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_BASENAME_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/dirname.dis
	$(OBJCOPY) --strip-debug $@

# yes: long-options.o is --help/--version's shared "--help"/"--version
# alone on the command line" handling, gnulib's usual long_options()
# helper.
$(BUILD)/_yes: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/yes.o \
		$(OBJDIR)/coreutils-pic/lib/long-options.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/yes.o \
		$(OBJDIR)/coreutils-pic/lib/long-options.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) -L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/yes.dis
	$(OBJCOPY) --strip-debug $@

# head/tr/cut: all three compile and link fine (see git history if
# reviving this), but real FILE*-buffered I/O (fopen/fgetc/ungetc/
# setvbuf/...) pushes libc.so past MAXFILE. MAXFILE is no longer the
# ~70KB single-indirect ceiling it once was (see include/fs.h's own
# MAXFILE comment - kernel/fs.c's bmap()/itrunc() and mkfs/mkfs.c's
# iappend() all grew real doubly-indirect block support specifically
# because this ceiling kept blocking libc.so growth), so reviving these
# three is now plausible again - just not yet done; nobody's re-run the
# resolver against them since the filesystem was extended.

# mkdir/rmdir/rm/ln/chmod/pwd: the first coreutils additions since the
# doubly-indirect MAXFILE extension - real directory operations, which
# is also why they needed SYS_getdents/SYS_fchdir (kernel/sysfile.c) to
# exist at all (opendirat.o/fts.o's real getdents()-based directory
# traversal, not just single-file open/read/write like cat/echo).
# Object lists below came from the same resolver-driven, one-undefined-
# symbol-at-a-time process as COREUTILS_GNULIB_OBJS/
# COREUTILS_CAT_GNULIB_OBJS above.
COREUTILS_MKDIR_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/mkdir-p.o \
	$(OBJDIR)/coreutils-pic/lib/modechange.o \
	$(OBJDIR)/coreutils-pic/src/prog-fprintf.o \
	$(OBJDIR)/coreutils-pic/lib/savewd.o \
	$(OBJDIR)/coreutils-pic/lib/dirchownmod.o \
	$(OBJDIR)/coreutils-pic/lib/mkancesdirs.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_mkdir: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/mkdir.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_MKDIR_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/mkdir.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_MKDIR_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/mkdir.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_RMDIR_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/src/prog-fprintf.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \

$(BUILD)/_rmdir: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/rmdir.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_RMDIR_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/rmdir.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_RMDIR_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/rmdir.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_RM_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/lib/closein.o \
	$(OBJDIR)/coreutils-pic/lib/root-dev-ino.o \
	$(OBJDIR)/coreutils-pic/src/remove.o \
	$(OBJDIR)/coreutils-pic/lib/yesno.o \
	$(OBJDIR)/coreutils-pic/lib/write-any-file.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/file-type.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/fts.o \
	$(OBJDIR)/coreutils-pic/lib/xfts.o \
	$(OBJDIR)/coreutils-pic/lib/c-file-type.o \
	$(OBJDIR)/coreutils-pic/lib/cycle-check.o \
	$(OBJDIR)/coreutils-pic/lib/i-ring.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_rm: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/rm.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_RM_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/rm.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_RM_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/rm.dis
	$(OBJCOPY) --strip-debug $@

# scratch_buffer_grow.o/scratch_buffer_grow_preserve.o genuinely live at
# coreutils/lib/malloc/ (not coreutils/lib/ directly, unlike everything
# else in this list) - $(OBJDIR)/coreutils-pic/%.o: coreutils/%.c's
# pattern rule mirrors that nesting, so these two are the only entries
# below with a lib/malloc/ path component rather than a flat lib/ one.
COREUTILS_LN_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/unlinkdir.o \
	$(OBJDIR)/coreutils-pic/lib/canonicalize.o \
	$(OBJDIR)/coreutils-pic/lib/closein.o \
	$(OBJDIR)/coreutils-pic/lib/dirname.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/backup-find.o \
	$(OBJDIR)/coreutils-pic/src/force-link.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/file-set.o \
	$(OBJDIR)/coreutils-pic/src/relpath.o \
	$(OBJDIR)/coreutils-pic/lib/same.o \
	$(OBJDIR)/coreutils-pic/lib/backupfile.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple-simple.o \
	$(OBJDIR)/coreutils-pic/lib/yesno.o \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow_preserve.o \
	$(OBJDIR)/coreutils-pic/lib/hash-pjw.o \
	$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/renameatu.o \
	$(OBJDIR)/coreutils-pic/lib/tempname.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_ln: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/ln.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_LN_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/ln.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_LN_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/ln.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_CHMOD_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/xfts.o \
	$(OBJDIR)/coreutils-pic/lib/root-dev-ino.o \
	$(OBJDIR)/coreutils-pic/lib/modechange.o \
	$(OBJDIR)/coreutils-pic/lib/fts.o \
	$(OBJDIR)/coreutils-pic/lib/filemode.o \
	$(OBJDIR)/coreutils-pic/lib/cycle-check.o \
	$(OBJDIR)/coreutils-pic/lib/i-ring.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_chmod: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/chmod.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_CHMOD_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/chmod.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_CHMOD_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/chmod.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_PWD_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/root-dev-ino.o \
	$(OBJDIR)/coreutils-pic/lib/xgetcwd.o \

$(BUILD)/_pwd: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/pwd.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_PWD_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/pwd.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_PWD_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/pwd.dis
	$(OBJCOPY) --strip-debug $@

# mv/cp: needed a real SYS_ftruncate (include/syscall.h, kernel/fs.c's
# itruncto(), kernel/sysfile.c's sys_ftruncate()) - coreutils/src/
# copy.c calls a real ftruncate() to record a sparse copy's final
# length - plus qcopy_acl()/qset_acl() actually linking, which turned
# out to be a config.h problem, not a missing-implementation one:
# coreutils/poc/config.h's USE_ACL/HAVE_SYS_ACL_H were both left at the
# values a native macOS ./configure run detected (macOS has its own
# <sys/acl.h>), pulling in gnulib's real ACL codepath and its acl_t
# type poc-os has no definition for. Flipped both off (see that file's
# own history) so gnulib's own no-ACL fallback path (plain chmod, via
# coreutils/lib/{qcopy,qset}-acl.c's USE_ACL==0 branch) compiles
# instead - the same "config.h assumed the wrong host" class of fix as
# HAVE_FCLONEFILEAT above, not a new shim.
COREUTILS_MV_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/lib/closein.o \
	$(OBJDIR)/coreutils-pic/src/copy.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/root-dev-ino.o \
	$(OBJDIR)/coreutils-pic/src/cp-hash.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/renameatu.o \
	$(OBJDIR)/coreutils-pic/src/remove.o \
	$(OBJDIR)/coreutils-pic/lib/backupfile.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \
	$(OBJDIR)/coreutils-pic/lib/targetdir.o \
	$(OBJDIR)/coreutils-pic/lib/backup-find.o \
	$(OBJDIR)/coreutils-pic/lib/areadlink-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/areadlinkat-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/backup-rename.o \
	$(OBJDIR)/coreutils-pic/lib/buffer-lcm.o \
	$(OBJDIR)/coreutils-pic/lib/write-any-file.o \
	$(OBJDIR)/coreutils-pic/lib/canonicalize.o \
	$(OBJDIR)/coreutils-pic/lib/copy-acl.o \
	$(OBJDIR)/coreutils-pic/lib/dirname.o \
	$(OBJDIR)/coreutils-pic/lib/fdutimensat.o \
	$(OBJDIR)/coreutils-pic/lib/file-type.o \
	$(OBJDIR)/coreutils-pic/src/force-link.o \
	$(OBJDIR)/coreutils-pic/lib/chmodat.o \
	$(OBJDIR)/coreutils-pic/lib/chownat.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/same-inode.o \
	$(OBJDIR)/coreutils-pic/lib/qset-acl.o \
	$(OBJDIR)/coreutils-pic/lib/file-set.o \
	$(OBJDIR)/coreutils-pic/lib/fts.o \
	$(OBJDIR)/coreutils-pic/lib/same.o \
	$(OBJDIR)/coreutils-pic/lib/savedir.o \
	$(OBJDIR)/coreutils-pic/lib/set-acl.o \
	$(OBJDIR)/coreutils-pic/lib/filemode.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple-simple.o \
	$(OBJDIR)/coreutils-pic/lib/utimecmp.o \
	$(OBJDIR)/coreutils-pic/lib/xfts.o \
	$(OBJDIR)/coreutils-pic/lib/yesno.o \
	$(OBJDIR)/coreutils-pic/lib/c-file-type.o \
	$(OBJDIR)/coreutils-pic/lib/cycle-check.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/acl-internal.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow_preserve.o \
	$(OBJDIR)/coreutils-pic/lib/hash-pjw.o \
	$(OBJDIR)/coreutils-pic/lib/i-ring.o \
	$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/opendir-safer.o \
	$(OBJDIR)/coreutils-pic/lib/qcopy-acl.o \
	$(OBJDIR)/coreutils-pic/lib/set-permissions.o \
	$(OBJDIR)/coreutils-pic/lib/tempname.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \
	$(OBJDIR)/coreutils-pic/lib/get-permissions.o \

$(BUILD)/_mv: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/mv.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_MV_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/mv.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_MV_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/mv.dis
	$(OBJCOPY) --strip-debug $@

COREUTILS_CP_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/src/copy.o \
	$(OBJDIR)/coreutils-pic/lib/closein.o \
	$(OBJDIR)/coreutils-pic/lib/copy-acl.o \
	$(OBJDIR)/coreutils-pic/lib/dirname-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/backup-find.o \
	$(OBJDIR)/coreutils-pic/src/cp-hash.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/chmodat.o \
	$(OBJDIR)/coreutils-pic/lib/chownat.o \
	$(OBJDIR)/coreutils-pic/lib/backupfile.o \
	$(OBJDIR)/coreutils-pic/lib/stripslash.o \
	$(OBJDIR)/coreutils-pic/lib/targetdir.o \
	$(OBJDIR)/coreutils-pic/lib/areadlink-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/areadlinkat-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/backup-rename.o \
	$(OBJDIR)/coreutils-pic/lib/buffer-lcm.o \
	$(OBJDIR)/coreutils-pic/lib/write-any-file.o \
	$(OBJDIR)/coreutils-pic/lib/canonicalize.o \
	$(OBJDIR)/coreutils-pic/lib/dirname.o \
	$(OBJDIR)/coreutils-pic/lib/fdutimensat.o \
	$(OBJDIR)/coreutils-pic/src/force-link.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/open-safer.o \
	$(OBJDIR)/coreutils-pic/lib/openat-safer.o \
	$(OBJDIR)/coreutils-pic/lib/opendirat.o \
	$(OBJDIR)/coreutils-pic/lib/same-inode.o \
	$(OBJDIR)/coreutils-pic/lib/qcopy-acl.o \
	$(OBJDIR)/coreutils-pic/lib/qset-acl.o \
	$(OBJDIR)/coreutils-pic/lib/file-set.o \
	$(OBJDIR)/coreutils-pic/lib/renameatu.o \
	$(OBJDIR)/coreutils-pic/lib/same.o \
	$(OBJDIR)/coreutils-pic/lib/savedir.o \
	$(OBJDIR)/coreutils-pic/lib/set-acl.o \
	$(OBJDIR)/coreutils-pic/lib/filemode.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple-simple.o \
	$(OBJDIR)/coreutils-pic/lib/utimecmp.o \
	$(OBJDIR)/coreutils-pic/lib/yesno.o \
	$(OBJDIR)/coreutils-pic/lib/fd-safer.o \
	$(OBJDIR)/coreutils-pic/lib/acl-internal.o \
	$(OBJDIR)/coreutils-pic/lib/get-permissions.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow_preserve.o \
	$(OBJDIR)/coreutils-pic/lib/hash-pjw.o \
	$(OBJDIR)/coreutils-pic/lib/opendir-safer.o \
	$(OBJDIR)/coreutils-pic/lib/set-permissions.o \
	$(OBJDIR)/coreutils-pic/lib/tempname.o \
	$(OBJDIR)/coreutils-pic/lib/dup-safer.o \

$(BUILD)/_cp: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/cp.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_CP_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/cp.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_CP_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/cp.dis
	$(OBJCOPY) --strip-debug $@

# ls: needed real getdents (SYS_getdents/opendir - already added for
# mkdir/rm/etc's fts.c above) plus three gnulib functions this port has
# no working real implementation of at all - not shimmed as no-ops,
# genuinely reimplemented:
#   - human_options()/human_readable() (coreutils/poc/human_shim.c):
#     real gnulib (lib/human.c) formats sizes with `long double`
#     arithmetic even in the plain, non "-h" case - needs the x87 FPU,
#     unavailable under -mgeneral-regs-only (see this file's own
#     COREUTILS_PIC_CFLAGS comment) for the same reason gnulib's
#     hash.c did (coreutils_shims.c's own hash_initialize comment).
#     Plain 64-bit integer arithmetic throughout instead - exact, not
#     an approximation, for any size poc-os's own MAXFILE could ever
#     actually produce.
#   - timespec_cmp() (coreutils/poc/timespec_shim.c): timespec.h
#     declares this as a plain C99 "inline" (needs one real external
#     instantiation to exist - same rule chmodat()/chownat()/
#     psame_inode() already needed one for), but the file gnulib
#     provides for that (lib/timespec.c) forces every OTHER inline in
#     the same header to instantiate too, including timespectod() -
#     which needs the FPU for the exact same reason human_readable()
#     does. A tiny replacement providing just the one function ls -l
#     actually calls sidesteps the FPU entirely.
# Also needed config.h's USE_ACL/HAVE_SYS_ACL_H fix (mv/cp's own
# comment above), SETLOCALE_NULL_MAX/setlocale_null_r and c32iscntrl/
# c32width declarations (poc_prelude.h - gnulib's own header
# replacements are the only place that normally declare these), and
# sigaction()/sigprocmask()/signal()/getpwnam()/getpwuid()/getgrnam()/
# getgrgid()/gethostname() (coreutils_shims.c - poc-os has no signal
# delivery or user/group database of any kind, so "always succeeded,
# nothing to report" is the accurate answer, not a fake success).
COREUTILS_LS_GNULIB_OBJS = \
	$(OBJDIR)/coreutils-pic/lib/argmatch.o \
	$(OBJDIR)/coreutils-pic/lib/obstack.o \
	$(OBJDIR)/coreutils-pic/lib/areadlink-with-size.o \
	$(OBJDIR)/coreutils-pic/lib/c-strncasecmp.o \
	$(OBJDIR)/coreutils-pic/lib/canonicalize.o \
	$(OBJDIR)/coreutils-pic/lib/file-has-acl.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat.o \
	$(OBJDIR)/coreutils-pic/lib/filemode.o \
	$(OBJDIR)/coreutils-pic/lib/filevercmp.o \
	$(OBJDIR)/coreutils-pic/lib/idcache.o \
	$(OBJDIR)/coreutils-pic/lib/gettime.o \
	$(OBJDIR)/coreutils-pic/lib/hard-locale.o \
	$(OBJDIR)/coreutils-pic/poc/human_shim.o \
	$(OBJDIR)/coreutils-pic/lib/imaxtostr.o \
	$(OBJDIR)/coreutils-pic/lib/basename-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/time_rz.o \
	$(OBJDIR)/coreutils-pic/src/ls-dir.o \
	$(OBJDIR)/coreutils-pic/lib/mbswidth.o \
	$(OBJDIR)/coreutils-pic/lib/mpsort.o \
	$(OBJDIR)/coreutils-pic/lib/nstrftime.o \
	$(OBJDIR)/coreutils-pic/poc/timespec_shim.o \
	$(OBJDIR)/coreutils-pic/lib/umaxtostr.o \
	$(OBJDIR)/coreutils-pic/lib/xgethostname.o \
	$(OBJDIR)/coreutils-pic/lib/xdectoumax.o \
	$(OBJDIR)/coreutils-pic/lib/xstrtol-error.o \
	$(OBJDIR)/coreutils-pic/lib/xstrtoumax.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow.o \
	$(OBJDIR)/coreutils-pic/lib/malloc/scratch_buffer_grow_preserve.o \
	$(OBJDIR)/coreutils-pic/lib/filenamecat-lgpl.o \
	$(OBJDIR)/coreutils-pic/lib/file-set.o \
	$(OBJDIR)/coreutils-pic/lib/setlocale_null.o \
	$(OBJDIR)/coreutils-pic/lib/hash-triple-simple.o \
	$(OBJDIR)/coreutils-pic/lib/hash-pjw.o \
	$(OBJDIR)/coreutils-pic/lib/setlocale_null-unlocked.o \

$(BUILD)/_ls: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/ls.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_LS_GNULIB_OBJS) \
		$(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/coreutils-pic/src/ls.o \
		$(COREUTILS_GNULIB_OBJS) $(COREUTILS_CAT_GNULIB_OBJS) $(COREUTILS_LS_GNULIB_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/ls.dis
	$(OBJCOPY) --strip-debug $@

# bash/: real GNU bash 5.2 source, vendored the same way musl/ and
# coreutils/ are (unmodified upstream) - bash/poc/ is poc-os's own
# addition on top, same idea as coreutils/poc/: config.h (normally
# produced by bash's own ./configure, here adapted from a real native,
# non-cross ./configure + make run - see bash/poc/config.h's presence
# for how HAVE_DLOPEN/HAVE_ICONV/HAVE_SYSLOG/HAVE_LIBDL/
# HAVE_LOCALE_CHARSET got turned back off for poc-os, and JOB_CONTROL/
# HISTORY/readline are off because --disable-job-control/--disable-
# history/--disable-readline were passed to that native configure -
# job control needs process groups/sessions/tcsetpgrp poc-os's kernel
# doesn't have, and readline needs a termcap/terminfo library this
# build has none of), pathnames.h/version.h/pipesize.h (pipesize.h
# hand-corrected to poc-os's own real kernel/pipe.c PIPESIZE, 512 -
# the native run's own probe measured *macOS's* pipe buffer, 64KB,
# which would just be a wrong answer for `ulimit -p` here), syntax.c
# and builtins.c/builtext.h (host-independent generated code, reused
# as-is), bash/poc/builtins/*.c (also generated - by builtins/
# mkbuiltins from builtins/*.def, which bash's own build deletes right
# after compiling in the ordinary case; regenerated here via a direct
# ./mkbuiltins invocation instead so they persist), and signames.h/
# lsignames.h (NOT reused from the native run - mksignames normally
# enumerates the *host's* <signal.h>, so a native run bakes in macOS's
# BSD-flavored signal set (SIGEMT, SIGINFO, ...); hand-derived instead
# from musl/arch/x86_64/bits/signal.h's real SIGHUP..SIGSYS numbering
# plus the standard glibc/musl SIGRTMIN=34/SIGRTMAX=64 real-time-signal
# convention - see bash/poc/signames.h's own comment). bash/poc/
# bash_prelude.h/bash_shims.c fill the remaining real libc-shaped gaps
# (dup2(), uname(), getrlimit()/setrlimit(), times() - poc-os has no
# syscall for any of these), the same role coreutils/poc/
# coreutils_shims.c plays for coreutils - part of libc.so itself (see
# MUSL_LDSO_OBJS above), not linked per-executable.
#
# job control disabled (see config.h above) means bash/nojobs.c (the
# stub job-control engine), not bash/jobs.c, is what actually gets
# built into BASH_OBJS below - matching bash's own JOBS_O=nojobs.o
# selection for this exact configuration.
#
# Every bash/gnulib-style object here is -fPIC (BASH_PIC_CFLAGS) and
# linked into build/_bash as a real PIE executable importing libc from
# libc.so via the ELF dynamic linker, exactly like coreutils'
# COREUTILS_PIC_CFLAGS/true/false/cat/etc above - not statically
# linked. This -fPIC+libc.so recipe (Scrt1.o + object set + -pie
# --dynamic-linker /usr/lib/libc.so -lc) is now poc-os's *default*
# convention for any future userland software, not just bash/
# coreutils - the static ULIB/xv6-native path (user/*.c, include/
# user.h) is retired (see git history for the old user/sh.c+user/
# init.c pair this replaced).
#
# -DCONF_HOSTTYPE/-DCONF_OSTYPE/-DCONF_MACHTYPE: bash/conftypes.h
# expects these predefined (normally by configure, from config.h's own
# uname-derived guess) to build $HOSTTYPE/$OSTYPE/$MACHTYPE - poc-os's
# own identity, not a borrowed host one.
# -Wno-error=implicit-function-declaration: bash/parse.y's own 'j' (job
# count) prompt-expansion case calls count_all_jobs() (bash/nojobs.c -
# JOB_CONTROL is off, see config.h - really does define it) without
# #include "jobs.h" in scope - a real, harmless upstream gap (GCC 16
# defaults this to a hard error; older GCC/the native macOS build above
# only ever warned, which is why nobody upstream noticed).
# curses/: poc-os's own minimal curses (curses/curses.h's own comment -
# not a vendored real ncurses, there's no upstream being tracked here),
# built for the nano port. -fPIC like every other *-pic tree above, but
# unlike musl-pic/coreutils-pic/bash-pic there's no libc.so involvement
# at all - curses is nano-only, so its objects just link straight into
# build/_nano's own PIE (once nano exists) the same way BASH_OBJS links
# straight into _bash, rather than through -lc.
CURSES_PIC_CFLAGS = -std=gnu11 -ffreestanding -nostdinc -D_XOPEN_SOURCE=700 -Os \
	-m64 -mgeneral-regs-only -fno-stack-protector -fPIC \
	-fno-omit-frame-pointer -g -Wall -MD
CURSES_INC = -Icurses/include \
	-Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/include -Imusl/include

$(OBJDIR)/curses-pic/%.o: curses/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(CURSES_PIC_CFLAGS) $(CURSES_INC) -c -o $@ $<

# nano/: real GNU nano 6.4 source (src/, lib/ - the latter is nano's own
# vendored gnulib, pulled in the same unmodified-upstream way musl/
# coreutils/bash are - see the "Stage 3" plan for why 6.4, not the
# latest release: it avoids a whole bundled Unicode "c32" character-type
# library newer nano versions carry, at the cost of a few years of
# upstream history). nano/poc/ is poc-os's own small addition on top -
# not part of nano's own gnulib, and not gnulib itself - handling real
# poc-os/musl gaps, the same role coreutils/poc/ and bash/poc/ play for
# their own ports (see coreutils/poc/config.h's own comment for how
# nano/poc/config.h was produced from scratch: a real native ./configure
# run's output, hand-corrected, not written from scratch).
#
# Every nano/gnulib object here is -fPIC (NANO_PIC_CFLAGS) and linked
# into build/_nano as a real PIE executable importing libc from libc.so
# via the ELF dynamic linker, exactly like bash/coreutils above - not
# statically linked. curses/include is on the include path so nano's own
# <config.h>-driven "#include <curses.h>" (definitions.h) picks up the
# poc-os curses/ library (Stage 2) rather than a real ncurses, since
# HAVE_NCURSES_H is never defined in nano/poc/config.h.
NANO_PIC_CFLAGS = -std=gnu11 -ffreestanding -nostdinc -D_XOPEN_SOURCE=700 -DHAVE_CONFIG_H -Os \
	-m64 -mgeneral-regs-only -fno-stack-protector -fPIC \
	-fno-omit-frame-pointer -g -Wall \
	-Wno-error=implicit-function-declaration -Wno-error=implicit-int -MD
NANO_INC = -include nano/poc/nano_prelude.h -Inano/poc -Inano/lib -Inano/src -Icurses/include \
	-Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/include -Imusl/include

$(OBJDIR)/nano-pic/%.o: nano/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(NANO_PIC_CFLAGS) $(NANO_INC) -c -o $@ $<

$(OBJDIR)/nano-pic/poc/%.o: nano/poc/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(NANO_PIC_CFLAGS) $(NANO_INC) -c -o $@ $<

# The real gnulib objects nano needs beyond libc.so/curses/, found the
# same empirical way as COREUTILS_*_GNULIB_OBJS/BASH_LIB_OBJS above:
# starting from "does nano/src/*.c compile and link" and adding
# whichever gnulib source the linker complained was undefined - see the
# "Stage 3" plan for how much smaller this ended up than the ~78-object
# native-macOS-configure baseline once nano/poc/config.h's HAVE_* was
# corrected for what musl actually provides directly (getopt_long,
# strcasestr, dirname, realpath, getc_unlocked, mkstemps, sigfillset,
# btowc, pthread_mutex_init/destroy - all added to MUSL_LDSO_OBJS above
# instead, real musl code, not gnulib replacements). Just two real gaps
# remained: nano/src/search.c's regex calls are compile-time redirected
# to rpl_regcomp/rpl_regexec/etc (config.h's "#define regcomp
# rpl_regcomp" family - the replacement really is required here, unlike
# the functions above, since nano's own call sites are macro-rewritten
# to the rpl_ names regardless of what musl provides), and regex.c's
# own dynamic-array resizing (regmatch_list_resize) needs gnulib's
# malloc/dynarray_resize.o.
NANO_GNULIB_OBJS = \
	$(OBJDIR)/nano-pic/lib/regex.o \
	$(OBJDIR)/nano-pic/lib/malloc/dynarray_resize.o \

NANO_SRC_OBJS = \
	$(OBJDIR)/nano-pic/src/browser.o \
	$(OBJDIR)/nano-pic/src/chars.o \
	$(OBJDIR)/nano-pic/src/color.o \
	$(OBJDIR)/nano-pic/src/cut.o \
	$(OBJDIR)/nano-pic/src/files.o \
	$(OBJDIR)/nano-pic/src/global.o \
	$(OBJDIR)/nano-pic/src/help.o \
	$(OBJDIR)/nano-pic/src/history.o \
	$(OBJDIR)/nano-pic/src/move.o \
	$(OBJDIR)/nano-pic/src/nano.o \
	$(OBJDIR)/nano-pic/src/prompt.o \
	$(OBJDIR)/nano-pic/src/rcfile.o \
	$(OBJDIR)/nano-pic/src/search.o \
	$(OBJDIR)/nano-pic/src/text.o \
	$(OBJDIR)/nano-pic/src/utils.o \
	$(OBJDIR)/nano-pic/src/winio.o \

NANO_OBJS = $(NANO_SRC_OBJS) $(NANO_GNULIB_OBJS)

$(BUILD)/_nano: $(OBJDIR)/musl-pic/crt/Scrt1.o $(NANO_OBJS) $(OBJDIR)/curses-pic/curses.o $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(NANO_OBJS) $(OBJDIR)/curses-pic/curses.o \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/nano.dis
	$(OBJCOPY) --strip-debug $@

BASH_PIC_CFLAGS = -std=gnu11 -ffreestanding -nostdinc -D_XOPEN_SOURCE=700 -DHAVE_CONFIG_H -DSHELL -Os \
	-DCONF_HOSTTYPE='"x86_64"' -DCONF_OSTYPE='"poc-os"' -DCONF_MACHTYPE='"x86_64-poc-os"' \
	-DPACKAGE='"bash"' -DLOCALEDIR='"/usr/share/locale"' \
	-m64 -mgeneral-regs-only -fno-stack-protector -fPIC \
	-fno-omit-frame-pointer -g -Wall -Wno-parentheses -Wno-format-security \
	-Wno-error=implicit-function-declaration -Wno-error=implicit-int -MD
BASH_INC = -include bash/poc/bash_prelude.h \
	-Ibash/poc -Ibash/poc/builtins -Ibash -Ibash/include -Ibash/builtins -Ibash/lib -Ibash/lib/sh -Ibash/lib/glob -Ibash/lib/tilde \
	-Imusl/arch/x86_64 -Imusl/arch/generic -I$(OBJDIR)/musl/include -Imusl/include

$(OBJDIR)/bash-pic/%.o: bash/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -c -o $@ $<

$(OBJDIR)/bash-pic/poc/%.o: bash/poc/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -c -o $@ $<

# gui/ (GUI roadmap phases 6-10: libgui.so + compositor/login/terminal
# clients) isn't under bash/, so it needs its own explicit pattern rule
# - same BASH_PIC_CFLAGS/BASH_INC environment as bash-pic/poc/%.o
# above (proven by fbtest.c/guitest.c/bashpipetest.c's own needs:
# fork/pipe/mmap/ioctl and bash_prelude.h's uname/getrlimit/times
# shims), plus -Igui/libgui so any gui/*.c can #include "libgui.h"
# without a relative path, and -Igui/libvterm/include so gui/terminal.c
# can #include "vterm.h" the same way.
$(OBJDIR)/gui-pic/%.o: gui/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -Igui/libgui -Igui/libvterm/include -c -o $@ $<

# ttf.c (vendor/stb_truetype.h, real antialiased TrueType text - GUI
# roadmap ToaruOS-style rewrite) is the one file in this whole codebase
# that needs real floating point, so it's the one file built without
# -mgeneral-regs-only (every other user/kernel object stays built with
# it - see kernel/vm.c's fpuinit()/kernel/proc.c's fpu_state for what
# had to change kernel-side before this was even safe to allow at all).
# A target-specific rule like this overrides the %.o pattern rule above
# for this one exact file, per ordinary GNU Make precedence.
$(OBJDIR)/gui-pic/libgui/ttf.o: gui/libgui/ttf.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(subst -mgeneral-regs-only,,$(BASH_PIC_CFLAGS)) $(BASH_INC) -Igui/libgui -c -o $@ $<

$(OBJDIR)/bash-pic/poc/builtins/%.o: bash/poc/builtins/%.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -c -o $@ $<

# Core shell engine (the top-level bash/*.c files the native build's
# own final link line pulled in for this exact config - see this
# section's own comment).
BASH_CORE_OBJS = \
	$(OBJDIR)/bash-pic/shell.o \
	$(OBJDIR)/bash-pic/eval.o \
	$(OBJDIR)/bash-pic/y.tab.o \
	$(OBJDIR)/bash-pic/general.o \
	$(OBJDIR)/bash-pic/make_cmd.o \
	$(OBJDIR)/bash-pic/print_cmd.o \
	$(OBJDIR)/bash-pic/dispose_cmd.o \
	$(OBJDIR)/bash-pic/execute_cmd.o \
	$(OBJDIR)/bash-pic/variables.o \
	$(OBJDIR)/bash-pic/copy_cmd.o \
	$(OBJDIR)/bash-pic/error.o \
	$(OBJDIR)/bash-pic/expr.o \
	$(OBJDIR)/bash-pic/flags.o \
	$(OBJDIR)/bash-pic/nojobs.o \
	$(OBJDIR)/bash-pic/subst.o \
	$(OBJDIR)/bash-pic/hashcmd.o \
	$(OBJDIR)/bash-pic/hashlib.o \
	$(OBJDIR)/bash-pic/mailcheck.o \
	$(OBJDIR)/bash-pic/trap.o \
	$(OBJDIR)/bash-pic/input.o \
	$(OBJDIR)/bash-pic/unwind_prot.o \
	$(OBJDIR)/bash-pic/pathexp.o \
	$(OBJDIR)/bash-pic/sig.o \
	$(OBJDIR)/bash-pic/test.o \
	$(OBJDIR)/bash-pic/version.o \
	$(OBJDIR)/bash-pic/alias.o \
	$(OBJDIR)/bash-pic/array.o \
	$(OBJDIR)/bash-pic/arrayfunc.o \
	$(OBJDIR)/bash-pic/assoc.o \
	$(OBJDIR)/bash-pic/braces.o \
	$(OBJDIR)/bash-pic/bracecomp.o \
	$(OBJDIR)/bash-pic/bashhist.o \
	$(OBJDIR)/bash-pic/bashline.o \
	$(OBJDIR)/bash-pic/list.o \
	$(OBJDIR)/bash-pic/stringlib.o \
	$(OBJDIR)/bash-pic/locale.o \
	$(OBJDIR)/bash-pic/findcmd.o \
	$(OBJDIR)/bash-pic/redir.o \
	$(OBJDIR)/bash-pic/xmalloc.o \
	$(OBJDIR)/bash-pic/poc/syntax.o \
	$(OBJDIR)/bash-pic/poc/builtins.o \

# builtins/: the four hand-written (not .def-generated) support files
# bash's own tarball ships pristine, plus every real builtin command
# (bash/poc/builtins/*.c - .def-generated, see this section's own
# comment for why they're regenerated into bash/poc/ instead of
# bash/builtins/ directly).
BASH_BUILTINS_OBJS = \
	$(OBJDIR)/bash-pic/builtins/common.o \
	$(OBJDIR)/bash-pic/builtins/evalfile.o \
	$(OBJDIR)/bash-pic/builtins/evalstring.o \
	$(OBJDIR)/bash-pic/builtins/bashgetopt.o \
	$(OBJDIR)/bash-pic/builtins/getopt.o \
	$(OBJDIR)/bash-pic/poc/builtins/alias.o \
	$(OBJDIR)/bash-pic/poc/builtins/bind.o \
	$(OBJDIR)/bash-pic/poc/builtins/break.o \
	$(OBJDIR)/bash-pic/poc/builtins/builtin.o \
	$(OBJDIR)/bash-pic/poc/builtins/caller.o \
	$(OBJDIR)/bash-pic/poc/builtins/cd.o \
	$(OBJDIR)/bash-pic/poc/builtins/colon.o \
	$(OBJDIR)/bash-pic/poc/builtins/command.o \
	$(OBJDIR)/bash-pic/poc/builtins/declare.o \
	$(OBJDIR)/bash-pic/poc/builtins/echo.o \
	$(OBJDIR)/bash-pic/poc/builtins/enable.o \
	$(OBJDIR)/bash-pic/poc/builtins/eval.o \
	$(OBJDIR)/bash-pic/poc/builtins/exec.o \
	$(OBJDIR)/bash-pic/poc/builtins/exit.o \
	$(OBJDIR)/bash-pic/poc/builtins/fc.o \
	$(OBJDIR)/bash-pic/poc/builtins/fg_bg.o \
	$(OBJDIR)/bash-pic/poc/builtins/getopts.o \
	$(OBJDIR)/bash-pic/poc/builtins/hash.o \
	$(OBJDIR)/bash-pic/poc/builtins/help.o \
	$(OBJDIR)/bash-pic/poc/builtins/kill.o \
	$(OBJDIR)/bash-pic/poc/builtins/let.o \
	$(OBJDIR)/bash-pic/poc/builtins/mapfile.o \
	$(OBJDIR)/bash-pic/poc/builtins/printf.o \
	$(OBJDIR)/bash-pic/poc/builtins/pushd.o \
	$(OBJDIR)/bash-pic/poc/builtins/read.o \
	$(OBJDIR)/bash-pic/poc/builtins/return.o \
	$(OBJDIR)/bash-pic/poc/builtins/set.o \
	$(OBJDIR)/bash-pic/poc/builtins/setattr.o \
	$(OBJDIR)/bash-pic/poc/builtins/shift.o \
	$(OBJDIR)/bash-pic/poc/builtins/shopt.o \
	$(OBJDIR)/bash-pic/poc/builtins/source.o \
	$(OBJDIR)/bash-pic/poc/builtins/suspend.o \
	$(OBJDIR)/bash-pic/poc/builtins/test.o \
	$(OBJDIR)/bash-pic/poc/builtins/times.o \
	$(OBJDIR)/bash-pic/poc/builtins/trap.o \
	$(OBJDIR)/bash-pic/poc/builtins/type.o \
	$(OBJDIR)/bash-pic/poc/builtins/ulimit.o \
	$(OBJDIR)/bash-pic/poc/builtins/umask.o \
	$(OBJDIR)/bash-pic/poc/builtins/wait.o \

# lib/sh, lib/glob, lib/tilde: bash's own portability/pattern-matching/
# ~-expansion support libraries (the real gnulib-equivalent bash ships
# itself) - member lists taken directly from the native run's own
# libsh.a/libglob.a/libtilde.a (no lib/readline/libhistory.a: HISTORY
# is off - see config.h - so bash's own code never calls into it).
BASH_LIB_OBJS = \
	$(OBJDIR)/bash-pic/lib/sh/clktck.o \
	$(OBJDIR)/bash-pic/lib/sh/clock.o \
	$(OBJDIR)/bash-pic/lib/sh/getenv.o \
	$(OBJDIR)/bash-pic/lib/sh/oslib.o \
	$(OBJDIR)/bash-pic/lib/sh/setlinebuf.o \
	$(OBJDIR)/bash-pic/lib/sh/strnlen.o \
	$(OBJDIR)/bash-pic/lib/sh/itos.o \
	$(OBJDIR)/bash-pic/lib/sh/zread.o \
	$(OBJDIR)/bash-pic/lib/sh/zwrite.o \
	$(OBJDIR)/bash-pic/lib/sh/shtty.o \
	$(OBJDIR)/bash-pic/lib/sh/shmatch.o \
	$(OBJDIR)/bash-pic/lib/sh/eaccess.o \
	$(OBJDIR)/bash-pic/lib/sh/timeval.o \
	$(OBJDIR)/bash-pic/lib/sh/makepath.o \
	$(OBJDIR)/bash-pic/lib/sh/pathcanon.o \
	$(OBJDIR)/bash-pic/lib/sh/pathphys.o \
	$(OBJDIR)/bash-pic/lib/sh/tmpfile.o \
	$(OBJDIR)/bash-pic/lib/sh/stringlist.o \
	$(OBJDIR)/bash-pic/lib/sh/stringvec.o \
	$(OBJDIR)/bash-pic/lib/sh/spell.o \
	$(OBJDIR)/bash-pic/lib/sh/shquote.o \
	$(OBJDIR)/bash-pic/lib/sh/strtrans.o \
	$(OBJDIR)/bash-pic/lib/sh/snprintf.o \
	$(OBJDIR)/bash-pic/lib/sh/mailstat.o \
	$(OBJDIR)/bash-pic/lib/sh/fmtulong.o \
	$(OBJDIR)/bash-pic/lib/sh/fmtullong.o \
	$(OBJDIR)/bash-pic/lib/sh/fmtumax.o \
	$(OBJDIR)/bash-pic/lib/sh/zcatfd.o \
	$(OBJDIR)/bash-pic/lib/sh/zmapfd.o \
	$(OBJDIR)/bash-pic/lib/sh/winsize.o \
	$(OBJDIR)/bash-pic/lib/sh/wcsdup.o \
	$(OBJDIR)/bash-pic/lib/sh/fpurge.o \
	$(OBJDIR)/bash-pic/lib/sh/zgetline.o \
	$(OBJDIR)/bash-pic/lib/sh/mbscmp.o \
	$(OBJDIR)/bash-pic/lib/sh/uconvert.o \
	$(OBJDIR)/bash-pic/lib/sh/ufuncs.o \
	$(OBJDIR)/bash-pic/lib/sh/casemod.o \
	$(OBJDIR)/bash-pic/lib/sh/input_avail.o \
	$(OBJDIR)/bash-pic/lib/sh/mbscasecmp.o \
	$(OBJDIR)/bash-pic/lib/sh/fnxform.o \
	$(OBJDIR)/bash-pic/lib/sh/unicode.o \
	$(OBJDIR)/bash-pic/lib/sh/shmbchar.o \
	$(OBJDIR)/bash-pic/lib/sh/strvis.o \
	$(OBJDIR)/bash-pic/lib/sh/utf8.o \
	$(OBJDIR)/bash-pic/lib/sh/random.o \
	$(OBJDIR)/bash-pic/lib/sh/gettimeofday.o \
	$(OBJDIR)/bash-pic/lib/sh/timers.o \
	$(OBJDIR)/bash-pic/lib/sh/wcsnwidth.o \
	$(OBJDIR)/bash-pic/lib/sh/mktime.o \
	$(OBJDIR)/bash-pic/lib/sh/mbschr.o \
	$(OBJDIR)/bash-pic/lib/sh/strtoimax.o \
	$(OBJDIR)/bash-pic/lib/glob/glob.o \
	$(OBJDIR)/bash-pic/lib/glob/strmatch.o \
	$(OBJDIR)/bash-pic/lib/glob/smatch.o \
	$(OBJDIR)/bash-pic/lib/glob/xmbsrtowcs.o \
	$(OBJDIR)/bash-pic/lib/glob/gmisc.o \
	$(OBJDIR)/bash-pic/lib/tilde/tilde.o \

BASH_OBJS = $(BASH_CORE_OBJS) $(BASH_BUILTINS_OBJS) $(BASH_LIB_OBJS)

$(BUILD)/_bash: $(OBJDIR)/musl-pic/crt/Scrt1.o $(BASH_OBJS) $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(BASH_OBJS) \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/bash.dis
	$(OBJCOPY) --strip-debug $@

# dinit: PID 1, replacing the static ULIB user/init.c - same
# Scrt1.o+libc.so PIE recipe as bash/coreutils above, not the static
# xv6-native path. Logic mirrors user/init.c exactly (console setup,
# fork+exec+reap loop), just built as a real dynamic binary and
# starting bash -i instead of sh - see bash/poc/dinit.c's own comments.
$(OBJDIR)/bash-pic/poc/dinit.o: bash/poc/dinit.c $(MUSL_GENH)
	@mkdir -p $(dir $@)
	$(CC) $(BASH_PIC_CFLAGS) $(BASH_INC) -c -o $@ $<

$(BUILD)/_dinit: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/bash-pic/poc/dinit.o $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/bash-pic/poc/dinit.o \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/dinit.dis
	$(OBJCOPY) --strip-debug $@

# login/su: multi-user support's account-switching pair - see bash/poc/
# login.c's and su.c's own comments. Same Scrt1.o+libc.so PIE recipe as
# every other dynamically-linked program above, using the existing
# generic $(OBJDIR)/bash-pic/poc/%.o pattern rule (plain musl-linked C,
# not bash source, but that rule's BASH_PIC_CFLAGS/BASH_INC work fine
# unchanged - same reasoning as rawtest.o not needing its own).
$(BUILD)/_login: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/bash-pic/poc/login.o $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/bash-pic/poc/login.o \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/login.dis
	$(OBJCOPY) --strip-debug $@

$(BUILD)/_su: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/bash-pic/poc/su.o $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/bash-pic/poc/su.o \
		-L $(BUILD) -lc
	$(OBJDUMP) -S $@ > $(BUILD)/su.dis
	$(OBJCOPY) --strip-debug $@

# compositor: gui/compositor.c's own comment - GUI roadmap phase 8's
# real yutani-equivalent daemon. Compiled via the gui-pic/%.o generic
# pattern rule above (already has -Igui/libgui built in), linked
# against libgui.so (rasterizer/font) same as libguitest above.
$(BUILD)/_compositor: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/compositor.o \
                       $(BUILD)/libgui.so $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie -z now --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/compositor.o \
		-L $(BUILD) -lgui -lc
	$(OBJDUMP) -S $@ > $(BUILD)/compositor.dis
	$(OBJCOPY) --strip-debug $@

# bootsplash: gui/bootsplash.c's own comment - a one-shot program that
# draws the boot splash image directly to the real framebuffer and
# exits, run by bash/poc/dinit.c before the compositor ever starts.
# Compiled via the gui-pic/%.o generic pattern rule, linked against
# libgui.so (gfx_load_raw()/gfx_blit()) same as compositor above.
$(BUILD)/_bootsplash: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/bootsplash.o \
                       $(BUILD)/libgui.so $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie -z now --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/bootsplash.o \
		-L $(BUILD) -lgui -lc
	$(OBJDUMP) -S $@ > $(BUILD)/bootsplash.dis
	$(OBJCOPY) --strip-debug $@

# login_gui: gui/login_gui.c's own comment - GUI roadmap phase 9's
# graphical login screen. Compiled via the gui-pic/%.o generic pattern
# rule (already has -Igui/libgui built in), linked against libgui.so
# same as compositor above.
$(BUILD)/_login_gui: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/login_gui.o \
                      $(BUILD)/libgui.so $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie -z now --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/login_gui.o \
		-L $(BUILD) -lgui -lc
	$(OBJDUMP) -S $@ > $(BUILD)/login_gui.dis
	$(OBJCOPY) --strip-debug $@

# desktop: gui/desktop.c's own comment - the desktop shell login_gui.c
# hands off to. Compiled via the gui-pic/%.o generic pattern rule
# (already has -Igui/libgui built in), linked against libgui.so same
# as compositor/login_gui/terminal above.
$(BUILD)/_desktop: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/desktop.o \
                    $(BUILD)/libgui.so $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie -z now --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/desktop.o \
		-L $(BUILD) -lgui -lc
	$(OBJDUMP) -S $@ > $(BUILD)/desktop.dis
	$(OBJCOPY) --strip-debug $@

# terminal: gui/terminal.c's own comment - GUI roadmap phase 10's
# terminal emulator client, now driven by vendored libvterm
# (gui/libvterm/) instead of a hand-rolled CSI/SGR parser. Compiled via
# the gui-pic/%.o generic pattern rule (has -Igui/libgui and
# -Igui/libvterm/include built in), dynamically linked against both
# libgui.so and libvterm.so same as compositor/login_gui link against
# libgui.so alone.
$(BUILD)/_terminal: $(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/terminal.o \
                     $(BUILD)/libgui.so $(BUILD)/libvterm.so $(BUILD)/libc.so | $(BUILD)
	$(LD) -m elf_x86_64 -pie -z now --dynamic-linker /usr/lib/libc.so -o $@ \
		$(OBJDIR)/musl-pic/crt/Scrt1.o $(OBJDIR)/gui-pic/terminal.o \
		-L $(BUILD) -lgui -lvterm -lc
	$(OBJDUMP) -S $@ > $(BUILD)/terminal.dis
	$(OBJCOPY) --strip-debug $@

$(BUILD)/mkfs: mkfs/mkfs.c include/fs.h | $(BUILD)
	# -iquote (not -I) so quoted poc headers resolve to include/ while
	# <fcntl.h> etc still resolve to the host's system headers.
	gcc -Werror -Wall -iquote include -o $(BUILD)/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: $(OBJDIR)/user/%.o $(OBJDIR)/kernel/%.o $(OBJDIR)/musl/%.o $(OBJDIR)/musl-test/%.o $(OBJDIR)/musl-pic/%.o $(OBJDIR)/coreutils-pic/%.o $(OBJDIR)/bash-pic/%.o $(OBJDIR)/curses-pic/%.o $(OBJDIR)/nano-pic/%.o

# UPROGS is mkfs/mkfs.c's usual root-placed/underscore-stripped
# convention (a bare host path, e.g. build/_foo -> installed as /foo -
# see mkfs.c's own comment on argv). Empty today: every current binary,
# including init/sh, is instead installed explicitly under /usr/bin via
# MKFS_INSTALL's "imgpath:hostpath" form below, so there's exactly one
# place binaries live rather than some at / and some at /usr/bin. Kept
# (rather than deleted) as the mechanism a future root-placed program
# would still use.
UPROGS=\

# init: PID 1 (the kernel loads user/initcode.asm, which SYS_execs
# this exact path - see that file's own "init:" string) - installed
# under /usr/bin like everything else rather than carved out as a
# root-level exception. bash/poc/dinit.c (dynamic, Scrt1.o+libc.so,
# like bash/coreutils - see its own comment) is the only init poc-os
# has now, starting bash directly; the static xv6-native user/init.c+
# user/sh.c pair is gone - see git history if reviving it.
MKFS_INSTALL = usr/bin/init:$(BUILD)/_dinit
MKFS_INSTALL_DEPS = $(BUILD)/_dinit

# GNU coreutils ports (true/false/cat/echo/basename/dirname/yes,
# runmusl - a manual musl-crt1-style launcher, see musl-test/%.o's own
# comment). libc.so has to live at exactly /usr/lib/libc.so - that
# path is baked into every one of these binaries' own PT_INTERP
# segment (--dynamic-linker /usr/lib/libc.so above) as the dynamic
# linker to load. Kept deliberately small - see $(BUILD)/_ghead's own
# comment above for why head/tr/cut aren't here too (MAXFILE).
MKFS_INSTALL += usr/lib/libgui.so:$(BUILD)/libgui.so
MKFS_INSTALL_DEPS += $(BUILD)/libgui.so

MKFS_INSTALL += usr/lib/libvterm.so:$(BUILD)/libvterm.so
MKFS_INSTALL_DEPS += $(BUILD)/libvterm.so

MKFS_INSTALL += usr/lib/libc.so:$(BUILD)/libc.so usr/bin/true:$(BUILD)/_true \
	usr/bin/false:$(BUILD)/_false usr/bin/cat:$(BUILD)/_gcat \
	usr/bin/echo:$(BUILD)/_gecho usr/bin/basename:$(BUILD)/_basename \
	usr/bin/dirname:$(BUILD)/_dirname usr/bin/yes:$(BUILD)/_yes \
	usr/bin/runmusl:$(BUILD)/_runmusl
MKFS_INSTALL_DEPS += $(BUILD)/libc.so $(BUILD)/_true $(BUILD)/_false \
	$(BUILD)/_gcat $(BUILD)/_gecho $(BUILD)/_basename $(BUILD)/_dirname \
	$(BUILD)/_yes $(BUILD)/_runmusl

# mkdir/rmdir/rm/ln/chmod/pwd: the first additions since include/fs.h's
# doubly-indirect MAXFILE extension - see that comment and each
# COREUTILS_*_GNULIB_OBJS variable's own comment above.
MKFS_INSTALL += usr/bin/mkdir:$(BUILD)/_mkdir usr/bin/rmdir:$(BUILD)/_rmdir \
	usr/bin/rm:$(BUILD)/_rm usr/bin/ln:$(BUILD)/_ln \
	usr/bin/chmod:$(BUILD)/_chmod usr/bin/pwd:$(BUILD)/_pwd
MKFS_INSTALL_DEPS += $(BUILD)/_mkdir $(BUILD)/_rmdir $(BUILD)/_rm \
	$(BUILD)/_ln $(BUILD)/_chmod $(BUILD)/_pwd

# mv/cp: needed real ftruncate() (SYS_ftruncate, kernel/fs.c's
# itruncto()) plus config.h's USE_ACL/HAVE_SYS_ACL_H fixes - see
# COREUTILS_MV_GNULIB_OBJS's own comment above.
MKFS_INSTALL += usr/bin/mv:$(BUILD)/_mv usr/bin/cp:$(BUILD)/_cp
MKFS_INSTALL_DEPS += $(BUILD)/_mv $(BUILD)/_cp

# ls: real directory listing - see COREUTILS_LS_GNULIB_OBJS's own
# comment above for what this needed beyond the mkdir/rm/etc batch's
# getdents/opendir infrastructure.
MKFS_INSTALL += usr/bin/ls:$(BUILD)/_ls
MKFS_INSTALL_DEPS += $(BUILD)/_ls

# bash: dynamically linked (Scrt1.o + libc.so, PIE) the same way as
# every coreutils entry above - see $(BUILD)/_bash's own comment for
# what this needed beyond the musl/coreutils infrastructure already
# built out.
MKFS_INSTALL += usr/bin/bash:$(BUILD)/_bash
MKFS_INSTALL_DEPS += $(BUILD)/_bash

# nano: dynamically linked (Scrt1.o + libc.so, PIE) the same way as
# bash/coreutils above - see $(BUILD)/_nano's own comment.
MKFS_INSTALL += usr/bin/nano:$(BUILD)/_nano
MKFS_INSTALL_DEPS += $(BUILD)/_nano

# Multi-user support: login/su (see their own Makefile build rules and
# bash/poc/{login,su}.c) plus the account database they read - see
# mkfs.c's install_mode_override() for why su specifically gets a
# setuid-root mode despite installfile()'s usual 0755 default, and for
# why etc/* gets 0644 instead.
MKFS_INSTALL += usr/bin/login:$(BUILD)/_login usr/bin/su:$(BUILD)/_su \
	etc/passwd:etc/passwd etc/group:etc/group
MKFS_INSTALL_DEPS += $(BUILD)/_login $(BUILD)/_su etc/passwd etc/group

# compositor/login_gui/terminal: see their own Makefile build rules
# above - GUI roadmap phases 8-10's real compositor daemon, graphical
# login screen, and terminal emulator (what dinit.c actually boots
# into by default - see its own comment).
MKFS_INSTALL += usr/bin/compositor:$(BUILD)/_compositor
MKFS_INSTALL_DEPS += $(BUILD)/_compositor
MKFS_INSTALL += usr/bin/bootsplash:$(BUILD)/_bootsplash
MKFS_INSTALL_DEPS += $(BUILD)/_bootsplash
MKFS_INSTALL += usr/bin/login_gui:$(BUILD)/_login_gui
MKFS_INSTALL_DEPS += $(BUILD)/_login_gui
MKFS_INSTALL += usr/bin/terminal:$(BUILD)/_terminal
MKFS_INSTALL_DEPS += $(BUILD)/_terminal
MKFS_INSTALL += usr/bin/desktop:$(BUILD)/_desktop
MKFS_INSTALL_DEPS += $(BUILD)/_desktop

# GUI assets (ToaruOS-style rewrite): the DejaVu fonts and the raw
# wallpaper/icon assets tools/genraw.py generated - see gui/libgui/
# ttf.c and gfx.c's gfx_load_raw()/gfx_load_raw_rgba() for the readers.
# Plain data files, same imgpath:hostpath MKFS_INSTALL form etc/passwd
# already uses above.
MKFS_INSTALL += usr/share/fonts/dejavu/DejaVuSans.ttf:gui/assets/fonts/DejaVuSans.ttf \
	usr/share/fonts/dejavu/DejaVuSans-Bold.ttf:gui/assets/fonts/DejaVuSans-Bold.ttf \
	usr/share/fonts/dejavu/DejaVuSansMono.ttf:gui/assets/fonts/DejaVuSansMono.ttf \
	usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf:gui/assets/fonts/DejaVuSansMono-Bold.ttf \
	usr/share/wallpaper.raw:gui/assets/images/wallpaper.raw \
	usr/share/icons/terminal.raw:gui/assets/images/terminal-icon.raw \
	usr/share/cursor.raw:gui/assets/images/cursor.raw
MKFS_INSTALL_DEPS += gui/assets/fonts/DejaVuSans.ttf gui/assets/fonts/DejaVuSans-Bold.ttf \
	gui/assets/fonts/DejaVuSansMono.ttf gui/assets/fonts/DejaVuSansMono-Bold.ttf \
	gui/assets/images/wallpaper.raw gui/assets/images/terminal-icon.raw \
	gui/assets/images/cursor.raw

$(BUILD)/fs.img: $(BUILD)/mkfs $(UPROGS) $(MKFS_INSTALL_DEPS)
	./$(BUILD)/mkfs $(BUILD)/fs.img $(UPROGS) $(MKFS_INSTALL)

-include $(OBJDIR)/kernel/*.d $(OBJDIR)/user/*.d $(OBJDIR)/bash-pic/*.d $(OBJDIR)/bash-pic/poc/*.d $(OBJDIR)/bash-pic/poc/builtins/*.d $(OBJDIR)/bash-pic/builtins/*.d $(OBJDIR)/bash-pic/lib/sh/*.d $(OBJDIR)/bash-pic/lib/glob/*.d $(OBJDIR)/bash-pic/lib/tilde/*.d $(OBJDIR)/curses-pic/*.d $(OBJDIR)/nano-pic/*.d $(OBJDIR)/nano-pic/poc/*.d $(OBJDIR)/nano-pic/src/*.d $(OBJDIR)/nano-pic/lib/*.d $(OBJDIR)/nano-pic/lib/malloc/*.d

all: $(BUILD)/$(IMAGE_NAME).hdd

# dist/poc-os.hdd is the one to dd to a USB stick for a real,
# legacy-BIOS-bootable pendrive; dist/poc-os.iso is for burning to
# optical media or attaching as a virtual/emulated CD/DVD drive (QEMU
# -cdrom, VirtualBox) - though since `limine bios-install` makes it
# hybrid too (see its own build rule's comment), either file actually
# works either way.
DISTDIR = dist
dist: $(BUILD)/$(IMAGE_NAME).hdd $(BUILD)/$(IMAGE_NAME).iso
	mkdir -p $(DISTDIR)
	cp $(BUILD)/$(IMAGE_NAME).hdd $(DISTDIR)/$(IMAGE_NAME).hdd
	cp $(BUILD)/$(IMAGE_NAME).iso $(DISTDIR)/$(IMAGE_NAME).iso

run: all
	$(QEMU) $(QEMUOPTS_BIOS) </dev/null >/dev/null &
	@echo "poc-os launched in its own QEMU window (serial/monitor: Ctrl-Alt-2/3 inside it)."

clean:
	rm -rf $(BUILD)
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg .gdbinit

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 2
endif

# $(IMAGE_NAME).hdd is one combined disk image - fs.img is already
# embedded in it as a Limine module (see its own build rule's comment),
# so this needs only one -drive. No -bios OVMF.fd/UEFI anywhere here:
# QEMU's default machine type boots via SeaBIOS (legacy BIOS/CSM),
# matching this image's own BIOS-only Limine install (limine bios-install,
# not a UEFI one) - see limine.conf's own comment for the BIOS-vs-UEFI
# scope decision.
QEMUOPTS_BIOS = -drive file=$(BUILD)/$(IMAGE_NAME).hdd,index=0,media=disk,format=raw -smp $(CPUS) -m 4G $(QEMUEXTRA)

# `run` launches QEMU detached from this shell's stdio (</dev/null so
# it can't be suspended by SIGTTIN when backgrounded, output silenced)
# so it opens its own GUI window and the terminal is free again
# immediately, instead of blocking until QEMU exits the way `qemu`
# below does. Serial console and monitor fall back to virtual-console
# tabs inside that window (Ctrl-Alt-2/3).
qemu: $(BUILD)/$(IMAGE_NAME).hdd
	$(QEMU) -serial mon:stdio $(QEMUOPTS_BIOS)

qemu-nox: $(BUILD)/$(IMAGE_NAME).hdd
	$(QEMU) -nographic $(QEMUOPTS_BIOS)

.gdbinit: .gdbinit.tmpl
	sed "s/localhost:1234/localhost:$(GDBPORT)/" < $^ > $@

qemu-gdb: $(BUILD)/$(IMAGE_NAME).hdd .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -serial mon:stdio $(QEMUOPTS_BIOS) -S $(QEMUGDB)

qemu-nox-gdb: $(BUILD)/$(IMAGE_NAME).hdd .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -nographic $(QEMUOPTS_BIOS) -S $(QEMUGDB)

.PHONY: all dist run clean tags
