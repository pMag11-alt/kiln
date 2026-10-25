/* User programs: text at 64 KiB (page 0..15 stay unmapped to catch NULL),
 * then data on its own page so text can be mapped read-only + executable
 * and data read-write, never both. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5);   /* R X */
  data PT_LOAD FLAGS(6);   /* R W */
}

SECTIONS
{
  . = 0x10000;
  .text : {
    *(.text.start)
    *(.text .text.*)
  } :text
  .rodata : {
    *(.srodata .srodata.*)
    *(.rodata .rodata.*)
  } :text

  . = ALIGN(4096);
  .data : {
    *(.data .data.*)
    *(.sdata .sdata.*)
  } :data
  .bss : {
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
  } :data

  /DISCARD/ : {
    *(.eh_frame .eh_frame_hdr .comment .note .note.*)
  }
}
