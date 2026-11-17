; ============================================================
; io16.inc - 16 位实模式 文本输出 / 键盘输入 / 字符串长度
;   puts      : SI = 以 0 结尾的字符串
;   gets      : DI = 缓冲区, CX = 最大字符数(不含结尾 0), 长度回填 in_len
;   gets_mask : 同 gets, 但回显 '*'
;   strlen    : SI -> AX
; 依赖: 无（只用 BIOS INT 10h / INT 16h）
; ============================================================

in_len    dw 0
in_max    dw 0
in_ptr    dw 0
in_mask   db 0
in_echo   db 0

; ---------------- 输出字符串 ----------------
puts:
    pusha
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .loop
.done:
    popa
    ret

; ---------------- 输入（明文） ----------------
gets:
    mov byte [in_mask], 0
    jmp gets_common

; ---------------- 输入（回显 *） ----------------
gets_mask:
    mov byte [in_mask], 1

; ---------------- 公共实现 ----------------
gets_common:
    push es
    pusha
    mov ax, ds                      ; 不信任 BIOS 保留 ES，强制 ES=DS
    mov es, ax
    mov [in_ptr], di
    mov [in_max], cx
    xor bp, bp                      ; bp = 已输入长度
.loop:
    xor ah, ah
    int 0x16                        ; 等待按键
    cmp al, 0x0D
    je .done
    cmp al, 0x08
    je .backspace
    cmp al, 0x20
    jb .loop                        ; 忽略控制字符
    cmp al, 0x7E
    ja .loop
    cmp bp, [in_max]                ; 长度上限，防缓冲区溢出
    jae .loop
    mov [in_echo], al
    cmp byte [in_mask], 0
    je .store
    mov byte [in_echo], '*'
.store:
    mov [di], al                    ; 用 DS 存，不依赖 ES（部分 BIOS 不保留 ES）
    inc di
    inc bp
    mov ah, 0x0E
    mov al, [in_echo]
    int 0x10
    jmp .loop
.backspace:
    test bp, bp
    jz .loop
    dec di
    dec bp
    mov ah, 0x0E
    mov al, 0x08
    int 0x10
    mov al, ' '
    int 0x10
    mov al, 0x08
    int 0x10
    jmp .loop
.done:
    mov byte [di], 0                ; 结尾 0（同样走 DS）
    mov ah, 0x0E
    mov al, 0x0D
    int 0x10
    mov al, 0x0A
    int 0x10
    mov [in_len], bp
    popa
    pop es
    ret

; ---------------- 求长度 ----------------
strlen:
    push si
    xor ax, ax
.loop:
    cmp byte [si], 0
    je .done
    inc si
    inc ax
    jmp .loop
.done:
    pop si
    ret

; ---------------- AL 输出为两位十六进制（诊断用） ----------------
put_hex8:
    pusha
    mov bl, al
    shr al, 4
    call .nib
    mov al, bl
    and al, 0x0F
    call .nib
    popa
    ret
.nib:
    cmp al, 10
    jb .dig
    add al, 'A' - 10
    jmp .out
.dig:
    add al, '0'
.out:
    mov ah, 0x0E
    push bx
    mov bx, 0x0007
    int 0x10
    pop bx
    ret

msg_disk_error db 0x0D, 0x0A, 'Disk error!', 0x0D, 0x0A, 0
