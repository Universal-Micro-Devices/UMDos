; ============================================================
; boot.asm - 第一级引导扇区 (LBA 0)
;   1. 读 LBA 6，若首字节 = 0xAA 说明已激活，直接进 loader
;   2. 否则提示输入用户名/密码（存 0x0500 / 0x0520，供 loader 计算哈希）
;   3. 读 LBA 1..5 (5 个扇区) 到 0x7E00 并跳转
;
;   编译: nasm -f bin boot.asm -o boot.bin   (正好 512 字节)
;   磁盘读写: 先 EDD(AH=42h)，失败退回简单 CHS（LBA<18，仅第 0 磁道）
; ============================================================
[BITS 16]
[ORG 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drv], dl

    ; ---- 已激活？----
    mov ax, 6
    mov cx, 1
    mov bx, 0x1600
    call read
    jc .err
    cmp byte [0x1600], 0xAA
    je .boot

    ; ---- 首次安装：用户名 / 密码 ----
    mov si, m_head
    call puts
    mov si, m_user
    call puts
    mov byte [mask], 0
    mov di, 0x0500
    mov cx, 16
    call gets
    mov si, m_pass
    call puts
    mov byte [mask], 1
    mov di, 0x0520
    mov cx, 16
    call gets

.boot:
    mov ax, 1
    mov cx, 5
    mov bx, 0x7E00
    call read
    jc .err
    mov dl, [drv]
    jmp 0x0000:0x7E00

.err:
    mov si, m_err
    call puts
    cli
.hang:
    hlt
    jmp .hang

; ---------------- 读扇区: AX=LBA, CX=数量, BX=偏移 ----------------
read:
    pusha
    mov [lba], ax
    mov [cnt], cx
    mov [off], bx
    mov [dap.lba], ax
    mov word [dap.lba+4], 0
    mov word [dap.lba+6], 0
    mov [dap.count], cx
    mov [dap.off], bx
    mov [dap.seg], ds
    mov ah, 0x42
    mov dl, [drv]
    mov si, dap
    int 0x13
    jnc .ok

    ; ---- CHS 回退（每次 1 扇区，仅第 0 磁道）----
    mov cx, [cnt]
.chs_loop:
    push cx
    mov ax, [lba]
    inc ax
    mov cl, al
    mov ch, 0
    mov dh, 0
    mov dl, [drv]
    mov bx, [off]
    mov ax, ds
    mov es, ax
    mov ax, 0x0201
    int 0x13
    pop cx
    jc .fail
    inc word [lba]
    add word [off], 512
    loop .chs_loop
.ok:
    popa
    clc
    ret
.fail:
    popa
    stc
    ret

; ---------------- 输出字符串 ----------------
puts:
    pusha
.l:
    lodsb
    or al, al
    jz .d
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .l
.d:
    popa
    ret

; ---------------- 输入: DI=缓冲, CX=上限 ----------------
gets:
    pusha
    mov [in_max], cx
    xor bp, bp
.l:
    xor ah, ah
    int 0x16
    cmp al, 0x0D
    je .d
    cmp al, 0x08
    je .b
    cmp al, 0x20
    jb .l
    cmp al, 0x7E
    ja .l
    cmp bp, [in_max]
    jae .l
    mov [di], al                    ; DS 存，避免依赖 BIOS 保留 ES
    inc di
    inc bp
    push ax
    cmp byte [mask], 0
    je .e2
    mov al, '*'
.e2:
    mov ah, 0x0E
    int 0x10
    pop ax
    jmp .l
.b:
    test bp, bp
    jz .l
    dec di
    dec bp
    mov ah, 0x0E
    mov al, 0x08
    int 0x10
    mov al, ' '
    int 0x10
    mov al, 0x08
    int 0x10
    jmp .l
.d:
    mov byte [di], 0
    mov ah, 0x0E
    mov al, 0x0D
    int 0x10
    mov al, 0x0A
    int 0x10
    popa
    ret

; ---------------- 数据 ----------------
drv     db 0
mask    db 0
lba     dw 0
cnt     dw 0
off     dw 0
in_max  dw 0

dap:
    db 0x10, 0
.count  dw 0
.off    dw 0
.seg    dw 0
.lba    dq 0

m_head  db 0x0D, 0x0A, 'UMDos installer', 0x0D, 0x0A, 0
m_user  db 'Username: ', 0
m_pass  db 'Password: ', 0
m_err   db 0x0D, 0x0A, 'Disk error!', 0x0D, 0x0A, 0

times 510 - ($ - $$) db 0
dw 0xAA55
