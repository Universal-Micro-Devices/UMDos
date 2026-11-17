; ============================================================
; installer/src/start.asm - 安装程序的 64 位入口
;   RDI = bootinfo (0x7000)，由 NextMain 传入
; ============================================================
[BITS 64]

global _start
extern kmain
extern __bss_start
extern __bss_end

section .text._start
_start:
    cli
    cld
    mov r15, rdi
    mov rax, cr4
    or rax, 0x600
    mov cr4, rax

    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb

    mov rsp, 0x90000
    mov rdi, r15
    call kmain

.halt:
    cli
    hlt
    jmp .halt
