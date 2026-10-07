/* FTP-only contract: 64 KiB SRAM, 2 KiB C heap, 8 KiB reserved stack.
 * lwIP uses its own static heap and pools, not _sbrk. */
ENTRY(Reset_Handler)
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
  SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
_estack = ORIGIN(SRAM) + LENGTH(SRAM);
_stack_limit = _estack - 8K;
SECTIONS
{
  .text :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    *(.text .text*)
    *(.rodata .rodata*)
    . = ALIGN(4);
  } > FLASH
  .ARM.extab : { *(.ARM.extab*) } > FLASH
  .ARM.exidx :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH
  .preinit_array :
  {
    . = ALIGN(4);
    __preinit_array_start = .;
    KEEP(*(.preinit_array))
    __preinit_array_end = .;
  } > FLASH
  .init_array :
  {
    . = ALIGN(4);
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } > FLASH
  .fini_array :
  {
    . = ALIGN(4);
    __fini_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    __fini_array_end = .;
  } > FLASH
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data .data*)
    . = ALIGN(4);
    _edata = .;
  } > SRAM AT> FLASH
  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss .bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > SRAM
  .c_heap (NOLOAD) :
  {
    . = ALIGN(8);
    _end = .;
    _heap_start = .;
    . += 2K;
    _heap_end = .;
  } > SRAM
  .stack _stack_limit (NOLOAD) :
  {
    . += 8K;
  } > SRAM
  ASSERT(_heap_end <= _stack_limit, "FTP SRAM exceeds heap/8 KiB stack boundary")
  /DISCARD/ : { *(.comment) *(.note*) }
}
