; ============================================================
; installer/boot.asm - 安装引导扇区 (LBA 0)
;   只做一件事：把现有的 FirstMain (LBA 30..45, 16 扇区) 读到 0xC000
;   然后跳过去。FirstMain 会完成 VBE 初始化、加载 LBA 50 起的
;   安装程序镜像、建立页表并进入 64 位长模式，最后由 NextMain
;   跳到 0x10000 执行安装程序。
;
;   这样安装引导完全复用现有启动链的后半段，不需要修改任何
;   现有内核源文件。
; ============================================================
[BITS 16]
[ORG 0x7C00]

STAGE2_LBA   equ 30
STAGE2_SECT  equ 16
STAGE2_SEG   equ 0x0C00        ; 段:0xC000 线性地址 = firstmain 的 0xC000

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

    call disk_init
    mov ax, STAGE2_LBA
    mov cx, STAGE2_SECT
    xor bx, bx
    mov dx, STAGE2_SEG
    call read_sectors_seg
    jc .fail
    jmp 0x0000:0xC000

.fail:
    mov si, msg_fail
    call puts
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

puts:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp puts
.done:
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

msg_fail db 'UMDos installer: disk read error', 0x0D, 0x0A, 0

times 510 - ($ - $$) db 0
dw 0xAA55
