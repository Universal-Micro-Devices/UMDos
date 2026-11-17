; ============================================================
; loader.asm - 第二级（LBA 1..8, ORG 0x7E00, 8 扇区 = 4KB）
;
; P2 之后的职责：
;   1. 打开 A20（快端口 + 自检）
;   2. VBE 选模式，填 bootinfo 的 framebuffer 字段
;   3. 读 MBR 分区表（只读，用于 bootinfo）
;   4. 把内核镜像（LBA 50，192 扇区）读到 0x10000
;      把 UMDFS 镜像（LBA 512，256 扇区）读到 0x40000
;      把第三级 head（LBA 9，4 扇区）读到 0x9000
;   5. 建 GDT，进入 32 位保护模式，跳到 head
;
; 旧实现保留在 src/legacy/loader.asm 供审计对照。
; ============================================================
[BITS 16]
[ORG 0x7E00]

KERNEL_LBA      equ 50
KERNEL_SECTORS  equ 320
FS_LBA          equ 512
FS_SECTORS      equ 256
HEAD_LBA        equ 9
HEAD_SECTORS    equ 4
HEAD_SEG        equ 0x0900          ; 0x9000
BOOTINFO        equ 0x7000
KERNEL_SEG      equ 0x1000          ; 0x10000
FS_SEG          equ 0x4000          ; 0x40000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drive_num], dl

    mov si, m_loader
    call serial_puts

    call disk_init
    call a20_enable
    call vbe_init
    mov si, m_vbe
    call serial_puts

    call load_mbr
    call load_e820
    call load_kernel
    call load_fs
    call load_head
    call fill_bootinfo

    mov si, m_ok
    call serial_puts
    call enter_pm

.hang:
    cli
    hlt
    jmp .hang

; ---------------- 几何参数 ----------------
disk_init:
    pusha
    mov ah, 0x08
    mov dl, [drive_num]
    int 0x13
    jc .def
    and cl, 0x3F
    mov [spt], cl
    inc dh
    mov [heads], dh
    popa
    ret
.def:
    mov byte [spt], 18
    mov byte [heads], 2
    popa
    ret

; ---------------- A20 ----------------
a20_enable:
    pusha
    call a20_test
    jc .done
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    call a20_test
    jc .done
    mov si, m_a20_fail
    call serial_puts
.done:
    popa
    ret

; 返回 CF=1 表示 A20 已开
a20_test:
    push ax
    push bx
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov bx, [0x0500]
    push bx
    mov bx, [0x0510]
    push bx

    mov byte [0x0500], 0x00
    mov byte [0x0510], 0xFF
    mov al, [0x0500]
    cmp al, 0xFF
    je .off

    pop bx
    mov [0x0510], bx
    pop bx
    mov [0x0500], bx
    pop es
    pop ds
    pop bx
    pop ax
    stc
    ret
.off:
    pop bx
    mov [0x0510], bx
    pop bx
    mov [0x0500], bx
    pop es
    pop ds
    pop bx
    pop ax
    clc
    ret

; ---------------- VBE ----------------
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
    and dx, 0x0090
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
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .fail

    mov eax, [mode_block+0x28]
    mov [BOOTINFO], eax
    mov dword [BOOTINFO+4], 0
    movzx eax, word [mode_block+0x10]
    movzx ebx, word [mode_block+0x14]
    mul ebx
    mov [BOOTINFO+8], eax
    mov dword [BOOTINFO+12], 0
    movzx eax, word [mode_block+0x12]
    mov [BOOTINFO+16], eax
    movzx eax, word [mode_block+0x14]
    mov [BOOTINFO+20], eax
    movzx eax, word [mode_block+0x10]
    shr eax, 2
    mov [BOOTINFO+24], eax
    movzx eax, byte [mode_block+0x19]
    mov [BOOTINFO+28], eax
    popa
    ret
.fail:
    mov si, m_vbe_fail
    call serial_puts
    cli
.h:
    hlt
    jmp .h

; ---------------- MBR 分区表（只读） ----------------
load_mbr:
    mov ax, 0
    mov cx, 1
    xor bx, bx
    mov dx, 0x0720
    call read_sectors_seg
    jc .done
    cmp word [0x73FE], 0xAA55
    jne .done
    mov si, 0x73BE
    mov bx, BOOTINFO+56
    mov di, BOOTINFO+72
    mov cx, 4
    xor dx, dx
.loop:
    cmp byte [si+4], 0
    je .next
    mov eax, [si+8]
    mov [bx], eax
    mov eax, [si+12]
    mov [di], eax
    inc dx
.next:
    add si, 16
    add bx, 4
    add di, 4
    loop .loop
    mov [BOOTINFO+52], dx
.done:
    ret

MEM_MAP   equ 0x7800
MAX_E820  equ 32

; ---------------- E820 物理内存图（P4 用） ----------------
load_e820:
    pusha
    push es
    xor ax, ax
    mov es, ax                     ; INT 15h 用 ES:DI 写条目，必须确保 ES=0
    mov dword [MEM_MAP], 0
    xor ebx, ebx
    mov di, MEM_MAP + 4
    mov bp, 0
.loop:
    cmp bp, MAX_E820
    jae .done
    mov eax, 0xE820
    mov edx, 0x534D4150             ; 'SMAP'
    mov ecx, 24
    mov dword [di + 20], 1          ; 条目属性，部分 BIOS 需要
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    ; 跳过长度为 0 的条目
    mov eax, [di + 8]
    or eax, [di + 12]
    jz .next
    inc bp
    add di, 24
.next:
    test ebx, ebx
    jz .done
    jmp .loop
