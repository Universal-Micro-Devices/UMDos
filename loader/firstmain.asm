; ============================================================
; firstmain.asm - 第五级 (LBA 30..45, 加载到 0xC000, 16 扇区)
;   1. VBE：优先 1280x720x32，退而求其次任意 32bpp 线性帧缓冲
;   2. 读 MBR / 分区表，加载内核(0x10000) 与文件系统镜像(0x30000)
;   3. 建 4GB 恒等映射页表（4 个 PD，2MB 大页），进 64 位长模式
;   4. 跳 NextMain(0xE000)，由它把 bootinfo 交给 C 内核
; ============================================================
[BITS 16]
[ORG 0xC000]

VBE_INFO   equ 0x7000               ; 同时充当 bootinfo 的起点
BOOTINFO   equ 0x7000
PML4_BASE  equ 0x1000
PDPT_BASE  equ 0x2000
PD_BASE    equ 0x3000               ; 0x3000..0x6FFF 共 4 个 PD
NEXT_ADDR  equ 0xE000
KERNEL_LBA     equ 50
KERNEL_SECTORS equ 128
FS_LBA_DEFAULT equ 512
FS_SECTORS     equ 256

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xC000
    sti
    cld
    mov [drive_num], dl
    call disk_init
    call vbe_init
    call load_mbr                   ; 读 MBR + 分区表 -> bootinfo
    call load_kernel                ; 内核 -> 0x10000
    call load_fs                    ; 文件系统镜像 -> 0x30000
    call fill_bootinfo
    call load_next
    call enter_long
.hang:
    cli
    hlt
    jmp .hang

; ================= 读取 MBR / 分区表 =================
load_mbr:
    mov ax, 0
    mov cx, 1
    xor bx, bx
    mov dx, 0x0720                  ; 0x7200
    call read_sectors_seg
    jc .done
    ; 签名检查
    cmp word [0x73FE], 0xAA55
    jne .done
    ; 解析 4 个分区项（偏移 446，每项 16 字节）
    mov si, 0x73BE
    mov bx, BOOTINFO+56             ; part_lba[]
    mov di, BOOTINFO+72             ; part_size[]
    mov cx, 4
    xor dx, dx                      ; 计数器
.loop:
    cmp byte [si+4], 0              ; 分区类型
    je .next
    mov eax, [si+8]                 ; 起始 LBA
    mov [bx], eax
    mov eax, [si+12]                ; 扇区数
    mov [di], eax
    inc dx
.next:
    add si, 16
    add bx, 4
    add di, 4
    loop .loop
    mov [BOOTINFO+52], dx           ; 分区数
.done:
    ret

; ================= 加载内核 =================
load_kernel:
    mov ax, KERNEL_LBA
    mov cx, KERNEL_SECTORS
    xor bx, bx
    mov dx, 0x1000                  ; 0x10000
    call read_sectors_seg
    jc disk_error
    ret

; ================= 加载文件系统镜像（128KB，分两段） =================
load_fs:
    mov ax, [BOOTINFO+56]           ; 分区 0 的起始 LBA
    cmp word [BOOTINFO+52], 0
    jne .have
    mov ax, FS_LBA_DEFAULT          ; 没有分区表（软盘）：固定 LBA
.have:
    mov [BOOTINFO+44], ax           ; fs_lba
    mov word [BOOTINFO+46], 0
    mov word [BOOTINFO+48], FS_SECTORS

    mov cx, 128
    xor bx, bx
    mov dx, 0x3000
    call read_sectors_seg
    jc disk_error

    mov ax, [BOOTINFO+44]
    add ax, 128
    mov cx, 128
    xor bx, bx
    mov dx, 0x4000
    call read_sectors_seg
    jc disk_error
    ret

; ================= 填写 bootinfo 扩展字段 =================
fill_bootinfo:
    mov dword [BOOTINFO+32], 0x31444D55     ; "UMD1"
    mov dword [BOOTINFO+36], KERNEL_LBA
    mov dword [BOOTINFO+40], KERNEL_SECTORS
    mov dword [BOOTINFO+88], 0x0700         ; 凭据块
    mov dword [BOOTINFO+92], 60             ; 启动画面停留 60 秒
    mov dword [BOOTINFO+96], 0
    ret

; ================= VBE =================
vbe_init:
    pusha
    mov dword [vbe_block], 'VBE2'
    mov ax, 0x4F00
    mov di, vbe_block
    int 0x10
    cmp ax, 0x004F
    jne .fail

    les si, [vbe_block+0x0E]
    mov word [best_mode], 0xFFFF
.scan:
    mov ax, [es:si]
    cmp ax, 0xFFFF
    je .scan_end
    mov [cur_mode], ax
    push es
    push si
    mov cx, ax
    mov ax, 0x4F01
    mov di, mode_block
    int 0x10
    pop si
    pop es
    cmp ax, 0x004F
    jne .next
    mov dx, [mode_block]
    and dx, 0x0090                  ; 图形 + 线性帧缓冲，两者都要
    cmp dx, 0x0090
    jne .next
    cmp byte [mode_block+0x19], 32
    jne .next
    cmp word [mode_block+0x12], 1280
    jne .fallback
    cmp word [mode_block+0x14], 720
    jne .fallback
    jmp .take
