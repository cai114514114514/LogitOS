; This is deliberately a disposable catalog fixture, not LogitOS stage1.  Its
; only contract is to prove that BIOS accepted the El Torito catalog, loaded
; four 512-byte sectors at physical 0x7C00, and transferred control here.
; Keeping it below 512 bytes also guards the measured fact that mkiso pads the
; remaining loaded sectors; there is deliberately no MBR-style 0xAA55 trailer.
BITS 16
; SeaBIOS reports the measured catalog's 07C0 load segment as a transfer to
; 0000:7C00.  Absolute data addresses therefore use physical 0x7C00 rather
; than assuming firmware preserves 07C0 in CS; both segment:offset forms name
; the same code bytes, but only the former makes DS:marker unambiguous.
ORG 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00

    mov dx, 0x3FB                 ; COM1 line-control register
    mov al, 0x80                  ; expose divisor latches
    out dx, al
    mov dx, 0x3F8
    mov al, 1                     ; 115200 baud
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03                  ; 8 data bits, no parity, one stop bit
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7                  ; enable and clear FIFOs
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B                  ; IRQs enabled, RTS/DSR asserted
    out dx, al

    mov si, marker
.next:
    lodsb
    test al, al
    jz .halt
    mov bl, al
.ready:
    mov dx, 0x3FD                 ; transmitter-holding register empty?
    in al, dx
    test al, 0x20
    jz .ready
    mov dx, 0x3F8
    mov al, bl
    out dx, al
    jmp .next

.halt:
    hlt
    jmp .halt

marker: db 'LOGIT_MKISO_FIXTURE_OK', 13, 10, 0
