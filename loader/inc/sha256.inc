; ============================================================
; sha256.inc - 16 位实模式 SHA-256（单块，输入 0..55 字节）
;   sha256: SI = 源, CX = 长度, DI = 输出 32 字节（大端）
;   工作区固定在 0x1200..0x15FF，不与各阶段的其余缓冲区冲突
;   需要 386+（ror/bswap/32 位寄存器）
; ============================================================

SHA_MSG  equ 0x1200                 ; 64 字节消息块
SHA_W    equ 0x1300                 ; 64 * 4
SHA_H    equ 0x1400                 ; 8 * 4
SHA_A    equ 0x1500                 ; a,b,c,d,e,f,g,h  (32 字节)
SHA_T1   equ 0x1520
SHA_T2   equ 0x1524
SHA_SRC  equ 0x1528
SHA_LEN  equ 0x152A
SHA_DST  equ 0x152C

sha256:
    push es
    pusha
    cld
    mov ax, ds                      ; stosd/rep stosd 走 ES:DI，不能依赖 BIOS 保留 ES
    mov es, ax
    mov [SHA_SRC], si
    mov [SHA_LEN], cx
    mov [SHA_DST], di

    ; ---- H = 初始值 ----
    mov eax, [h0]
    mov [SHA_H+0], eax
    mov eax, [h1]
    mov [SHA_H+4], eax
    mov eax, [h2]
    mov [SHA_H+8], eax
    mov eax, [h3]
    mov [SHA_H+12], eax
    mov eax, [h4]
    mov [SHA_H+16], eax
    mov eax, [h5]
    mov [SHA_H+20], eax
    mov eax, [h6]
    mov [SHA_H+24], eax
    mov eax, [h7]
    mov [SHA_H+28], eax

    ; ---- 清零消息块 ----
    mov di, SHA_MSG
    mov cx, 16
    xor eax, eax
    rep stosd

    ; ---- 复制数据 ----
    mov si, [SHA_SRC]
    mov cx, [SHA_LEN]
    mov di, SHA_MSG
    rep movsb

    ; ---- 0x80 填充 ----
    mov byte [di], 0x80

    ; ---- 64 位长度（比特，大端；高 32 位为 0）----
    movzx eax, word [SHA_LEN]       ; 注意：只读 2 字节，不能 mov eax,[SHA_LEN]
    shl eax, 3
    bswap eax
    mov [SHA_MSG+60], eax

    call sha_transform

    ; ---- 输出 ----
    mov di, [SHA_DST]
    mov eax, [SHA_H+0]
    call store_be32
    mov eax, [SHA_H+4]
    call store_be32
    mov eax, [SHA_H+8]
    call store_be32
    mov eax, [SHA_H+12]
    call store_be32
    mov eax, [SHA_H+16]
    call store_be32
    mov eax, [SHA_H+20]
    call store_be32
    mov eax, [SHA_H+24]
    call store_be32
    mov eax, [SHA_H+28]
    call store_be32
    popa
    pop es
    ret

store_be32:
    bswap eax
    mov [di], eax
    add di, 4
    ret

; ---------------- 压缩函数 ----------------
sha_transform:
    pusha

    ; W[0..15] = 大端消息字
    mov si, SHA_MSG
    mov di, SHA_W
    mov cx, 16
.load_w:
    lodsd
    bswap eax
    stosd
    loop .load_w

    ; W[16..63]
    xor ebx, ebx
.extend:
    mov eax, [SHA_W + ebx*4 + 4]    ; W[i-15]
    mov edx, eax
    ror eax, 7
    ror edx, 18
    xor eax, edx
    mov edx, [SHA_W + ebx*4 + 4]
    shr edx, 3
    xor eax, edx
    mov [SHA_T1], eax               ; s0

    mov eax, [SHA_W + ebx*4 + 56]   ; W[i-2]
    mov edx, eax
    ror eax, 17
    ror edx, 19
    xor eax, edx
    mov edx, [SHA_W + ebx*4 + 56]
    shr edx, 10
    xor eax, edx                    ; s1

    add eax, [SHA_T1]
    add eax, [SHA_W + ebx*4 + 0]    ; W[i-16]
    add eax, [SHA_W + ebx*4 + 36]   ; W[i-7]
    mov [SHA_W + ebx*4 + 64], eax

    inc ebx
    cmp ebx, 48
    jb .extend

    ; a..h = H
    mov eax, [SHA_H+0]
    mov [SHA_A+0], eax
    mov eax, [SHA_H+4]
    mov [SHA_A+4], eax
    mov eax, [SHA_H+8]
    mov [SHA_A+8], eax
    mov eax, [SHA_H+12]
    mov [SHA_A+12], eax
    mov eax, [SHA_H+16]
    mov [SHA_A+16], eax
    mov eax, [SHA_H+20]
    mov [SHA_A+20], eax
    mov eax, [SHA_H+24]
    mov [SHA_A+24], eax
    mov eax, [SHA_H+28]
    mov [SHA_A+28], eax

    ; 64 轮
    xor ebx, ebx
