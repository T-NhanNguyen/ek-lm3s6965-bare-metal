ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0, LENGTH = 256K
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS {
 .vectors 0 : { KEEP(*(.isr_vector)) } > FLASH
 .text : { *(.text*) *(.rodata*) } > FLASH
 .slot_a 0x8800 : { KEEP(*(.slot_a)) } > FLASH
 .slot_b 0x24000 : { KEEP(*(.slot_b)) } > FLASH
 .bss (NOLOAD) : {
  _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .;
 } > RAM
 ASSERT(_ebss < 0x2000b000, "fixture overlaps old stacks")
 /DISCARD/ : { *(.comment) *(.ARM.attributes) }
}
