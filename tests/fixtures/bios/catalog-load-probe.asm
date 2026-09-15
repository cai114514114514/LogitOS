BITS 16
ORG 0x7c00

%ifndef PROBE_SECTORS
%error "PROBE_SECTORS must name the requested 512-byte catalog count"
%endif

%define COM1 0x3f8
%define SENTINEL_BYTES 8
%define SENTINEL_PHYS (0x7c00 + PROBE_SECTORS * 512 - SENTINEL_BYTES)

start:
    jmp 0x0000:entry

entry:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7000
    sti
    cld
    call serial_init

    ; Asking the catalog for a large preload and checking the final controlled
    ; eight bytes measures bytes delivered, not merely a catalog field that
    ; SeaBIOS might clamp.  The gate currently asks for 1,152 sectors: 576 KiB
    ; ending at physical 0x97c00, below VGA/ROM space and the usual EBDA.
    mov ax, SENTINEL_PHYS >> 4
    mov es, ax
    mov di, SENTINEL_PHYS & 0x0f
    mov si, sentinel_reference
    mov cx, SENTINEL_BYTES
.compare:
    mov al, [si]
    cmp al, [es:di]
    jne .miss
    inc si
    inc di
    loop .compare

    mov si, loaded_marker
    call serial_print
    jmp halt

.miss:
    mov si, missed_marker
    call serial_print

halt:
    cli
.again:
    hlt
    jmp .again

serial_init:
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x80
    out dx, al
    mov dx, COM1
    mov al, 3
    out dx, al
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 3
    out dx, al
    mov dx, COM1 + 2
    mov al, 0xc7
    out dx, al
    mov dx, COM1 + 4
    mov al, 0x0b
    out dx, al
    ret

serial_print:
    lodsb
    test al, al
    jz .done
    call serial_putc
    jmp serial_print
.done:
    ret

serial_putc:
    push ax
.wait:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, COM1
    out dx, al
    ret

sentinel_reference: db 'L1152END'
loaded_marker: db 'LOGIT_BIOS_CATALOG_1152_OK', 13, 10, 0
missed_marker: db 'LOGIT_BIOS_CATALOG_1152_MISS', 13, 10, 0

times PROBE_SECTORS * 512 - SENTINEL_BYTES - ($ - $$) db 0
db 'L1152END'

