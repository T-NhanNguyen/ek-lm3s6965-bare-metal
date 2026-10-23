/* LM3S6965 bootloader: self-contained for out-of-tree -T commands.
 * Slot producers must also use -Wl,-z,max-page-size=0x100. */
ENTRY(Reset_Handler)
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x8000
  SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0xDBC0
  CONTINUITY (rw) : ORIGIN = 0x2000DBC0, LENGTH = 1024
  DIAGNOSTIC (rw) : ORIGIN = 0x2000DFC0, LENGTH = 64
  STACK (rw) : ORIGIN = 0x2000E000, LENGTH = 8K
}
_metadata_start = 0x00008000;
_metadata_end = 0x00008800;
_flash_payload_start = ORIGIN(FLASH);
_flash_payload_limit = ORIGIN(FLASH) + LENGTH(FLASH);
_diagnostic_start = ORIGIN(DIAGNOSTIC);
_diagnostic_end = ORIGIN(DIAGNOSTIC) + LENGTH(DIAGNOSTIC);
_stack_limit = ORIGIN(STACK);
_estack = ORIGIN(STACK) + LENGTH(STACK);
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
    *(.text .text*)
    *(.rodata .rodata*)
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
  .preinit_array :
  {
    . = ALIGN(4);
    __preinit_array_start = .;
    KEEP(*(.preinit_array .preinit_array.*))
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
  .ctors : { KEEP(*(.ctors .ctors.*)) } > FLASH
  .dtors : { KEEP(*(.dtors .dtors.*)) } > FLASH
  /* Metadata/manifests are separately programmed, never firmware loads. */
  .forbidden : { *(.manifest .manifest.* .metadata .metadata.*) } > FLASH
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data .data*)
    . = ALIGN(4);
    _edata = .;
  } > SRAM AT> FLASH
  _flash_payload_end = LOADADDR(.data) + SIZEOF(.data);
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
    *(.heap .heap.*)
    . = ALIGN(8);
    _heap_static_end = .;
  } > SRAM
  /* Dynamic capacity is all remaining lower SRAM, not a new 2 KiB policy.
   * Runtime allocator binding to these bounds is a separate increment. */
  _heap_end = _continuity_start;
  _continuity_start = ORIGIN(CONTINUITY);
  _continuity_end = ORIGIN(CONTINUITY) + LENGTH(CONTINUITY);
  boot_startup_continuity = ABSOLUTE(_continuity_start);
  .boot_continuity ORIGIN(CONTINUITY) (NOLOAD) : { . += 1024; } > CONTINUITY
  ASSERT(_continuity_start == 0x2000DBC0 && (_continuity_start & 7) == 0 &&
         _continuity_end == _diagnostic_start && SIZEOF(.boot_continuity) == 1024,
         "continuity reservation contract")
  ASSERT(_edata <= _continuity_start && _ebss <= _continuity_start &&
         _heap_static_end <= _continuity_start, "ordinary SRAM overlaps continuity")
  .boot_diagnostic ORIGIN(DIAGNOSTIC) (NOLOAD) :
  {
    . += 64;
  } > DIAGNOSTIC
  .stack ORIGIN(STACK) (NOLOAD) :
  {
    . += 8K;
  } > STACK
  ASSERT(_vectors_start == ORIGIN(FLASH) && (_vectors_start & 255) == 0,
         "vectors must start at the 256-byte-aligned payload origin")
  ASSERT(SIZEOF(.isr_vector) >= 236 && (SIZEOF(.isr_vector) & 3) == 0,
         "vectors must contain at least 59 words")
  ASSERT(_estack == 0x20010000 && _stack_limit == 0x2000E000,
         "exact MSP/8 KiB stack contract")
  ASSERT(_sdata <= _edata && _edata <= _diagnostic_start && _ebss <= _diagnostic_start,
         "data/bss overlaps diagnostic SRAM")
  ASSERT(_heap_start <= _heap_static_end && _heap_static_end <= _heap_end && _heap_end <= _diagnostic_start,
         "heap overlaps diagnostic SRAM")
  ASSERT(_diagnostic_start == 0x2000DFC0 && _diagnostic_end == _stack_limit && SIZEOF(.boot_diagnostic) == 64,
         "diagnostic reservation contract")
  ASSERT(_flash_payload_start == 0 && _flash_payload_limit == _metadata_start,
         "bootloader/metadata partition contract")
  ASSERT(_flash_payload_end <= _flash_payload_limit,
         "flash payload including data LMA exceeds partition")
  ASSERT(SIZEOF(.forbidden) == 0, "manifest/metadata must not be firmware load bytes")
  /* Reject retained unowned allocated inputs after --gc-sections. Startup only
   * copies .data and clears .bss; never silently discard allocated notes. */
  .unsupported_allocated :
  {
    INPUT_SECTION_FLAGS (SHF_ALLOC) *(*)
  } > FLASH
  ASSERT(SIZEOF(.unsupported_allocated) == 0, "unsupported allocated input section")
  /DISCARD/ : { *(.comment) *(.note*) }
}
