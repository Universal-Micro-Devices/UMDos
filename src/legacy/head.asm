; ============================================================
; head.asm - 第三级：图形登录界面 (LBA 8..17, 加载到 0x8800)
;   - VGA 320x200x256 + 8x8 字库
;   - 键盘输入（Tab 切换 / 退格 / 回车登录）
;   - PS/2 鼠标（可选，初始化失败自动降级为纯键盘）
;   - 用 SHA-256 校验用户名/密码，与 loader 放进 0x0700 的 creds 块比较
;   - 通过后读 LBA 18..29 (12 扇区) 到 0xA000 进入 HelloAndLoading
; ============================================================
[BITS 16]
[ORG 0x8800]

CREDS    equ 0x0700
USER     equ 0x1000
PASS     equ 0x1020
HASH     equ 0x1100
SEC_BUF  equ 0x1600

BOX_X    equ 30
BOX_W    equ 220
BOX_H    equ 16
BOX1_Y   equ 64
BOX2_Y   equ 104
TXT_X    equ 34
TXT1_Y   equ 68
TXT2_Y   equ 108

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8800
    sti
    cld
    mov [drive_num], dl
    ; 清空输入缓冲（0x1000/0x1020 可能残留上一阶段的数据）
    mov di, USER
    mov cx, 32
    xor ax, ax
    cld
    rep stosb
    mov di, PASS
    mov cx, 32
    rep stosb
    call disk_init
    mov si, msg_gfx
    call puts                       ; 切图形模式前的文本面包屑：看到它之后变黑就是 VGA 初始化
    call vga_init
    call draw_ui
    call mouse_init                 ; 失败不影响键盘
    call draw_fields

; ---------------- 主循环 ----------------
.loop:
    mov ah, 1
    int 0x16
    jz .idle
    mov ah, 0
    int 0x16
    call key
    jmp .loop
.idle:
    sti
    hlt
    jmp .loop

; ---------------- 按键处理 ----------------
key:
    cmp al, 0x0D
    je login_try
    cmp al, 0x09
    je tab_key
    cmp al, 0x08
    je bs_key
    cmp al, 0x20
    jb .ret
    cmp al, 0x7E
    ja .ret

    ; 追加字符
    mov bx, USER
    cmp byte [field], 0
    je .have_buf
    mov bx, PASS
.have_buf:
    mov cl, [ulen]
    cmp byte [field], 0
    je .len_ok
    mov cl, [plen]
.len_ok:
    xor ch, ch
    cmp cx, 16
    jae .ret
    add bx, cx
    mov [bx], al
    inc cx
    mov [bx+1], byte 0
    cmp byte [field], 1
    jne .store_len
    mov [plen], cl
    jmp .redraw
.store_len:
    mov [ulen], cl
.redraw:
    call draw_fields
.ret:
    ret

tab_key:
    xor byte [field], 1
    call draw_fields
    ret

bs_key:
    ; 当前字段长度
    mov cl, [ulen]
    cmp byte [field], 0
    je .have
    mov cl, [plen]
.have:
    xor ch, ch
    test cl, cl
    jz .redraw
    dec cx
    mov bx, USER
    cmp byte [field], 0
    je .p
    mov bx, PASS
.p:
    add bx, cx
    mov byte [bx], 0
    cmp byte [field], 0
    jne .setp
    mov [ulen], cl
    jmp .redraw
.setp:
    mov [plen], cl
.redraw:
    call draw_fields
    ret

; ---------------- 登录校验 ----------------
login_try:
    cmp byte [CREDS], 0x5A
    jne .nocr
    ; 用户名哈希
    mov si, USER
    call strlen
    cmp ax, 0
    je .fail
    mov cx, ax
    mov si, USER
    mov di, HASH
    call sha256
    mov ax, ds                      ; repe cmpsb 用 ES:DI
    mov es, ax
    mov si, HASH
    mov di, CREDS+1
    mov cx, 32
    cld
    repe cmpsb
    jne .fail
    ; 密码哈希
    mov si, PASS
    call strlen
    cmp ax, 0
    je .fail
    mov cx, ax
    mov si, PASS
    mov di, HASH
    call sha256
    mov ax, ds
    mov es, ax
    mov si, HASH
    mov di, CREDS+33
    mov cx, 32
    cld
    repe cmpsb
    jne .fail

    ; ---- 通过 ----
    mov si, s_ok
    mov bx, 30
    mov cx, 156
    mov al, 10
    call draw_string
    mov ax, 18
    mov cx, 12
    mov bx, 0xA000
    call read_sectors
    jc disk_error
    mov dl, [drive_num]
    jmp 0x0000:0xA000

.fail:
    mov si, s_fail
    mov bx, 30
    mov cx, 156
    mov al, 12
    call draw_string
    mov ax, ds
    mov es, ax
    mov di, PASS
    mov cx, 32
    xor ax, ax
    cld
    rep stosb
    mov byte [plen], 0
    call draw_fields
    ret
