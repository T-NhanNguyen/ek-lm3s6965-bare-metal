/* Linker script for the TI/Luminary LM3S6965.
 * Flash 256 KB @ 0x00000000, SRAM 64 KB @ 0x20000000. */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
  .text :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  .preinit_array :
  {
    . = ALIGN(4);
    __preinit_array_start = .;
    KEEP(*(.preinit_array))
    . = ALIGN(4);
    __preinit_array_end = .;
  } > FLASH

  .init_array :
  {
    . = ALIGN(4);
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    . = ALIGN(4);
    __init_array_end = .;
  } > FLASH

  .fini_array :
  {
    . = ALIGN(4);
    __fini_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    . = ALIGN(4);
    __fini_array_end = .;
  } > FLASH

  /* Load address of .data, used by Reset_Handler to copy it into SRAM.
   * `AT> FLASH` lets the linker place the LMA automatically after the
   * preceding flash content, so sections can never overlap. */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > SRAM AT> FLASH

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > SRAM

  . = ALIGN(4);
  _end = .;

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
  }
}