.round:
    ; S1(e)
    mov eax, [SHA_A+16]
    mov edx, eax
    ror eax, 6
    ror edx, 11
    xor eax, edx
    mov edx, [SHA_A+16]
    ror edx, 25
    xor eax, edx

    ; ch(e,f,g)
    mov ecx, [SHA_A+16]
    mov edx, [SHA_A+20]
    and ecx, edx
    mov edx, [SHA_A+16]
    not edx
    and edx, [SHA_A+24]
    xor ecx, edx

    add eax, ecx
    add eax, [SHA_A+28]             ; h
    add eax, [K + ebx*4]
    add eax, [SHA_W + ebx*4]
    mov [SHA_T1], eax               ; T1

    ; S0(a)
    mov eax, [SHA_A+0]
    mov edx, eax
    ror eax, 2
    ror edx, 13
    xor eax, edx
    mov edx, [SHA_A+0]
    ror edx, 22
    xor eax, edx

    ; maj(a,b,c)
    mov ecx, [SHA_A+0]
    mov edx, [SHA_A+4]
    and ecx, edx
    mov edx, [SHA_A+0]
    and edx, [SHA_A+8]
    xor ecx, edx
    mov edx, [SHA_A+4]
    and edx, [SHA_A+8]
    xor ecx, edx

    add eax, ecx
    mov [SHA_T2], eax               ; T2

    ; 变量更新
    mov eax, [SHA_A+24]
    mov [SHA_A+28], eax             ; h = g
    mov eax, [SHA_A+20]
    mov [SHA_A+24], eax             ; g = f
    mov eax, [SHA_A+16]
    mov [SHA_A+20], eax             ; f = e
    mov eax, [SHA_A+12]
    add eax, [SHA_T1]
    mov [SHA_A+16], eax             ; e = d + T1
    mov eax, [SHA_A+8]
    mov [SHA_A+12], eax             ; d = c
    mov eax, [SHA_A+4]
    mov [SHA_A+8], eax              ; c = b
    mov eax, [SHA_A+0]
    mov [SHA_A+4], eax              ; b = a
    mov eax, [SHA_T1]
    add eax, [SHA_T2]
    mov [SHA_A+0], eax              ; a = T1 + T2

    inc ebx
    cmp ebx, 64
    jb .round

    ; H += a..h
    mov eax, [SHA_A+0]
    add [SHA_H+0], eax
    mov eax, [SHA_A+4]
    add [SHA_H+4], eax
    mov eax, [SHA_A+8]
    add [SHA_H+8], eax
    mov eax, [SHA_A+12]
    add [SHA_H+12], eax
    mov eax, [SHA_A+16]
    add [SHA_H+16], eax
    mov eax, [SHA_A+20]
    add [SHA_H+20], eax
    mov eax, [SHA_A+24]
    add [SHA_H+24], eax
    mov eax, [SHA_A+28]
    add [SHA_H+28], eax

    popa
    ret

; ---------------- 常量 ----------------
h0 dd 0x6a09e667
h1 dd 0xbb67ae85
h2 dd 0x3c6ef372
h3 dd 0xa54ff53a
h4 dd 0x510e527f
h5 dd 0x9b05688c
h6 dd 0x1f83d9ab
h7 dd 0x5be0cd19

K:
    dd 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5
    dd 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5
    dd 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3
    dd 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174
    dd 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc
    dd 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da
    dd 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7
    dd 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967
    dd 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13
    dd 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85
    dd 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3
    dd 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070
    dd 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5
    dd 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3
    dd 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208
    dd 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