.nocr:
    mov si, s_nocr
    mov bx, 30
    mov cx, 156
    mov al, 12
    call draw_string
    ret

; ---------------- 静态界面 ----------------
draw_ui:
    mov si, s_title
    mov bx, 30
    mov cx, 10
    mov al, 15
    call draw_string

    mov si, s_sub
    mov bx, 30
    mov cx, 26
    mov al, 11
    call draw_string

    mov si, s_user
    mov bx, 30
    mov cx, 52
    mov al, 15
    call draw_string

    mov bx, BOX_X
    mov cx, BOX1_Y
    mov si, BOX_W
    mov di, BOX_H
    xor al, al
    call draw_rect

    mov si, s_pass
    mov bx, 30
    mov cx, 92
    mov al, 15
    call draw_string

    mov bx, BOX_X
    mov cx, BOX2_Y
    mov si, BOX_W
    mov di, BOX_H
    xor al, al
    call draw_rect

    mov si, s_hint
    mov bx, 30
    mov cx, 134
    mov al, 7
    call draw_string

    mov si, s_footer
    mov bx, 30
    mov cx, 184
    mov al, 8
    call draw_string
    ret

; ---------------- 重绘输入区 ----------------
draw_fields:
    cli
    call cursor_hide
    ; 清空两个输入框内部
    mov bx, BOX_X+2
    mov cx, BOX1_Y+2
    mov si, BOX_W-4
    mov di, BOX_H-4
    xor al, al
    call draw_rect
    mov bx, BOX_X+2
    mov cx, BOX2_Y+2
    mov si, BOX_W-4
    mov di, BOX_H-4
    xor al, al
    call draw_rect

    ; 用户名
    mov si, USER
    mov bx, TXT_X
    mov cx, TXT1_Y
    mov al, 15
    call draw_string

    ; 密码掩码
    call make_mask
    mov si, pmask
    mov bx, TXT_X
    mov cx, TXT2_Y
    mov al, 15
    call draw_string

    ; 边框
    mov bx, BOX_X
    mov cx, BOX1_Y
    mov si, BOX_W
    mov di, BOX_H
    mov al, 7
    cmp byte [field], 0
    jne .b1
    mov al, 15
.b1:
    call draw_frame

    mov bx, BOX_X
    mov cx, BOX2_Y
    mov si, BOX_W
    mov di, BOX_H
    mov al, 7
    cmp byte [field], 1
    jne .b2
    mov al, 15
.b2:
    call draw_frame

    ; 光标（下划线）
    mov al, [ulen]
    mov cx, TXT1_Y+9
    cmp byte [field], 0
    je .c1
    mov al, [plen]
    mov cx, TXT2_Y+9
.c1:
    xor ah, ah
    mov bx, TXT_X
    add bx, ax
    mov si, 6
    mov di, 1
    mov al, 15
    call draw_rect
    call cursor_show
    sti
    ret

; ---------------- 密码显示缓冲 ----------------
make_mask:
    pusha
    mov cl, [plen]
    xor ch, ch
    mov di, pmask
    mov al, '*'
    test cx, cx
    jz .done
.fill:
    mov [di], al                    ; DS 存，不依赖 ES
    inc di
    loop .fill
.done:
    mov byte [di], 0
    popa
    ret

; ================= 鼠标（可选） =================
kbc_wait_write:
    push cx
    mov cx, 0x4000
.w:
    in al, 0x64
    test al, 2
    jz .ok
    loop .w
    pop cx
    stc
    ret
.ok:
    pop cx
    clc
    ret

kbc_wait_read:
    push cx
    mov cx, 0x4000
.w:
    in al, 0x64
    test al, 1
    jnz .ok
    loop .w
    pop cx
    stc
    ret
.ok:
    pop cx
    clc
    ret

mouse_cmd:
    push bx
    mov bl, al
    call kbc_wait_write
    jc .bad
    mov al, 0xD4
    out 0x64, al
    call kbc_wait_write
    jc .bad
    mov al, bl
    out 0x60, al
    call kbc_wait_read
    jc .bad
    in al, 0x60
    cmp al, 0xFA
    jne .bad
    clc
    pop bx
    ret
.bad:
    stc
    pop bx
    ret

mouse_init:
    pusha
    call kbc_wait_write
    jc .fail
    mov al, 0xA8                    ; 打开 aux 端口
    out 0x64, al

    call kbc_wait_write
    jc .fail
    mov al, 0x20                    ; 读配置字节
    out 0x64, al
    call kbc_wait_read
    jc .fail
    in al, 0x60
    or al, 0x03                     ; IRQ1 + IRQ12
    and al, 0xDF                    ; aux 时钟打开
    mov bl, al
    call kbc_wait_write
    jc .fail
    mov al, 0x60
    out 0x64, al
    call kbc_wait_write
    jc .fail
    mov al, bl
    out 0x60, al

    mov al, 0xF6
    call mouse_cmd
    jc .fail
    mov al, 0xF4
    call mouse_cmd
    jc .fail

    cli
    push es
    xor ax, ax
    mov es, ax
    mov word [es:0x74*4], mouse_isr
    mov word [es:0x74*4+2], 0
    pop es
    in al, 0xA1
    and al, 0xEF                    ; 打开 IRQ12
    out 0xA1, al
    in al, 0x21
    and al, 0xFB                    ; 打开级联 IRQ2
    out 0x21, al
    sti
    mov byte [m_ok], 1
    popa
    clc
    ret
