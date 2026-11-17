; ============================================================
; switch.asm - 上下文切换 + ring3 进入/返回（P5/P6）
; ============================================================
[BITS 64]

global switch_ctx
global user_enter
global user_leave_to_kernel
global user_kernel_rsp
global user_exit_code

section .bss
user_kernel_rsp: resq 1
user_exit_code:  resd 1

section .data
g_user_stack_save: dq 0

section .text

; void switch_ctx(u64 *old_rsp_save, u64 new_rsp)
switch_ctx:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; void user_enter(u64 entry, u64 ustack) —— iretq 到 ring3，不返回
user_enter:
    mov [rel user_kernel_rsp], rsp
    mov [rel g_user_stack_save], rsp
    mov ax, 0x23                 ; 用户数据段 | RPL3
    mov ds, ax
    mov es, ax
    push qword 0x23              ; SS
    push rsi                     ; RSP
    push qword 0x202             ; RFLAGS: IF=1
    push qword 0x1B              ; CS = 0x18 | RPL3
    push rdi                     ; RIP
    iretq

; void user_leave_to_kernel(int code) —— 从 exit 系统调用返回 user_run
user_leave_to_kernel:
    mov [rel user_exit_code], edi
    mov ax, 0x10                 ; 恢复内核数据段
    mov ds, ax
    mov es, ax
    mov rsp, [rel user_kernel_rsp]
    ret
