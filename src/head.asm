; ============================================================
; head.asm - 第三级（LBA 9..12, ORG 0x9000, 4 扇区 = 2KB）
;
; P2 之后的职责：由 loader 以 32 位保护模式进入，这里只做
;   1. 建 4GB 恒等映射页表（4 级页表 + 2MB 大页）
;   2. 打开 PAE / LME / PG，进入 64 位长模式
;   3. 跳到内核入口 0x10000（RDI = bootinfo）
;
; 旧的 firstmain/nextmain 保留给 dev.1 安装引导使用，
; 旧 head/hello 保留在 src/legacy/ 供审计对照。
; ============================================================
[BITS 32]
[ORG 0x9000]

PML4_BASE  equ 0x1000
PDPT_BASE  equ 0x2000
PD_BASE    equ 0x3000               ; 0x3000..0x6FFF 共 4 个 PD
BOOTINFO   equ 0x7000

start:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    mov esi, m_head
    call puts32

    ; 检查长模式支持
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb no_lm
    mov eax, 0x80000001
    cpuid
    test edx, 0x20000000
    jz no_lm

    call setup_paging

    mov eax, cr4
    or eax, 0x20                    ; PAE
    mov cr4, eax
    mov eax, PML4_BASE
    mov cr3, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, 0x100                   ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000001              ; PG | PE
    mov cr0, eax
    jmp 0x10:long64

no_lm:
    mov esi, m_no_lm
    call puts32
.h:
    cli
    hlt
    jmp .h

; ---------------- 页表 ----------------
setup_paging:
    cld
    mov ax, ds
    mov es, ax
    mov edi, PML4_BASE
    mov ecx, (6 * 4096) / 4         ; 清 0x1000..0x6FFF
    xor eax, eax
    rep stosd

    mov eax, PDPT_BASE + 3
    mov [PML4_BASE], eax

    mov eax, PD_BASE + 0x0000 + 3
    mov [PDPT_BASE + 0], eax
    mov eax, PD_BASE + 0x1000 + 3
    mov [PDPT_BASE + 8], eax
    mov eax, PD_BASE + 0x2000 + 3
    mov [PDPT_BASE + 16], eax
    mov eax, PD_BASE + 0x3000 + 3
    mov [PDPT_BASE + 24], eax

    mov edi, PD_BASE
    xor ebx, ebx
    mov ecx, 2048                   ; 4 PD * 512 项 = 4GB
.fill:
    mov eax, ebx
    or eax, 0x83                    ; P | RW | PS
    mov [edi], eax
    add edi, 8
    add ebx, 0x200000
    loop .fill
    ret

; ---------------- 32 位串口输出 ----------------
putc32:
    push eax
    push edx
    mov ah, al
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x3F8
    mov al, ah
    out dx, al
    pop edx
    pop eax
    ret

puts32:
    push eax
    push esi
.next:
    lodsb
    test al, al
    jz .done
    call putc32
    jmp .next
.done:
    pop esi
    pop eax
    ret

m_head   db '[head] ', 0
m_no_lm  db 'CPU does not support long mode', 0x0D, 0x0A, 0

; ================= 64 位 =================
[BITS 64]
long64:
    ; 长模式下 DS/ES/SS 的基址恒为 0，沿用 32 位阶段的选择子即可，不重新加载
    mov rsp, 0x90000

    mov rsi, m_lm
    call puts64

    mov rdi, BOOTINFO
    jmp 0x10000

puts64:
    mov rbx, rsi
.next:
    mov al, [rbx]
    test al, al
    jz .done
.wait:
    push rax
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    pop rax
    jz .wait
    mov dx, 0x3F8
    out dx, al
    inc rbx
    jmp .next
.done:
    ret

m_lm db 'long mode ok, jump kernel', 0x0D, 0x0A, 0

times (4 * 512) - ($ - $$) db 0