.fail:
    popa
    stc
    ret

; ---------------- IRQ12 处理 ----------------
mouse_isr:
    pusha
    in al, 0x60
    mov bl, [mstate]
    test bl, bl
    jz .b0
    cmp bl, 1
    je .b1
    mov [mdy], al
    mov byte [mstate], 0
    call mouse_apply
    jmp .done
.b0:
    test al, 0x08
    jz .done
    mov [mflags], al
    mov byte [mstate], 1
    jmp .done
.b1:
    mov [mdx], al
    mov byte [mstate], 2
    jmp .done
.done:
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    popa
    iret

mouse_apply:
    call cursor_hide
    ; dx
    mov al, [mdx]
    movsx cx, al
    add [mx], cx
    ; dy（屏幕 Y 向下）
    mov al, [mdy]
    movsx cx, al
    sub [my], cx
    ; 裁剪
    cmp word [mx], 0
    jge .xok
    mov word [mx], 0
.xok:
    cmp word [mx], 314
    jle .xok2
    mov word [mx], 314
.xok2:
    cmp word [my], 0
    jge .yok
    mov word [my], 0
.yok:
    cmp word [my], 194
    jle .yok2
    mov word [my], 194
.yok2:
    call cursor_show
    ; 左键按下沿 -> 切换焦点
    mov al, [mflags]
    test al, 1
    jz .nobtn
    test byte [prev_btn], 1
    jnz .nobtn
    ; 是否点在两个框内
    mov ax, [my]
    cmp ax, BOX1_Y-4
    jb .nobtn
    cmp ax, BOX2_Y+BOX_H
    ja .nobtn
    mov ax, [mx]
    cmp ax, BOX_X-4
    jb .nobtn
    cmp ax, BOX_X+BOX_W
    ja .nobtn
    mov ax, [my]
    cmp ax, BOX2_Y
    jb .f1
    mov byte [field], 1
    jmp .redraw
.f1:
    mov byte [field], 0
.redraw:
    call draw_fields
.nobtn:
    mov al, [mflags]
    mov [prev_btn], al
    ret

; 在 (mx,my) 处 XOR 一个 5x5 方块；mvis 跟踪光标当前是否可见，
; 避免"隐藏/显示"两次 XOR 相互抵消。
cursor_show:
    cmp byte [m_ok], 0
    je .ret
    cmp byte [mvis], 0
    jne .ret
    mov byte [mvis], 1
    jmp cursor_xor
.ret:
    ret

cursor_hide:
    cmp byte [m_ok], 0
    je .ret
    cmp byte [mvis], 0
    je .ret
    mov byte [mvis], 0
    jmp cursor_xor
.ret:
    ret

cursor_xor:
    pusha
    mov cx, [my]
    mov si, 5
.r:
    mov bx, [mx]
    mov di, 5
.c:
    call xor_px
    inc bx
    dec di
    jnz .c
    inc cx
    dec si
    jnz .r
    popa
    ret

xor_px:
    cmp bx, 320
    jae .ret
    cmp cx, 200
    jae .ret
    push ax
    push dx
    push di
    push es
    mov dx, 0xA000
    mov es, dx
    imul di, cx, 320
    add di, bx
    mov al, [es:di]
    xor al, 0x0F
    mov [es:di], al
    pop es
    pop di
    pop dx
    pop ax
.ret:
    ret

; ---------------- 数据 ----------------
drive_num db 0
field     db 0
ulen      db 0
plen      db 0
m_ok      db 0
mvis      db 0
mstate    db 0
mflags    db 0
mdx       db 0
mdy       db 0
prev_btn  db 0
mx        dw 160
my        dw 100
pmask     times 18 db 0

msg_gfx  db 'Entering graphics mode...', 0x0D, 0x0A, 0
s_title  db 'UMDos-DOS1 beta', 0
s_sub    db 'Graphical login', 0
s_user   db 'Username:', 0
s_pass   db 'Password:', 0
s_hint   db 'Enter=Login  Tab=Next  BS=Del', 0
s_footer db 'UMDos boot chain v1.0', 0
s_ok     db 'Login OK, loading...', 0
s_fail   db 'Login failed!', 0
s_nocr   db 'No credentials - reboot and install', 0

; 公共库放在入口代码之后，保证 start 位于映像偏移 0
%include "io16.inc"
%include "disk16.inc"
%include "sha256.inc"
%include "vga8.inc"

times (10 * 512) - ($ - $$) db 0
