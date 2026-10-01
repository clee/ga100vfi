/* CB 128 KiB flash, 8 KiB persistent USB loader, final 32-byte app footer.
 * Main SRAM reserves the reset cookie at the stack boundary. */
ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0x08002000, LENGTH = 120K - 32
 RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 22K - 8
 CCM (xrw)  : ORIGIN = 0x10000000, LENGTH = 10K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0;
_Min_Stack_Size = 0x1000;
SECTIONS {
 .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH
 .text : { . = ALIGN(4); *(.text*) *(.glue_7) *(.glue_7t) *(.eh_frame) KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4); _etext = .; } >FLASH
 .rodata : { . = ALIGN(4); *(.rodata*) . = ALIGN(4); } >FLASH
 .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
 .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH
 .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
 .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
 .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH
 _sidata = LOADADDR(.data);
 .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } >RAM AT>FLASH
 .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; __bss_end__ = .; } >RAM
 ._user_heap_stack (NOLOAD) : { . = ALIGN(8); PROVIDE(end = .); PROVIDE(_end = .); . += _Min_Heap_Size; . += _Min_Stack_Size; . = ALIGN(8); } >RAM
 /DISCARD/ : { *(.note.GNU-stack) }
 .ARM.attributes 0 : { *(.ARM.attributes) }
}
ASSERT(_estack >= ADDR(._user_heap_stack) + SIZEOF(._user_heap_stack), "CB main SRAM overflow")
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(FLASH) + LENGTH(FLASH), "CB app overlaps footer")
ASSERT(_estack == 0x200057f8, "CB app stack overlaps reset cookie")
