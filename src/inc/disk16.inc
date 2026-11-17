; ============================================================
; disk16.inc - 16 位磁盘读写
;   优先用 EDD(INT 13h AH=42h/43h)；失败自动退回 CHS（软盘 /
;   El Torito 软盘仿真只支持 CHS）。
;
;   调用方必须定义:  drive_num db 0
;   本文件定义:      spt heads dap read_sectors write_sector
;                    disk_init disk_error
;
;   read_sectors : AX = LBA(16 位), CX = 扇区数, BX = 缓冲区偏移(段 = DS)
;   write_sector : AX = LBA(16 位), BX = 缓冲区偏移(段 = DS), 写 1 扇区
;   两者失败时返回 CF = 1
; ============================================================

spt       db 18
heads     db 2

dap:
    db 0x10, 0
.count  dw 0
.off    dw 0
.seg    dw 0
.lba    dq 0

chs_lba   dw 0
chs_cnt   dw 0
chs_off   dw 0
chs_sec   db 0
edd_lba   dw 0
edd_cnt   dw 0
edd_off   dw 0
seg_tmp   dw 0

; ---------------- 探测几何参数 ----------------
disk_init:
    pusha
    push es
    mov ah, 0x08
    mov dl, [drive_num]
    int 0x13
    jc .default
    and cl, 0x3F
    mov [spt], cl
    inc dh
    mov [heads], dh
    pop es
    popa
    ret
.default:
    mov byte [spt], 18
    mov byte [heads], 2
    pop es
    popa
    ret

; ---------------- 读扇区 ----------------
; read_sectors     : AX=LBA, CX=数量, BX=偏移, 段 = DS
; read_sectors_seg : AX=LBA, CX=数量, BX=偏移, DX=段
read_sectors:
    mov [seg_tmp], ds
    jmp read_common

read_sectors_seg:
    mov [seg_tmp], dx

read_common:
    pusha
    mov [edd_lba], ax
    mov [edd_cnt], cx
    mov [edd_off], bx
    mov [dap.lba], ax
    mov word [dap.lba+4], 0
    mov word [dap.lba+6], 0
    mov [dap.count], cx
    mov [dap.off], bx
    mov ax, [seg_tmp]
    mov [dap.seg], ax
    mov ah, 0x42
    mov dl, [drive_num]
    mov si, dap
    int 0x13
    jnc .ok
    ; EDD 不支持：还原被 BIOS 改掉的参数后走 CHS
    mov ax, [edd_lba]
    mov cx, [edd_cnt]
    mov bx, [edd_off]
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

; ---------------- CHS 读（每次 1 扇区） ----------------
read_chs:
    pusha
    push es
    mov [chs_lba], ax
    mov [chs_cnt], cx
    mov [chs_off], bx
.next:
    cmp word [chs_cnt], 0
    je .ok
    mov ax, [chs_lba]
    xor dx, dx
    movzx bx, byte [spt]
    div bx                          ; AX = 磁道号, DX = 扇区号(0 基)
    inc dl
    mov [chs_sec], dl
    xor dx, dx
    movzx bx, byte [heads]
    div bx                          ; AX = 柱面, DX = 磁头
    mov ch, al
    mov dh, dl
    mov cl, [chs_sec]
    mov dl, [drive_num]
    mov bx, [chs_off]
    mov ax, [seg_tmp]
    mov es, ax
    mov ax, 0x0201
    int 0x13
    jc .fail
    inc word [chs_lba]
    add word [chs_off], 512
    dec word [chs_cnt]
    jmp .next
.ok:
    pop es
    popa
    clc
    ret
.fail:
    pop es
    popa
    stc
    ret

; ---------------- 写扇区（只走 EDD，只读介质会失败） ----------------
write_sector:
    pusha
    mov [dap.lba], ax
    mov word [dap.lba+4], 0
    mov word [dap.lba+6], 0
    mov word [dap.count], 1
    mov [dap.off], bx
    mov [dap.seg], ds
    mov ah, 0x43
    xor al, al
    mov dl, [drive_num]
    mov si, dap
    int 0x13
    popa
    ret                             ; CF 保留给调用方

; ---------------- 致命磁盘错误 ----------------
disk_error:
    mov si, msg_disk_error
    call puts
    cli
.hang:
    hlt
    jmp .hang
