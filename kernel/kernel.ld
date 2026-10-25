/* The kernel is linked at KERNBASE + 0x80200000 and loaded by OpenSBI at
 * physical 0x80200000. It ships as a flat binary, so .text.boot must come
 * first: offset 0 of the image is _start. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFC080200000;

SECTIONS
{
  . = KERNEL_VMA;
  PROVIDE(kernel_start = .);

  .text : {
    *(.text.boot)
    *(.text .text.*)
  }
  . = ALIGN(4096);
  PROVIDE(etext = .);

  .rodata : {
    *(.srodata .srodata.*)
    *(.rodata .rodata.*)
  }
  . = ALIGN(4096);
  PROVIDE(erodata = .);

  .data : {
    *(.data .data.*)
    *(.sdata .sdata.*)
  }

  .bss : ALIGN(16) {
    PROVIDE(__bss_start = .);
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(16);
    PROVIDE(__bss_end = .);
  }
  . = ALIGN(4096);
  PROVIDE(end = .);

  /DISCARD/ : {
    *(.eh_frame .eh_frame_hdr .comment .note .note.*)
  }
}
