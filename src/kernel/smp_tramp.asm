; ============================================================
; smp_tramp.asm - AP 启动跳板（P15），装入 0x8000 执行
;   BSP 通过 LAPIC INIT-SIPI-SIPI 让 AP 从 0x8000 开始
;   信息块固定在 0x8F00：
;     +0  cpu_index (u32)
;     +4  ap_entry  (u64)
;     +12 stack_top (u64)
;     +20 pml4      (u64)
; ============================================================
[BITS 16]
org 0x8000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm32

[BITS 32]
pm32:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [0x8F14]          ; pml4 低 32 位
    mov cr3, eax
    mov eax, cr4
    or eax, 0x20               ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100              ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000001         ; PG | PE
    mov cr0, eax
    jmp 0x10:lm64

[BITS 64]
lm64:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [0x8F0C]          ; stack_top
    mov rdi, [0x8F00]          ; cpu_index（u64 低 32 位）
    mov eax, [0x8F04]
    mov rdx, [0x8F08]          ; ap_entry
    mov rax, rdx
    jmp rax
.hang:
    cli
    hlt
    jmp .hang

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; 0x08: 32 位代码
    dq 0x00AF9A000000FFFF       ; 0x10: 64 位代码
    dq 0x00CF92000000FFFF       ; 0x18: 数据
gdtr:
    dw 0x20
    dd 0x8000 + (gdt - start)
