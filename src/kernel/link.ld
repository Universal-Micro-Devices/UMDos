/* link.ld - 内核链接脚本：固定装载到 0x10000 */
ENTRY(_start)
SECTIONS
{
    . = 0x10000;
    .text : { *(.text._start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss :
    {
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.rela*) *(.rel.*) }
}
