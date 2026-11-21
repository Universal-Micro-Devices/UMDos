/* link.ld - 用户程序链接到 USER_BASE（内核页表里的用户窗口） */
ENTRY(_start)
SECTIONS
{
    . = 0x0000008000000000;
    .text : { *(.text._start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss :
    {
        *(.bss*) *(COMMON)
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.rela*) *(.rel.*) }
}
