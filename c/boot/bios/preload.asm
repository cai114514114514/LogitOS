BITS 16
ORG 0x7c00

%define COM1                    0x3f8
%define LOADER_SEGMENT          0x1000
%define LOADER_PATCH_OFFSET     0x1f0

start:
    ; El Torito permits either 0000:7c00 or 07c0:0000 as the entry address.
    ; Normalising CS makes every absolute label below mean the ORG 0x7c00
    ; address NASM emitted instead of silently depending on one BIOS choice.
    jmp 0x0000:entry

entry:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    cld
    mov [boot_drive], dl

    call serial_init
    mov si, preload_marker
    call serial_print

%ifdef PRELOAD_NEGCTL_WRONG_DRIVE
    ; Gate-only mutation: drive zero is not the El Torito CD handed to us in
    ; DL.  If loader still speaks, the positive path did not actually depend
    ; on preserving the firmware's drive number.
    mov byte [boot_drive], 0
%endif

%ifndef PRELOAD_NEGCTL_BAD_DAP
    mov dl, [boot_drive]
    mov bx, 0x55aa
    mov ah, 0x41
    int 0x13
    jc extensions_missing
    cmp bx, 0xaa55
    jne extensions_missing
    test cx, 1
    jz extensions_missing
%endif

    ; A CD's AH=42h LBA and count are 2,048-byte native blocks, not the
    ; 512-byte units used only by the El Torito catalog preload.  mkiso.py
    ; patches native-CD values below; the gate places controlled bytes at that
    ; LBA and proves that multiplying it by 512 reaches different bytes.
%ifdef PRELOAD_NEGCTL_BAD_DAP
    ; The first version of this control used only DAP size 0x0f and reserved
    ; byte 0xa5 below.  SeaBIOS accepted both and reached loader (measured
    ; 2026-09-15), so they are not a control on this firmware.  A zero block
    ; count is also invalid and, unlike those ignored header fields, leaves the
    ; destination unread; retaining all three records what SeaBIOS did ignore.
    xor ax, ax
%else
    mov ax, [loader_block_count]
%endif
    mov [dap_block_count], ax
    mov eax, [loader_lba]
    mov [dap_lba], eax
    xor eax, eax
    mov [dap_lba + 4], eax

    mov dl, [boot_drive]
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc read_failed

    jmp LOADER_SEGMENT:0

extensions_missing:
    mov si, no_extensions_message
    call serial_print
    jmp halt

read_failed:
    mov si, read_failed_message
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
    mov al, 3                   ; 38,400 baud from the 115,200 Hz divisor
    out dx, al
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 3                   ; 8 data bits, no parity, one stop bit
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

boot_drive: db 0

align 4
dap:
%ifdef PRELOAD_NEGCTL_BAD_DAP
    ; Gate-only mutation paired with removing AH=41h above.  These header
    ; violations are deliberately retained beside the measured correction at
    ; the count assignment: SeaBIOS ignores them, while the zero count is what
    ; makes this malformed DAP unable to load the fixture.
    db 0x0f, 0xa5
%else
    db 0x10, 0
%endif
dap_block_count: dw 0
    dw 0                         ; destination offset
    dw LOADER_SEGMENT            ; destination segment (physical 0x10000)
dap_lba: dq 0

preload_marker:          db 'LOGIT_BIOS_PRELOAD_OK', 13, 10, 0
no_extensions_message: db 'LOGIT BIOS: INT13 extensions unavailable', 13, 10, 0
read_failed_message:   db 'LOGIT BIOS: loader read failed', 13, 10, 0

; mkiso.py is the authority for this ten-byte little-endian wire format:
; "L2P!", uint32 native-CD LBA, uint16 native-CD block count.  This mirror is
; deliberately fixed at 0x1f0; mkiso refuses a missing, moved, or duplicate
; magic rather than guessing.  Keeping preload exactly one 2,048-byte CD sector
; is a design boundary, not an MBR limit: the split keeps the BIOS catalog
; preload fixed while later loader work grows behind an explicit AH=42h read.
times LOADER_PATCH_OFFSET - ($ - $$) db 0
loader_patch_magic: db 'L2P!'
loader_lba: dd 0
loader_block_count: dw 0

; El Torito does not inspect an MBR signature.  In particular, offsets 510-511
; are intentionally zero, matching the measured GRUB no-emulation image.
times 2048 - ($ - $$) db 0
