; ============================================================
; boot.asm - 第一级引导扇区（LBA 0, 0x7C00, 512 字节）
;
; P2 之后 boot 只做一件事：把第二级 loader 读到 0x7E00 然后跳过去。
;   - 不再有用户名/密码/激活码输入
;   - 不再解析分区表
;   - 不再加载任何内核或文件系统镜像
; 输出一行串口日志 [boot] ...，便于阶段定位。
; 旧实现保留在 src/legacy/boot.asm 供审计对照。
; ============================================================
[BITS 16]
[ORG 0x7C00]

LOADER_LBA  equ 1
LOADER_SEC  equ 8
LOADER_SEG  equ 0x07E0          ; 0x7E00

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

    call serial_init
    mov si, m_boot
    call serial_puts

    call disk_init
    mov ax, LOADER_LBA
    mov cx, LOADER_SEC
    xor bx, bx
    mov dx, LOADER_SEG
    call read_sectors_seg
    jc .fail

    mov si, m_ok
    call serial_puts
    jmp 0x0000:0x7E00

.fail:
    mov si, m_err
    call serial_puts
.hang:
    cli
    hlt
    jmp .hang

; ---------------- 几何参数（只在 boot 探测一次） ----------------
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

drive_num db 0
spt       db 18
heads     db 2
seg_tmp   dw 0
rd_lba    dw 0
rd_cnt    dw 0
rd_off    dw 0
ch_lba    dw 0
ch_cnt    dw 0
ch_off    dw 0
ch_sec    db 0

dap:
    db 0x10, 0
dap_cnt  dw 0
dap_off  dw 0
dap_seg  dw 0
dap_lba  dq 0

m_boot db '[boot] ', 0
m_ok   db 'stage1 ok, jump loader', 0x0D, 0x0A, 0
m_err  db 'disk error', 0x0D, 0x0A, 0

%include "serial16.inc"

times 510 - ($ - $$) db 0
dw 0xAA55
