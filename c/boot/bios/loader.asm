BITS 16
ORG 0

%define COM1                 0x3f8
%define MB2_MAGIC            0x36d76289
%define MMAP_BUFFER          0x5000
%define MMAP_ENTRY_BYTES     24
%define MMAP_MAX_ENTRIES     128
%define VBE_INFO             0x6000
%define VBE_MODE_INFO        0x6200
%define MB2_INFO             0x8000
%define MB2_LIMIT            0xa000

%ifndef LOADER_HANDOFF
%define LOADER_HANDOFF loader_halt
%endif

loader_entry:
    cli
    mov ax, cs
    mov ds, ax
    xor ax, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    cld

    call serial_init
    mov si, loader_marker
    call serial_print

    ; Do not conflate an enable request with an enabled address line.  Every
    ; method below is followed by the alias test because several BIOSes return
    ; success from AX=2401 while leaving A20 unchanged; the silent wrap would
    ; corrupt a later 1 MiB copy and only surface thousands of instructions
    ; after this code ran.
    call a20_is_enabled
    mov [a20_was_enabled], al
%ifdef LOADER_NEGCTL_SKIP_A20
    test al, al
    jnz .a20_pre_enabled_control
    jmp a20_failed
.a20_pre_enabled_control:
    mov si, a20_control_unavailable
    call serial_print
    jmp .a20_ready
%else
    test al, al
    jnz .a20_ready
    mov ax, 0x2401
    int 0x15
    call a20_is_enabled
    test al, al
    jnz .a20_ready
    call a20_keyboard_controller
    call a20_is_enabled
    test al, al
    jnz .a20_ready
    in al, 0x92
    or al, 2
    and al, 0xfe
    out 0x92, al
    call a20_is_enabled
    test al, al
    jz a20_failed
%endif
.a20_ready:
    mov si, a20_ok
    call serial_print

    call collect_e820
    jc e820_failed
    call find_rsdp
    call probe_vbe
    call build_mb2
    jc mb2_failed

    mov eax, MB2_MAGIC
    mov ebx, MB2_INFO
    jmp LOADER_HANDOFF

a20_failed:
    mov si, a20_fail
    call serial_print
    jmp loader_halt

e820_failed:
    mov si, e820_fail
    call serial_print
    jmp loader_halt

mb2_failed:
    mov si, mb2_fail
    call serial_print

loader_halt:
    cli
.halt:
    hlt
    jmp .halt

; Return AL=1 only after writing physical 0x100000 and observing that its
; 1 MiB alias at zero did not change.  Interrupts stay off while IVT bytes zero
; through three are temporarily exposed to the test value.
a20_is_enabled:
    pushf
    cli
    push ds
    push es
    push bx
    push cx
    push dx
    xor ax, ax
    mov ds, ax
    mov ax, 0xffff
    mov es, ax
    mov ebx, [ds:0]
    mov ecx, [es:0x10]
    mov edx, ebx
    not edx
    mov [es:0x10], edx
    cmp [ds:0], ebx
    setz al
    mov [es:0x10], ecx
    mov [ds:0], ebx
    pop dx
    pop cx
    pop bx
    pop es
    pop ds
    popf
    ret

; The 8042 fallback has bounded waits.  A missing keyboard controller must
; lead to the port-92 fallback, not turn an optional method into an infinite
; boot hang.
a20_keyboard_controller:
    push ax
    push bx
    call kbc_wait_input_clear
    jc .done
    mov al, 0xad
    out 0x64, al
    call kbc_wait_input_clear
    jc .reenable
    mov al, 0xd0
    out 0x64, al
    call kbc_wait_output_full
    jc .reenable
    in al, 0x60
    mov bl, al
    or bl, 2
    call kbc_wait_input_clear
    jc .reenable
    mov al, 0xd1
    out 0x64, al
    call kbc_wait_input_clear
    jc .reenable
    mov al, bl
    out 0x60, al
.reenable:
    call kbc_wait_input_clear
    jc .done
    mov al, 0xae
    out 0x64, al
.done:
    pop bx
    pop ax
    ret

kbc_wait_input_clear:
    mov cx, 0xffff
.again:
    in al, 0x64
    test al, 2
    jz .ok
    loop .again
    stc
    ret
.ok:
    clc
    ret

