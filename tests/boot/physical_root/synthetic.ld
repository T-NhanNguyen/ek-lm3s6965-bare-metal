/* Synthetic unit closure, deliberately NOT bootloader layout or fit. */
ENTRY(fixture_entry)
MEMORY { FLASH (rx) : ORIGIN = 0, LENGTH = 1M
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }
SECTIONS {
 .text : { *(.text*) *(.rodata*) } > FLASH
 .ARM.extab : { *(.ARM.extab*) } > FLASH
 .ARM.exidx : { *(.ARM.exidx*) } > FLASH
 .data : { *(.data*) } > RAM AT > FLASH
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
 . = ALIGN(8); end = .; _end = .;
}
