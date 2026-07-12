/* Copyright (C) 2014-2018 Free Software Foundation, Inc.
   Copyright (C) 2019 ETH Zürich and University of Bologna
   Copying and distribution of this script, with or without modification,
   are permitted in any medium without royalty provided the copyright
   notice and this notice are preserved.

   Modified by Kalyan Sriram for Clotho, 2026: adapted from
   openhwgroup/cv32e40p@6033d2b1be32 example_tb/core/custom/link.ld (itself
   derived from the RISC-V GNU ld default script) for the picorv32 memory map. */

/* picorv32 bare-metal linker script for CoreMark.
   The testbench RAM is 128 KiB at address 0 (memory[0:128*1024/4-1]); picorv32
   resets PC to 0, so .text.start (crt0 _start) must land at 0. Reserve the top
   32 KiB for the stack (sp = 0x20000), matching the stock picorv32 firmware. */

OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
	ram (rwxai) : ORIGIN = 0x00000000, LENGTH = 0x00018000  /* 96 KiB, 32 KiB stack above */
}

SECTIONS {
	.text : {
		KEEP(*(.text.start))   /* crt0 _start pinned to address 0 */
		*(.text .text.* .gnu.linkonce.t.*)
	} > ram

	.rodata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		*(.srodata .srodata.*)
	} > ram

	. = ALIGN(4);
	.data : {
		__DATA_BEGIN__ = .;
		*(.data .data.* .gnu.linkonce.d.*)
	} > ram

	.sdata : {
		__SDATA_BEGIN__ = .;
		*(.sdata .sdata.* .gnu.linkonce.s.*)
	} > ram

	. = ALIGN(4);
	__bss_start = .;
	.sbss : {
		*(.sbss .sbss.* .gnu.linkonce.sb.*)
		*(.scommon)
	} > ram
	.bss : {
		*(.bss .bss.* .gnu.linkonce.b.*)
		*(COMMON)
	} > ram
	. = ALIGN(4);
	__bss_end = .;

	/* gp points into the small-data window so gp-relative loads reach
	   .sdata/.sbss with a single 12-bit signed offset. */
	__global_pointer$ = MIN(__SDATA_BEGIN__ + 0x800,
	                        MAX(__DATA_BEGIN__ + 0x800, __bss_end - 0x800));

	_end = .;
	PROVIDE(end = .);

	/DISCARD/ : { *(.eh_frame*) *(.riscv.attributes) *(.comment) *(.note.*) }
}