kbc_wait_output_full:
    mov cx, 0xffff
.again:
    in al, 0x64
    test al, 1
    jnz .ok
    loop .again
    stc
    ret
.ok:
    clc
    ret

; Preserve the BIOS map verbatim and in firmware order.  In particular, zero
; length and reserved entries are not filtered: the PMM owns interpretation of
; type, and editing the map here would create a lie it cannot detect later.
collect_e820:
    xor ax, ax
    mov es, ax
    mov di, MMAP_BUFFER
    xor ebx, ebx
    xor bp, bp
.next:
%ifdef LOADER_NEGCTL_TRUNCATE_E820
    cmp bp, LOADER_NEGCTL_TRUNCATE_E820
    jae .done
%endif
    cmp bp, MMAP_MAX_ENTRIES
    jae .overflow
    mov dword [es:di + 20], 1
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, MMAP_ENTRY_BYTES
    int 0x15
    jc .bios_done
    cmp eax, 0x534d4150
    jne .bad
    cmp ecx, 20
    jb .bad
    ; E820 may return only the original 20-byte structure.  MB2 entries have a
    ; fixed 24-byte stride here, so the non-firmware tail is deterministically
    ; zero rather than leaking the request attribute into the block.
    mov dword [es:di + 20], 0
    inc bp
    add di, MMAP_ENTRY_BYTES
    test ebx, ebx
    jnz .next
.done:
    test bp, bp
    jz .bad
    mov [mmap_count], bp
    clc
    ret
.bios_done:
    test bp, bp
    jnz .done
.bad:
    stc
    ret
.overflow:
    mov si, e820_overflow
    call serial_print
    stc
    ret

; Scan the first KiB of the EBDA, then the complete 0xE0000-0xFFFFF BIOS
; window on 16-byte boundaries.  A revision-2 candidate is accepted only when
; both checksums pass; emitting a plausible tag after only the legacy checksum
; is worse than omitting ACPI because acpi.c deliberately trusts no unchecked
; RSDP.
find_rsdp:
    mov word [rsdp_segment], 0
    mov word [rsdp_offset], 0
    mov word [rsdp_length], 0
    xor ax, ax
    mov es, ax
    mov ax, [es:0x040e]
    test ax, ax
    jz .high
    mov es, ax
    xor di, di
    mov cx, 64
    call scan_rsdp_range
    jnc .found
.high:
    mov ax, 0xe000
    mov es, ax
    xor di, di
    mov cx, 4096
    call scan_rsdp_range
    jnc .found
    mov ax, 0xf000
    mov es, ax
    xor di, di
    mov cx, 4096
    call scan_rsdp_range
    jc .none
.found:
    mov [rsdp_segment], es
    mov [rsdp_offset], di
    ret
.none:
    ret

scan_rsdp_range:
.candidate:
    push cx
    push di
    mov si, rsdp_signature
    mov cx, 8
.signature_byte:
    mov al, [es:di]
    cmp al, [ds:si]
    jne .not_this
    inc di
    inc si
    loop .signature_byte
    pop di
    push di
    call validate_rsdp
    jnc .valid
.not_this:
    pop di
    pop cx
    add di, 16
    loop .candidate
    stc
    ret
.valid:
    pop di
    pop cx
    clc
    ret

; ES:DI candidate.  Return CF clear and record its exact byte length.
validate_rsdp:
    push ax
    push bx
    push cx
    push dx
    push si
    mov si, di
    mov cx, 20
    xor bx, bx
.sum20:
    mov al, [es:si]
    add bl, al
    inc si
    loop .sum20
%ifdef LOADER_NEGCTL_BAD_RSDP
    ; Gate-only mutation: make a genuinely valid checksum fail at the same
    ; branch a corrupt firmware table would take.  The block builder therefore
    ; never sees this candidate and cannot accidentally emit tag 14 or 15.
    mov si, rsdp_control_rejected
    call serial_print
    inc bl
%endif
    test bl, bl
    jnz .bad
    mov al, [es:di + 15]
    cmp al, 2
    jb .legacy
    mov ecx, [es:di + 20]
    test ecx, 0xffff0000
    jnz .bad
    cmp cx, 36
    jb .bad
    cmp cx, 256
    ja .bad
    mov dx, di
    add dx, cx
    jc .bad
    mov si, di
    xor bx, bx
