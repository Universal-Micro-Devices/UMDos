; ============================================================
; vga8.inc - VGA 320x200x256 绘图 + 8x8 字库
;   vga_init            : 切到 13h，设置调色板 1 号色为深蓝并清屏
;   vga_clear   AL=颜色  : 整屏填充
;   draw_pixel  BX=X, CX=Y, AL=颜色（自动裁剪）
;   draw_rect   BX=X, CX=Y, SI=宽, DI=高, AL=颜色
;   draw_frame  BX=X, CX=Y, SI=宽, DI=高, AL=颜色（空心）
;   draw_char   AL=字符, 位置取 text_x/text_y, 颜色取 text_color
;   draw_string SI=字符串, BX=X, CX=Y, AL=颜色
; 说明: 行宽 320 字节；所有例程保存/恢复被破坏的寄存器
; ============================================================

%include "font8x8.inc"

text_x     dw 0
text_y     dw 0
text_color db 15
rect_x     dw 0
rect_y     dw 0
rect_w     dw 0
rect_h     dw 0
rect_c     db 0

; ---------------- 初始化图形模式 ----------------
vga_init:
    mov ax, 0x0013
    int 0x10
    ; 有些虚拟/板载 VGA 的 BIOS 只把模式切了一半：分辨率变了，但平面掩码
    ; 或 chain4 没设，写进显存的东西根本显示不出来（全黑）。这里显式再设一遍。
    mov dx, 0x3C4
    mov ax, 0x0100                  ; 定序器：同步复位
    out dx, ax
    mov ax, 0x0E04                  ; 内存模式 = chain4 + 关闭 odd/even + 扩展
    out dx, ax
    mov ax, 0x0F02                  ; 映射掩码 = 4 个平面全开
    out dx, ax
    mov ax, 0x0300                  ; 结束复位
    out dx, ax
    mov dx, 0x3D4
    mov al, 0x17
    out dx, al
    inc dx
    in al, dx
    or al, 0x40                     ; CRTC：字节模式
    out dx, al
    ; 调色板 1 号色 = 深蓝 (DAC 只有 6 位！)
    mov dx, 0x3C8
    mov al, 1
    out dx, al
    inc dx
    xor al, al
    out dx, al                      ; R
    out dx, al                      ; G
    mov al, 42
    out dx, al                      ; B
    mov al, 1
    call vga_clear
    ret

; ---------------- 清屏 ----------------
vga_clear:
    pushf
    cli                             ; 填充期间别被中断打断（个别 BIOS 不保 ES）
    pusha
    push es
    mov dx, 0xA000
    mov es, dx
    xor di, di
    mov cx, 64000
    cld
    rep stosb
    pop es
    popa
    popf
    ret

; ---------------- 画点 ----------------
draw_pixel:
    cmp bx, 320
    jae .ret
    cmp cx, 200
    jae .ret
    push dx
    push es
    push di
    mov dx, 0xA000
    mov es, dx
    imul di, cx, 320                ; 行宽 320 = 0x140
    add di, bx
    stosb
    pop di
    pop es
    pop dx
.ret:
    ret

; ---------------- 实心矩形 ----------------
draw_rect:
    pusha
    mov [rect_x], bx
    mov [rect_y], cx
    mov [rect_w], si
    mov [rect_h], di
    mov [rect_c], al
.row:
    mov cx, [rect_y]
    mov bx, [rect_x]
    mov si, [rect_w]
.col:
    mov al, [rect_c]
    call draw_pixel
    inc bx
    dec si
    jnz .col
    inc word [rect_y]
    dec word [rect_h]
    jnz .row
    popa
    ret

; ---------------- 空心矩形 ----------------
draw_frame:
    pusha
    mov [rect_x], bx
    mov [rect_y], cx
    mov [rect_w], si
    mov [rect_h], di
    mov [rect_c], al
    ; 上
    call .hline
    ; 下
    mov cx, [rect_y]
    add cx, [rect_h]
    dec cx
    call .hline
    ; 左
    mov cx, [rect_y]
    mov bx, [rect_x]
    mov di, [rect_h]
.vline:
    mov al, [rect_c]
    call draw_pixel
    inc cx
    dec di
    jnz .vline
    ; 右
    mov cx, [rect_y]
    mov bx, [rect_x]
    add bx, [rect_w]
    dec bx
    mov di, [rect_h]
.vline2:
    mov al, [rect_c]
    call draw_pixel
    inc cx
    dec di
    jnz .vline2
    popa
    ret
.hline:
    mov bx, [rect_x]
    mov si, [rect_w]
.hloop:
    mov al, [rect_c]
    call draw_pixel
    inc bx
    dec si
    jnz .hloop
    ret

; ---------------- 画一个字符 ----------------
draw_char:
    pusha
    cmp al, 32
    jae .ok
    mov al, 32
.ok:
    cmp al, 126
    jbe .ok2
    mov al, 32
.ok2:
    sub al, 32
    movzx di, al
    shl di, 3
    add di, font8x8
    mov si, 8                       ; 行
.row:
    mov ah, [di]
    inc di
    mov dx, 8                       ; 列
.col:
    shl ah, 1
    jnc .skip
    push ax
    push dx
    push si
    mov al, [text_color]
    mov bx, [text_x]
    add bx, 8
    sub bx, dx
    mov cx, [text_y]
    add cx, 8
    sub cx, si
    call draw_pixel
    pop si
    pop dx
    pop ax
.skip:
    dec dx
    jnz .col
    dec si
    jnz .row
    popa
    ret

; ---------------- 输出字符串 ----------------
draw_string:
    pusha
    mov [text_color], al
    mov [text_x], bx
    mov [text_y], cx
.loop:
    lodsb
    or al, al
    jz .done
    call draw_char
    add word [text_x], 8
    jmp .loop
.done:
    popa
    ret