.done:
    mov [MEM_MAP], bp
    mov eax, MEM_MAP + 4           ; 条目从 +4 开始（前 4 字节是条目数）
    mov [BOOTINFO+100], eax
    movzx eax, bp
    mov [BOOTINFO+104], eax
    mov si, m_e820
    call serial_puts
    pop es
    popa
    ret

; ---------------- 读内核镜像（192 扇区，分两段不跨 64KB） ----------------
load_kernel:
    mov ax, KERNEL_LBA
    mov cx, 128
    xor bx, bx
    mov dx, KERNEL_SEG
    call read_sectors_seg
    jc disk_error

    mov ax, KERNEL_LBA + 128
    mov cx, 96
    xor bx, bx
    mov dx, KERNEL_SEG + 0x1000      ; 0x20000
    call read_sectors_seg
    jc disk_error

    mov ax, KERNEL_LBA + 224
    mov cx, KERNEL_SECTORS - 224     ; 96 扇区 = 48KB，不跨 64KB 段
    xor bx, bx
    mov dx, KERNEL_SEG + 0x1C00      ; 0x2C000
    call read_sectors_seg
    jc disk_error
    ret

; ---------------- 读文件系统镜像（256 扇区） ----------------
load_fs:
    mov ax, FS_LBA
    mov cx, 128
    xor bx, bx
    mov dx, FS_SEG
    call read_sectors_seg
    jc disk_error

    mov ax, FS_LBA + 128
    mov cx, 128
    xor bx, bx
    mov dx, FS_SEG + 0x1000          ; 0x50000
    call read_sectors_seg
    jc disk_error
    ret

; ---------------- 读 head ----------------
load_head:
    mov ax, HEAD_LBA
    mov cx, HEAD_SECTORS
    xor bx, bx
    mov dx, HEAD_SEG
    call read_sectors_seg
    jc disk_error
    ret

; ---------------- bootinfo 扩展字段 ----------------
fill_bootinfo:
    mov dword [BOOTINFO+32], 0x31444D55     ; "UMD1"
    mov dword [BOOTINFO+36], KERNEL_LBA
    mov dword [BOOTINFO+40], KERNEL_SECTORS
    mov dword [BOOTINFO+44], FS_LBA
    mov dword [BOOTINFO+48], FS_SECTORS
    mov dword [BOOTINFO+88], 0              ; 认证改为从 /config/auth 读取
    mov dword [BOOTINFO+92], 60
    mov dword [BOOTINFO+96], 0
    ret

; ---------------- 读扇区：AX=LBA, CX=数量, DX=段, BX=偏移 ----------------
read_sectors_seg:
    mov [seg_tmp], dx
    pusha
    mov [rd_lba], ax
    mov [rd_cnt], cx
    mov [rd_off], bx
    mov [dap_lba], ax
    mov word [dap_lba+2], 0
    mov [dap_cnt], cx
    mov [dap_off], bx
    mov [dap_seg], dx
    mov ah, 0x42
    mov dl, [drive_num]
    mov si, dap
    int 0x13
    jnc .ok
    mov ax, [rd_lba]
    mov cx, [rd_cnt]
    mov bx, [rd_off]
    call read_chs
    jc .fail
.ok:
    popa
    clc
    ret
.fail:
    popa
    stc
    ret

read_chs:
    pusha
    mov [ch_lba], ax
    mov [ch_cnt], cx
    mov [ch_off], bx
.next:
    cmp word [ch_cnt], 0
    je .ok
    mov ax, [ch_lba]
    xor dx, dx
    movzx bx, byte [spt]
    div bx
    inc dl
    mov [ch_sec], dl
    xor dx, dx
    movzx bx, byte [heads]
    div bx
    mov ch, al
    mov dh, dl
    mov cl, [ch_sec]
    mov dl, [drive_num]
    mov bx, [ch_off]
    mov ax, [seg_tmp]
    mov es, ax
    mov ax, 0x0201
    int 0x13
    jc .fail
    inc word [ch_lba]
    add word [ch_off], 512
    dec word [ch_cnt]
    jmp .next
.ok:
    popa
    clc
    ret
.fail:
    popa
    stc
    ret

disk_error:
    mov si, m_disk
    call serial_puts
    cli
.h:
    hlt
    jmp .h

; ---------------- GDT 与保护模式 ----------------
enter_pm:
    cli
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
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp 0x08:0x9000             ; 进入第三级 head

[BITS 16]
gdt:
    dq 0
    ; 0x08: 32 位代码段
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    ; 0x10: 64 位代码段（L=1）
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xAF, 0x00
    ; 0x18: 数据段
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:
gdtr:
    dw gdt_end - gdt - 1
    dd gdt

drive_num  db 0
spt        db 18
heads      db 2
seg_tmp    dw 0
rd_lba     dw 0
rd_cnt     dw 0
rd_off     dw 0
ch_lba     dw 0
ch_cnt     dw 0
ch_off     dw 0
ch_sec     db 0
best_mode  dw 0xFFFF
cur_mode   dw 0
vbe_block  times 512 db 0
mode_block times 256 db 0

dap:
    db 0x10, 0
dap_cnt  dw 0
dap_off  dw 0
dap_seg  dw 0
dap_lba  dq 0

m_loader   db '[loader] ', 0
m_e820     db 'e820 ok', 0x0D, 0x0A, 0
m_vbe      db 'vbe ok, A20 ok', 0x0D, 0x0A, 0
m_ok       db 'images loaded, enter protected mode', 0x0D, 0x0A, 0
m_vbe_fail db 'VBE: no linear framebuffer', 0x0D, 0x0A, 0
m_a20_fail db 'A20: enable failed', 0x0D, 0x0A, 0
m_disk     db 'disk read error', 0x0D, 0x0A, 0

%include "serial16.inc"

times (8 * 512) - ($ - $$) db 0