.sum_extended:
    mov al, [es:si]
    add bl, al
    inc si
    loop .sum_extended
    test bl, bl
    jnz .bad
    mov cx, [es:di + 20]
    jmp .accepted
.legacy:
    mov cx, 20
.accepted:
    mov [rsdp_length], cx
    clc
    jmp .out
.bad:
    stc
.out:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Select exactly the optional 1024x768x32 direct-colour linear mode requested
; by the existing MB2 header.  If VBE is absent (the shipping -vga none path),
; unsupported, or refuses the mode, vbe_present remains zero and tag 8 is
; absent.  A text-mode address must never be dressed up as a framebuffer tag.
probe_vbe:
    mov byte [vbe_present], 0
    xor ax, ax
    mov es, ax
    mov dword [es:VBE_INFO], 'VBE2'
    mov ax, 0x4f00
    mov di, VBE_INFO
    int 0x10
    cmp ax, 0x004f
    jne .none
    cmp dword [es:VBE_INFO], 0x41534556 ; little-endian bytes "VESA"
    jne .none
    mov si, [es:VBE_INFO + 14]
    mov ax, [es:VBE_INFO + 16]
    mov fs, ax
    mov cx, 256
.mode:
    mov bx, [fs:si]
    add si, 2
    cmp bx, 0xffff
    je .none
    push cx
    push si
    push bx
    mov cx, bx
    mov ax, 0x4f01
    mov di, VBE_MODE_INFO
    int 0x10
    pop bx
    pop si
    pop cx
    cmp ax, 0x004f
    jne .next
    mov ax, [es:VBE_MODE_INFO]
    and ax, 0x0091             ; supported, graphics, linear-framebuffer
    cmp ax, 0x0091
    jne .next
    cmp word [es:VBE_MODE_INFO + 18], 1024
    jne .next
    cmp word [es:VBE_MODE_INFO + 20], 768
    jne .next
    cmp byte [es:VBE_MODE_INFO + 25], 32
    jne .next
    cmp byte [es:VBE_MODE_INFO + 27], 6
    jne .next
    push bx
    or bx, 0x4000
    mov ax, 0x4f02
    int 0x10
    pop bx
    cmp ax, 0x004f
    jne .none
    mov ax, 0x4f01
    mov cx, bx
    mov di, VBE_MODE_INFO
    int 0x10
    cmp ax, 0x004f
    jne .none
    mov eax, [es:VBE_MODE_INFO + 40]
    mov [vbe_addr], eax
    mov ax, [es:VBE_MODE_INFO + 16]
    mov [vbe_pitch], ax
    mov ax, [es:VBE_MODE_INFO + 18]
    mov [vbe_width], ax
    mov ax, [es:VBE_MODE_INFO + 20]
    mov [vbe_height], ax
    mov al, [es:VBE_MODE_INFO + 25]
    mov [vbe_bpp], al
    mov al, [es:VBE_MODE_INFO + 31]
    mov [vbe_red_size], al
    mov al, [es:VBE_MODE_INFO + 32]
    mov [vbe_red_pos], al
    mov al, [es:VBE_MODE_INFO + 33]
    mov [vbe_green_size], al
    mov al, [es:VBE_MODE_INFO + 34]
    mov [vbe_green_pos], al
    mov al, [es:VBE_MODE_INFO + 35]
    mov [vbe_blue_size], al
    mov al, [es:VBE_MODE_INFO + 36]
    mov [vbe_blue_pos], al
    mov byte [vbe_present], 1
.none:
    mov ax, cs
    mov ds, ax
    xor ax, ax
    mov es, ax
    ret
.next:
    dec cx
    jnz near .mode
    jmp .none

build_mb2:
    xor ax, ax
    mov es, ax
    mov di, MB2_INFO
    mov dword [es:di], 0
    mov dword [es:di + 4], 0
    add di, 8

    ; Memory-map tag: 16-byte header followed by the firmware entries exactly
    ; as returned.  The chosen 128-entry ceiling plus ACPI/VBE tags fits within
    ; the explicit MB2_LIMIT; overflow is fatal instead of silently truncating.
    mov dword [es:di], 6
    movzx eax, word [mmap_count]
    imul eax, MMAP_ENTRY_BYTES
    add eax, 16
    mov [es:di + 4], eax
    mov dword [es:di + 8], MMAP_ENTRY_BYTES
    mov dword [es:di + 12], 0
    add di, 16
    mov si, MMAP_BUFFER
    mov cx, [mmap_count]
