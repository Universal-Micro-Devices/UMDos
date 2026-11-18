; ============================================================
; serial16.inc - COM1 串口输出（16 位实模式）
;   提供 serial_init / serial_putc / serial_puts / serial_hex8
;   阶段串口日志用，波特率 115200 8N1（分频 1）
; ============================================================

serial_init:
    push ax
    push dx
    mov dx, 0x3F9               ; IER = 0
    xor al, al
    out dx, al
    mov dx, 0x3FB               ; LCR: DLAB=1
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8               ; 分频低字节 = 1
    mov al, 1
    out dx, al
    mov dx, 0x3F9               ; 分频高字节 = 0
    xor al, al
    out dx, al
    mov dx, 0x3FB               ; 8 位、无校验、1 停止位
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA               ; FIFO 使能 + 清空
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC               ; DTR/RTS
    mov al, 0x0B
    out dx, al
    pop dx
    pop ax
    ret

; AL = 字符
serial_putc:
    push ax
    push dx
    mov ah, al
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20               ; THR empty
    jz .wait
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop dx
    pop ax
    ret

; SI = 以 0 结尾的字符串
serial_puts:
    push ax
    push si
.next:
    lodsb
    test al, al
    jz .done
    call serial_putc
    jmp .next
.done:
    pop si
    pop ax
    ret
