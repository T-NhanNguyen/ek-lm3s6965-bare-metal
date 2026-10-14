/* Linker script for the TI/Luminary LM3S6965.
 * Flash 256 KB @ 0x00000000, SRAM 64 KB @ 0x20000000. */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0xDFC0
  DIAGNOSTIC (rw) : ORIGIN = 0x2000DFC0, LENGTH = 64
  STACK (rw) : ORIGIN = 0x2000E000, LENGTH = 8K
}

_estack = ORIGIN(STACK) + LENGTH(STACK);
_stack_limit = ORIGIN(STACK);
_diagnostic_start = ORIGIN(DIAGNOSTIC);
_diagnostic_end = ORIGIN(DIAGNOSTIC) + LENGTH(DIAGNOSTIC);
_flash_payload_start = ORIGIN(FLASH);
_flash_payload_limit = ORIGIN(FLASH) + LENGTH(FLASH);

SECTIONS
{
  .isr_vector ORIGIN(FLASH) : ALIGN(256)
  {
    _vectors_start = .;
    KEEP(*(.isr_vector))
    _vectors_end = .;
  } > FLASH
  .text :
  {
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
  } > FLASH

  .ARM.extab : { *(.ARM.extab*) } > FLASH
  .ARM.exidx :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH
  .ctors : { KEEP(*(.ctors .ctors.*)) } > FLASH
  .dtors : { KEEP(*(.dtors .dtors.*)) } > FLASH
  .preinit_array :
  {
    . = ALIGN(4);
    __preinit_array_start = .;
    KEEP(*(.preinit_array .preinit_array.*))
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

  _flash_payload_end = LOADADDR(.data) + SIZEOF(.data);

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

  .c_heap (NOLOAD) :
  {
    . = ALIGN(8);
    _end = .;
    _heap_start = .;
    *(.heap .heap.*)
    . = ALIGN(8);
    _heap_static_end = .;
  } > SRAM
  /* Preserve dynamic capacity in remaining lower SRAM; allocator must bind
   * to these bounds in a separate increment. */
  _heap_end = _diagnostic_start;
  .boot_diagnostic ORIGIN(DIAGNOSTIC) (NOLOAD) : { . += 64; } > DIAGNOSTIC
  .stack ORIGIN(STACK) (NOLOAD) : { . += 8K; } > STACK
  ASSERT(_vectors_start == ORIGIN(FLASH) && (_vectors_start & 255) == 0,
         "vectors must start at the 256-byte-aligned payload origin")
  ASSERT(SIZEOF(.isr_vector) >= 236 && (SIZEOF(.isr_vector) & 3) == 0,
         "vectors must contain at least 59 words")
  ASSERT(_estack == 0x20010000 && _stack_limit == 0x2000E000,
         "exact MSP/8 KiB stack contract")
  ASSERT(_edata <= _diagnostic_start && _ebss <= _diagnostic_start &&
         _heap_start <= _heap_static_end && _heap_static_end <= _heap_end && _heap_end <= _diagnostic_start,
         "ordinary SRAM overlaps diagnostic reservation")
  ASSERT(_diagnostic_end == _stack_limit && SIZEOF(.boot_diagnostic) == 64,
         "diagnostic reservation contract")
  ASSERT(_flash_payload_end <= _flash_payload_limit, "flash data LMA exceeds partition")

  /* Reject retained unowned allocated inputs after --gc-sections. Startup only
   * copies .data and clears .bss; never silently discard allocated notes. */
  .unsupported_allocated :
  {
    INPUT_SECTION_FLAGS (SHF_ALLOC) *(*)
  } > FLASH
  ASSERT(SIZEOF(.unsupported_allocated) == 0, "unsupported allocated input section")
  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
  }
}