.copy_mmap_entry:
    push cx
    mov cx, MMAP_ENTRY_BYTES
.copy_mmap_byte:
    mov al, [es:si]
    mov [es:di], al
    inc si
    inc di
    loop .copy_mmap_byte
    pop cx
    loop .copy_mmap_entry

    cmp word [rsdp_segment], 0
    je .maybe_framebuffer
    mov ax, [rsdp_segment]
    mov fs, ax
    mov si, [rsdp_offset]
    mov al, [fs:si + 15]
    cmp al, 2
    jb .old_acpi
    mov dword [es:di], 15
    jmp .acpi_header
.old_acpi:
    mov dword [es:di], 14
.acpi_header:
    movzx eax, word [rsdp_length]
    add eax, 8
    mov [es:di + 4], eax
    add di, 8
    mov cx, [rsdp_length]
.copy_rsdp:
    mov al, [fs:si]
    mov [es:di], al
    inc si
    inc di
    loop .copy_rsdp
    call align_di_8

.maybe_framebuffer:
    cmp byte [vbe_present], 1
    jne .end_tag
    mov dword [es:di], 8
    mov dword [es:di + 4], 38
    mov eax, [vbe_addr]
    mov [es:di + 8], eax
    mov dword [es:di + 12], 0
    movzx eax, word [vbe_pitch]
    mov [es:di + 16], eax
    movzx eax, word [vbe_width]
    mov [es:di + 20], eax
    movzx eax, word [vbe_height]
    mov [es:di + 24], eax
    mov al, [vbe_bpp]
    mov [es:di + 28], al
    mov byte [es:di + 29], 1
    mov word [es:di + 30], 0
    mov al, [vbe_red_pos]
    mov [es:di + 32], al
    mov al, [vbe_red_size]
    mov [es:di + 33], al
    mov al, [vbe_green_pos]
    mov [es:di + 34], al
    mov al, [vbe_green_size]
    mov [es:di + 35], al
    mov al, [vbe_blue_pos]
    mov [es:di + 36], al
    mov al, [vbe_blue_size]
    mov [es:di + 37], al
    add di, 38
    call align_di_8

.end_tag:
    cmp di, MB2_LIMIT - 8
    ja .overflow
    mov dword [es:di], 0
    mov dword [es:di + 4], 8
    add di, 8
    movzx eax, di
    sub eax, MB2_INFO
    mov [es:MB2_INFO], eax
    clc
    ret
.overflow:
    stc
    ret

align_di_8:
    push bx
    mov bx, di
    add bx, 7
    and bx, 0xfff8
.zero_padding:
    cmp di, bx
    jae .done
    mov byte [es:di], 0
    inc di
    jmp .zero_padding
.done:
    pop bx
    ret

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

a20_was_enabled: db 0
mmap_count: dw 0
rsdp_segment: dw 0
rsdp_offset: dw 0
rsdp_length: dw 0
vbe_present: db 0
vbe_addr: dd 0
vbe_pitch: dw 0
vbe_width: dw 0
vbe_height: dw 0
vbe_bpp: db 0
vbe_red_pos: db 0
vbe_red_size: db 0
vbe_green_pos: db 0
vbe_green_size: db 0
vbe_blue_pos: db 0
vbe_blue_size: db 0

rsdp_signature: db 'RSD PTR '
loader_marker: db 'LOGIT_BIOS_LOADER_MB2', 13, 10, 0
a20_ok: db 'LOADER A20 VERIFY PASS', 13, 10, 0
a20_fail: db 'LOADER A20 VERIFY FAIL', 13, 10, 0
a20_control_unavailable: db 'LOADER CONTROL A20 PRE_ENABLED', 13, 10, 0
e820_fail: db 'LOADER E820 FAIL', 13, 10, 0
e820_overflow: db 'LOADER E820 OVERFLOW', 13, 10, 0
rsdp_control_rejected: db 'LOADER CONTROL RSDP CHECKSUM REJECTED', 13, 10, 0
mb2_fail: db 'LOADER MB2 OVERFLOW', 13, 10, 0
