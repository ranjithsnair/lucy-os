/* Linker script for the 64-bit poc kernel.
   See the GNU ld 'info' manual ("info ld") to learn the syntax. */

OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(entry)

/* Limine reads the requests Limine bootloader itself needs (base
   revision tag, kernel/limine.c's framebuffer/module/HHDM requests) out
   of the .limine_requests section below via this program header, before
   ever handing control to `entry` - see that section's own comment. */
PHDRS
{
	limine_requests PT_LOAD;
	text PT_LOAD;
	rodata PT_LOAD;
	data PT_LOAD;
}

SECTIONS
{
	/* Link the kernel at this address: "." means the current address */
	/* Must be equal to KERNLINK (KERNBASE+EXTMEM, see memlayout.h) */
	. = 0xFFFFFFFF80100000;

	/* Limine protocol boilerplate (see kernel/limine.c): the requests
	   Limine answers before jumping to `entry`, bracketed by start/end
	   marker sections so Limine can find them regardless of where the
	   linker places this relative to .text - required as of base
	   revision 2 (kernel/limine.c uses base revision 3). Physically
	   placed right after the kernel's own load address, same AT()
	   convention as .text below - Limine loads this ELF's PT_LOAD
	   segments exactly like any other non-relocatable kernel. */
	.limine_requests : AT(0x100000) {
		KEEP(*(.limine_requests_start))
		KEEP(*(.limine_requests))
		KEEP(*(.limine_requests_end))
	} :limine_requests

	. = ALIGN(0x1000);

	/* No explicit AT() here: ld continues the same VMA-LMA delta (i.e.
	   physical = virtual - KERNBASE) .limine_requests' own AT(0x100000)
	   established, same as this file always relied on for everything
	   after .text (.rodata/.data/.bss below never had their own AT()
	   either). */
	.text : {
		*(.text .stub .text.* .gnu.linkonce.t.*)
	} :text

	PROVIDE(etext = .);	/* Define the 'etext' symbol to this value */

	/* Own PT_LOAD segment (:rodata, R only vs .text's R E), so needs its
	   own page-boundary start too - Limine's ELF loader rejects two
	   PT_LOADs with different permissions sharing a page (found the hard
	   way: this was the one page-alignment point the original raw-binary-
	   flattening BIOS boot loader never needed, since it never parsed
	   ELF program headers at all - see git history's boot/boot2_bios.asm
	   for that old approach). */
	. = ALIGN(0x1000);

	.rodata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
	} :rodata

	/* Include debugging information in kernel memory */
	.stab : {
		PROVIDE(__STAB_BEGIN__ = .);
		*(.stab);
		PROVIDE(__STAB_END__ = .);
	}

	.stabstr : {
		PROVIDE(__STABSTR_BEGIN__ = .);
		*(.stabstr);
		PROVIDE(__STABSTR_END__ = .);
	}

	/* Adjust the address for the data segment to the next page */
	. = ALIGN(0x1000);

	/* Conventionally, Unix linkers provide pseudo-symbols
	 * etext, edata, and end, at the end of the text, data, and bss.
	 * For the kernel mapping, we need the address at the beginning
	 * of the data section, but that's not one of the conventional
	 * symbols, because the convention started before there was a
	 * read-only rodata section between text and data. */
	PROVIDE(data = .);

	/* The data segment */
	.data : {
		*(.data)
	} :data

	PROVIDE(edata = .);

	.bss : {
		*(.bss)
	} :data

	PROVIDE(end = .);

	/DISCARD/ : {
		*(.eh_frame .note.GNU-stack)
	}
}
