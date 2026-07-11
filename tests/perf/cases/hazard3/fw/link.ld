/* Minimal linker script for the Hazard3 perf testbench firmware.
 * Places the reset vector table at MEM_BASE (0x80000000) so _reset lands at
 * the core's 0x80000040 reset vector; everything fits in the 256 KiB modelled
 * by tb_hazard3.sv. Freestanding: no newlib, no crt0. */

OUTPUT_ARCH(riscv)
ENTRY(_reset)

MEMORY {
	RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 256K
}

SECTIONS {
	. = 0x80000000;

	.vectors : { KEEP(*(.vectors)) } > RAM

	.text : {
		*(.text .text.*)
	} > RAM

	.rodata : {
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
	} > RAM

	.data : {
		*(.data .data.*)
		*(.sdata .sdata.*)
	} > RAM

	.bss : {
		__bss_start = .;
		*(.bss .bss.*)
		*(.sbss .sbss.*)
		*(COMMON)
		__bss_end = .;
	} > RAM

	. = ALIGN(16);
	__stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
