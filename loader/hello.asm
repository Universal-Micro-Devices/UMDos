; ============================================================
; hello.asm - 第四级：启动动画 + 四项初始化任务 (LBA 18..29, 0xA000)
;   任务1 网络: 检测 PXE 是否存在（如实报告，不伪造 IP）
;   任务2 CPU : CPUID 取厂商字符串
;   任务3 时间: CMOS RTC 读时分秒
;   任务4 图形: 渲染色块
;   然后读 LBA 30..45 (16 扇区) 到 0xC000 进入 FirstMain
; ============================================================
[BITS 16]
[ORG 0xA000]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8000
    sti
    cld
    mov [drive_num], dl
    call disk_init
    call vga_init
    call draw_ui

    ; ---- 任务 1：网络 ----
    mov si, s_t1
    call status
    call task_net
    call step

    ; ---- 任务 2：CPU ----
    mov si, s_t2
    call status
    call task_cpu
    call step

    ; ---- 任务 3：时间 ----
    mov si, s_t3
    call status
    call task_time
    call step

    ; ---- 任务 4：色块 ----
    mov si, s_t4
    call status
    call task_blocks
    call step

    mov si, s_done
    call status
    call wait_1s

    ; ---- 进入 FirstMain ----
    mov ax, 30
    mov cx, 16
    mov bx, 0xC000
    call read_sectors
    jc disk_error
    mov dl, [drive_num]
    jmp 0x0000:0xC000

; ---------------- 界面 ----------------
draw_ui:
    mov si, s_title
    mov bx, 36
    mov cx, 14
    mov al, 15
    call draw_string

    ; 进度条外框
    mov bx, 36
    mov cx, 152
    mov si, 248
    mov di, 18
    mov al, 15
    call draw_frame
    ; 进度条底色
    mov bx, 38
    mov cx, 154
    mov si, 244
    mov di, 14
    mov al, 8
    call draw_rect
    ret

status:
    pusha
    ; 清状态行
    mov bx, 36
    mov cx, 132
    mov si, 250
    mov di, 9
    mov al, 1
    call draw_rect
    popa
    mov bx, 36
    mov cx, 132
    mov al, 10
    call draw_string
    ret

step:
    pusha
    add byte [prog], 25
    movzx ax, byte [prog]
    mov bx, 244
    mul bx
    mov bx, 100
    div bx
    mov si, ax
    test si, si
    jz .done
    mov bx, 38
    mov cx, 154
    mov di, 14
    mov al, 42
    call draw_rect
.done:
    popa
    ret

; ---------------- 任务 1：PXE ----------------
task_net:
    mov ax, 0x5650
    mov bx, 0x5845
    int 0x1A
    jc .no
    mov si, s_net_yes
    jmp .show
.no:
    mov si, s_net_no
.show:
    mov bx, 36
    mov cx, 80
    mov al, 11
    call draw_string
    ret

; ---------------- 任务 2：CPU 厂商 ----------------
task_cpu:
    mov si, s_cpu
    mov bx, 36
    mov cx, 96
    mov al, 14
    call draw_string

    xor eax, eax
    cpuid
    mov [cpu_str+0], ebx
    mov [cpu_str+4], edx
    mov [cpu_str+8], ecx
    mov byte [cpu_str+12], 0

    mov si, cpu_str
    mov bx, 76
    mov cx, 96
    mov al, 15
    call draw_string
    ret

; ---------------- 任务 3：RTC ----------------
task_time:
    mov si, s_time
    mov bx, 36
    mov cx, 112
    mov al, 14
    call draw_string

    call read_rtc
    mov di, time_str
    mov al, [rtc_h]
    call two_digits
    add di, 2
    mov byte [di], ':'
    inc di
    mov al, [rtc_m]
    call two_digits
    add di, 2
    mov byte [di], ':'
    inc di
    mov al, [rtc_s]
    call two_digits

    mov si, time_str
    mov bx, 76
    mov cx, 112
    mov al, 15
    call draw_string
    ret

; AL=数值(0..99), DI=输出, 写 2 位十进制, DI 前进 2
two_digits:
    pusha
    mov ah, 0
    mov bl, 10
    div bl
    add al, '0'
    add ah, '0'
    mov [di], al
    mov [di+1], ah
    popa
    ret

read_rtc:
    ; 等 UIP 清零
    mov cx, 0x2000
.wait:
    mov al, 0x0A
    out 0x70, al
    in al, 0x71
    test al, 0x80
    jz .ready
    loop .wait
.ready:
    ; 是否 BCD
    mov al, 0x0B
    out 0x70, al
    in al, 0x71
    and al, 0x04
    mov [rtc_bin], al

    mov al, 0x00
    out 0x70, al
    in al, 0x71
    call rtc_conv
    mov [rtc_s], al

    mov al, 0x02
    out 0x70, al
    in al, 0x71
    call rtc_conv
    mov [rtc_m], al

    mov al, 0x04
    out 0x70, al
    in al, 0x71
    call rtc_conv
    mov [rtc_h], al
    ret

; BCD -> 二进制（若 RTC 已是二进制则原样返回）
rtc_conv:
    cmp byte [rtc_bin], 0
    jne .ret
    push bx
    mov bl, al
    shr al, 4
    mov bh, al
    and bl, 0x0F
    mov al, bh
    mov ah, 10
    mul ah
    add al, bl
    pop bx
.ret:
    ret

; ---------------- 任务 4：色块 ----------------
task_blocks:
    mov bx, 36
    mov cx, 180
    mov si, 24
    mov di, 10
    mov al, 40
    call draw_rect
    mov bx, 66
    mov al, 44
    call draw_rect
    mov bx, 96
    mov al, 47
    call draw_rect
    mov bx, 126
    mov al, 41
    call draw_rect
    mov bx, 156
    mov al, 45
    call draw_rect
    ret

; ---------------- 等待约 1 秒 ----------------
wait_1s:
    pusha
    mov cx, 2
.l:
    push cx
    mov cx, 0x0007
    mov dx, 0xA120              ; 500000 us
    mov ah, 0x86
    int 0x15
    pop cx
    loop .l
    popa
    ret

; ---------------- 数据 ----------------
drive_num  db 0
prog       db 0
rtc_bin    db 0
rtc_s      db 0
rtc_m      db 0
rtc_h      db 0
cpu_str    times 13 db 0
time_str   times 9 db 0

s_title   db 'Hello and Loading...', 0
s_cpu     db 'CPU: ', 0
s_time    db 'Time: ', 0
s_net_yes db 'Net: PXE detected', 0
s_net_no  db 'Net: no PXE (offline mode)', 0
s_t1      db 'Task 1/4: probing network...', 0
s_t2      db 'Task 2/4: reading CPU id...', 0
s_t3      db 'Task 3/4: reading RTC...', 0
s_t4      db 'Task 4/4: drawing color blocks...', 0
s_done    db 'All tasks done. Starting FirstMain', 0

; 公共库放在入口代码之后，保证 start 位于映像偏移 0
%include "io16.inc"
%include "disk16.inc"
%include "vga8.inc"

times (12 * 512) - ($ - $$) db 0