.fallback:
    cmp word [best_mode], 0xFFFF
    jne .next
    mov ax, [cur_mode]
    mov [best_mode], ax
.next:
    add si, 2
    jmp .scan
.scan_end:
    cmp word [best_mode], 0xFFFF
    je .fail
    mov cx, [best_mode]
    mov [cur_mode], cx
    mov ax, 0x4F01
    mov di, mode_block
    int 0x10
    cmp ax, 0x004F
    jne .fail
.take:
    mov bx, [cur_mode]
    or bx, 0x4000                   ; 线性帧缓冲
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .fail

    ; 保存帧缓冲信息到 0x7000
    mov eax, [mode_block+0x28]      ; PhysBasePtr
    mov [VBE_INFO], eax
    mov dword [VBE_INFO+4], 0
    movzx eax, word [mode_block+0x10]
    movzx ebx, word [mode_block+0x14]
    mul ebx
    mov [VBE_INFO+8], eax
    mov dword [VBE_INFO+12], 0
    movzx eax, word [mode_block+0x12]
    mov [VBE_INFO+16], eax
    movzx eax, word [mode_block+0x14]
    mov [VBE_INFO+20], eax
    movzx eax, word [mode_block+0x10]
    shr eax, 2
    mov [VBE_INFO+24], eax          ; 每扫描线像素数
    movzx eax, byte [mode_block+0x19]
    mov [VBE_INFO+28], eax
    popa
    ret
.fail:
    mov si, m_vbe
    call puts
    cli
.h:
    hlt
    jmp .h

; ================= 读 NextMain =================
load_next:
    mov ax, 46
    mov cx, 4
    mov bx, NEXT_ADDR
    call read_sectors
    jc disk_error
    ret

; ================= 页表 =================
setup_paging:
    cld
    mov ax, ds                      ; rep stosd 走 ES:DI，前面的 BIOS 调用可能弄脏 ES
    mov es, ax
    mov edi, PML4_BASE
    mov ecx, (6*4096)/4             ; 清 0x1000..0x6FFF
    xor eax, eax
    rep stosd

    mov eax, PDPT_BASE | 3
    mov [PML4_BASE], eax

    mov eax, PD_BASE+0x0000 | 3
    mov [PDPT_BASE+0], eax
    mov eax, PD_BASE+0x1000 | 3
    mov [PDPT_BASE+8], eax
    mov eax, PD_BASE+0x2000 | 3
    mov [PDPT_BASE+16], eax
    mov eax, PD_BASE+0x3000 | 3
    mov [PDPT_BASE+24], eax

    mov edi, PD_BASE
    xor ebx, ebx
    mov ecx, 2048                   ; 4 PD * 512 = 4GB
.fill:
    mov eax, ebx
    or eax, 0x83                    ; P | RW | PS
    mov [edi], eax
    add edi, 8
    add ebx, 0x200000
    loop .fill
    ret

; ================= 进入长模式 =================
enter_long:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_lm
    mov eax, 0x80000001
    cpuid
    test edx, 0x20000000            ; LM
    jz .no_lm

    call setup_paging
    lgdt [gdtr]
    cli
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
    or eax, 0x80000001              ; PG | PE —— 缺一不可
    mov cr0, eax
    jmp 0x08:long64
.no_lm:
    mov si, m_lm
    call puts
    cli
.h:
    hlt
    jmp .h

; ================= 64 位 =================
[BITS 64]
long64:
    ; 长模式下 DS/ES/SS 的基址恒为 0、界限不检查，不必重新加载
    mov rsp, 0x90000
    mov rdi, BOOTINFO
    jmp 0xE000                      ; -> NextMain 跳板 -> C 内核

; ================= 数据 =================
gdtr:
    dw gdt_end - gdt - 1
    dd gdt
gdt:
    dq 0
    ; 64 位代码段: L=1
    dw 0xFFFF, 0
    db 0
    db 0x9A
    db 0xAF
    db 0
    ; 64 位数据段
    dw 0xFFFF, 0
    db 0
    db 0x92
    db 0xCF
    db 0
gdt_end:

drive_num  db 0
best_mode  dw 0xFFFF
cur_mode   dw 0
vbe_block  times 512 db 0
mode_block times 256 db 0

m_vbe db 'VBE: no suitable linear framebuffer mode!', 0x0D, 0x0A, 0
m_lm  db 'CPU does not support long mode!', 0x0D, 0x0A, 0

; 公共库放在入口代码之后，保证 start 位于映像偏移 0
[BITS 16]
%include "io16.inc"
%include "disk16.inc"

times (16 * 512) - ($ - $$) db 0
