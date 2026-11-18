; ============================================================
; isr.asm - 64 位中断/异常入口（P3）
;   0–31 异常，32–47 硬件 IRQ（PIC 重映射到 0x20..0x2F）
;   统一入口：压入向量号和错误码 -> 保存寄存器 -> 调 isr_dispatch
; ============================================================
[BITS 64]

global isr_stub_table
extern isr_dispatch

section .text

%macro ISR_NOERR 1
isr_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_%1:
    push qword %1
    jmp isr_common
%endmacro

; ---- 异常 0..31（带错误码的按 Intel 手册） ----
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- IRQ 32..47 ----
%assign v 32
%rep 16
ISR_NOERR v
%assign v v+1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; 栈布局（从低到高）：rax..r15 (120B), vector, error, rip, cs, rflags
    mov rdi, [rsp + 120]
    mov rsi, [rsp + 128]
    mov rdx, [rsp + 136]
    mov rcx, [rsp + 144]
    mov r8,  [rsp + 152]
    mov r9, rsp                  ; 保存的通用寄存器区（syscall 用）
    mov rbp, rsp
    and rsp, -16
    xor eax, eax
    call isr_dispatch
    mov rsp, rbp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; int 0x80：系统调用门（DPL=3，在 idt.c 里单独安装）
global isr_0x80
isr_0x80:
    push qword 0
    push qword 128
    jmp isr_common

section .rodata
align 8
isr_stub_table:
%assign i 0
%rep 48
    dq isr_%+i
%assign i i+1
%endrep
