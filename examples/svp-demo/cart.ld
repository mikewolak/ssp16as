/* cart.ld -- 68000 link map of the SVP demonstration cartridge: vectors and header at 0, the SSP's program memory at
 * 0x800-0x1FFFF left free, 68000 code and constants from 0x20000, RAM from 0xFF0000. */
MEMORY { rom (rx) : ORIGIN = 0, LENGTH = 0x40000  ram (rw) : ORIGIN = 0xFF0000, LENGTH = 0xFE00 }
SECTIONS
{
    .boot 0 : { KEEP(*(.text.boot)) } > rom
    .text 0x20000 : { *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(2); } > rom
    _sdata_rom = LOADADDR(.data);
    .data : { _sdata = .; *(.data .data.*) . = ALIGN(2); _edata = .; } > ram AT > rom
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) . = ALIGN(2); _ebss = .; } > ram
    /DISCARD/ : { *(.comment) *(.note*) }
}
