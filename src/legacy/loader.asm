; ============================================================
; loader.asm - 第二级引导 (LBA 1..5, 加载到 0x7E00)
;   1. 读 LBA 6：首字节 0xAA -> 已激活，把两个哈希读进 creds 块
;   2. 否则要求输入激活码，SHA-256 与内置哈希比较
;      成功: 计算用户名/密码哈希 -> creds 块 -> 尽力写回 LBA 6/7
;   3. 读 LBA 8..17 (10 扇区) 到 0x8800 并跳转
;
;   creds 块 (0x0700): [0]=0x5A, [1..32]=用户名哈希, [33..64]=密码哈希
;   这是跨阶段的约定，head 只认这个块，不再依赖磁盘可写。
; ============================================================
[BITS 16]
[ORG 0x7E00]

CREDS    equ 0x0700
USER_BUF equ 0x0500
PASS_BUF equ 0x0520
IN_BUF   equ 0x1000
HASH_BUF equ 0x1100
SEC_BUF  equ 0x1600

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7E00
    sti
    cld
    mov [drive_num], dl
    call disk_init

    ; ---- 读 LBA 6 ----
    mov ax, 6
    mov cx, 1
    mov bx, SEC_BUF
    call read_sectors
    jc disk_error
    cmp byte [SEC_BUF], 0xAA
    jne .setup

    ; ---- 已激活：读回两个哈希 ----
    mov si, msg_already
    call puts
    mov byte [CREDS], 0x5A
    mov ax, ds                      ; 后面全是 rep movsb/stosb，强制 ES=DS
    mov es, ax
    mov si, SEC_BUF+1
    mov di, CREDS+1
    mov cx, 32
    cld
    rep movsb
    mov ax, 7
    mov cx, 1
    mov bx, SEC_BUF
    call read_sectors
    jc disk_error
    cmp byte [SEC_BUF], 0x55
    jne .load
    mov si, SEC_BUF+1
    mov di, CREDS+33
    mov cx, 32
    rep movsb
    jmp .load

.setup:
    ; ---- 输入激活码 ----
    mov si, msg_act
    call puts
    mov di, IN_BUF
    mov cx, 31
    call gets

    mov si, IN_BUF
    call strlen
    mov cx, ax
    ; 去掉尾部空格（屏幕上不可见，很容易误输入）
.trim:
    test cx, cx
    jz .trimmed
    mov di, IN_BUF
    add di, cx
    dec di
    cmp byte [di], ' '
    jne .trimmed
    mov byte [di], 0
    dec cx
    jmp .trim
.trimmed:
    cmp cx, 55
    ja .too_long
    mov si, IN_BUF
    mov di, HASH_BUF
    call sha256

    mov ax, ds                      ; repe cmpsb 用 ES:DI，必须强制 ES=DS
    mov es, ax
    mov si, HASH_BUF
    mov di, expected_activate
    mov cx, 32
    cld
    repe cmpsb
    jne .wrong

    ; ---- 用户名哈希 ----
    mov si, USER_BUF
    call strlen
    mov cx, ax
    mov si, USER_BUF
    mov di, CREDS+1
    call sha256

    ; ---- 密码哈希 ----
    mov si, PASS_BUF
    call strlen
    mov cx, ax
    mov si, PASS_BUF
    mov di, CREDS+33
    call sha256

    mov byte [CREDS], 0x5A
    call save_creds                ; 尽力写盘，只读介质失败也继续

    mov si, msg_ok
    call puts
    jmp .load

.wrong:
    ; 失败时打印诊断：输入长度 + 实际算出的前 8 字节哈希
    mov si, msg_wrong
    call puts
    mov si, msg_len
    call puts
    mov al, [SHA_LEN]
    call put_hex8
    mov si, msg_sha
    call puts
    mov si, HASH_BUF
    mov cx, 8
.dl:
    lodsb
    call put_hex8
    loop .dl
    mov si, msg_crlf
    call puts
    cli
.halt1:
    hlt
    jmp .halt1

.too_long:
    mov si, msg_long
    call puts
    cli
.halt2:
    hlt
    jmp .halt2

.load:
    ; ---- 读 head: LBA 8, 10 扇区 -> 0x8800 ----
    mov ax, 8
    mov cx, 10
    mov bx, 0x8800
    call read_sectors
    jc disk_error
    mov dl, [drive_num]
    jmp 0x0000:0x8800

; ---------------- 把 creds 写回 LBA 6 / 7 ----------------
save_creds:
    mov ax, ds                      ; rep stosb/movsb 走 ES:DI
    mov es, ax
    ; LBA 6: [0xAA][用户名哈希]
    mov di, SEC_BUF
    mov cx, 512
    xor ax, ax
    cld
    rep stosb
    mov byte [SEC_BUF], 0xAA
    mov si, CREDS+1
    mov di, SEC_BUF+1
    mov cx, 32
    rep movsb
    mov ax, 6
    mov bx, SEC_BUF
    call write_sector

    ; LBA 7: [0x55][密码哈希]
    mov di, SEC_BUF
    mov cx, 512
    xor ax, ax
    rep stosb
    mov byte [SEC_BUF], 0x55
    mov si, CREDS+33
    mov di, SEC_BUF+1
    mov cx, 32
    rep movsb
    mov ax, 7
    mov bx, SEC_BUF
    call write_sector
    ret

; ---------------- 数据 ----------------
drive_num db 0

msg_act     db 0x0D, 0x0A, 'Enter activation code: ', 0
msg_ok      db 0x0D, 0x0A, 'Activation OK.', 0x0D, 0x0A, 0
msg_already db 0x0D, 0x0A, 'Activated, booting...', 0x0D, 0x0A, 0
msg_wrong   db 0x0D, 0x0A, 'Wrong activation code!', 0
msg_len     db 0x0D, 0x0A, 'len=', 0
msg_sha     db ' sha=', 0
msg_crlf    db 0x0D, 0x0A, 0
msg_long    db 0x0D, 0x0A, 'Code too long (max 55).', 0

; SHA-256("UMDOS2024")
expected_activate:
    db 0x72, 0xB6, 0xE9, 0x15, 0xAB, 0x72, 0x5A, 0x9C
    db 0x95, 0x8B, 0xD7, 0x1B, 0x00, 0x2A, 0x69, 0x4F
    db 0x9D, 0xD6, 0xFA, 0x6D, 0x1F, 0xAF, 0xF9, 0x1A
    db 0x00, 0x22, 0x02, 0x23, 0xED, 0x7F, 0xD9, 0xF5

; 公共库放在入口代码之后，保证 start 位于映像偏移 0
%include "io16.inc"
%include "disk16.inc"
%include "sha256.inc"

times (5 * 512) - ($ - $$) db 0
