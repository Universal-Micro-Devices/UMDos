; ============================================================
; start.asm - 64 位内核入口（由 NextMain 跳入）
;   RDI = bootinfo 指针
;   1. 清 .bss  2. 设栈  3. 调 kmain
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
    mov r15, rdi                    ; 保存 bootinfo

    ; 打开 SSE（OSFXSR | OSXMMEXCPT）：编译器可能用 movups/movaps 拷贝结构体
    mov rax, cr4
    or rax, 0x600
    mov cr4, rax

    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb                       ; 64 位下 rep stosb 用 RDI，ES 忽略

    mov rsp, 0x90000
    mov rdi, r15
    call kmain

.halt:
    cli
    hlt
    jmp .halt
