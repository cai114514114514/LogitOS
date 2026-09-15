BITS 16

; This wrapper makes the fixture part of the payload read by preload.  The
; loader itself ends at a fixture handoff today: no ELF parsing, mode switch,
; or kernel jump is hidden in this gate.
%define LOADER_HANDOFF mb2_fixture_entry
%include "c/boot/bios/loader.asm"

%define DEBUG_EXIT 0xf4

mb2_fixture_entry:
    cmp eax, MB2_MAGIC
    jne fixture_bad_magic
    cmp ebx, MB2_INFO
    jne fixture_bad_address
    xor ax, ax
    mov fs, ax
    mov esi, ebx
    mov eax, [fs:esi]
    mov [fixture_total], eax
    cmp eax, 16
    jb fixture_bad_total
    cmp eax, MB2_LIMIT - MB2_INFO
    ja fixture_bad_total
    add eax, ebx
    mov [fixture_end], eax
    mov byte [fixture_seen_mmap], 0
    mov byte [fixture_seen_usable], 0
    mov byte [fixture_seen_end], 0

    mov si, msg_begin
    call serial_print
    mov si, msg_total
    call serial_print
    mov eax, [fixture_total]
    call print_hex32
    call print_newline
    mov esi, ebx
    add esi, 8
    mov [fixture_cursor], esi

.tag:
    mov esi, [fixture_cursor]
    mov eax, esi
    add eax, 8
    cmp eax, [fixture_end]
    ja fixture_bad_bounds
    mov eax, [fs:esi]
    mov [fixture_tag_type], eax
    mov eax, [fs:esi + 4]
    mov [fixture_tag_size], eax
    cmp eax, 8
    jb fixture_bad_size
    mov edx, esi
    add edx, eax
    jc fixture_bad_bounds
    cmp edx, [fixture_end]
    ja fixture_bad_bounds

    mov si, msg_tag
    call serial_print
    mov eax, [fixture_tag_type]
    call print_hex32
    call print_space
    mov eax, [fixture_tag_size]
    call print_hex32
    call print_newline

    cmp dword [fixture_tag_type], 6
    je .mmap
    cmp dword [fixture_tag_type], 8
    je .framebuffer
    cmp dword [fixture_tag_type], 14
    je .acpi
    cmp dword [fixture_tag_type], 15
    je .acpi
    cmp dword [fixture_tag_type], 0
    je .end
    jmp .advance

.mmap:
    mov esi, [fixture_cursor]
    mov byte [fixture_seen_mmap], 1
    cmp dword [fixture_tag_size], 16
    jb fixture_bad_mmap
    mov ecx, [fs:esi + 8]
    cmp ecx, 24
    jb fixture_bad_mmap
    mov edi, esi
    add edi, 16
    mov ebp, esi
    add ebp, [fixture_tag_size]
.mmap_entry:
    mov eax, edi
    add eax, ecx
    cmp eax, ebp
    ja .advance
    mov si, msg_mmap
    call serial_print
    mov eax, [fs:edi]
    mov edx, [fs:edi + 4]
    call print_hex64
    call print_space
    mov eax, [fs:edi + 8]
    mov edx, [fs:edi + 12]
    call print_hex64
    call print_space
    mov eax, [fs:edi + 16]
    cmp eax, 1
    jne .not_usable
    cmp dword [fs:edi + 8], 0
    jne .mark_usable
    cmp dword [fs:edi + 12], 0
    je .not_usable
.mark_usable:
    mov byte [fixture_seen_usable], 1
.not_usable:
    call print_hex32
    call print_newline
    add edi, ecx
    jmp .mmap_entry

.acpi:
    mov esi, [fixture_cursor]
    cmp dword [fixture_tag_size], 28
    jb fixture_bad_acpi
    mov si, msg_acpi
    call serial_print
    mov esi, [fixture_cursor]
    mov edi, esi
    add edi, 17                 ; payload + OEM id offset 9
    mov cx, 6
.oem:
    mov al, [fs:edi]
    call print_hex8
    inc edi
    loop .oem
    call print_space
    mov al, [fs:esi + 23]       ; payload + revision offset 15
    call print_hex8
    call print_space
    mov eax, [fs:esi + 24]      ; payload + RSDT address offset 16
    call print_hex32
    call print_space
    cmp dword [fixture_tag_type], 15
    jne .old_xsdt
    cmp dword [fixture_tag_size], 44
    jb fixture_bad_acpi
    mov eax, [fs:esi + 32]      ; payload + XSDT address offset 24
    mov edx, [fs:esi + 36]
    jmp .print_xsdt
.old_xsdt:
    xor eax, eax
    xor edx, edx
.print_xsdt:
    call print_hex64
    call print_newline
    jmp .advance

.framebuffer:
    mov esi, [fixture_cursor]
    cmp dword [fixture_tag_size], 32
    jb fixture_bad_framebuffer
    mov si, msg_fb
    call serial_print
    mov esi, [fixture_cursor]
    mov eax, [fs:esi + 8]
    mov edx, [fs:esi + 12]
    call print_hex64
    call print_space
    mov eax, [fs:esi + 16]
    call print_hex32
    call print_space
    mov eax, [fs:esi + 20]
    call print_hex32
    call print_space
    mov eax, [fs:esi + 24]
    call print_hex32
    call print_space
    mov al, [fs:esi + 28]
    call print_hex8
    call print_space
    mov al, [fs:esi + 29]
    call print_hex8
    call print_newline
    jmp .advance

.end:
    mov esi, [fixture_cursor]
    cmp dword [fixture_tag_size], 8
    jne fixture_bad_end
    mov eax, esi
    add eax, 8
    cmp eax, [fixture_end]
    jne fixture_bad_end
    mov byte [fixture_seen_end], 1
    cmp byte [fixture_seen_mmap], 1
    jne fixture_bad_mmap
    cmp byte [fixture_seen_usable], 1
    jne fixture_bad_mmap
    mov si, msg_end
    call serial_print
    mov si, msg_pass
    call serial_print
    mov al, 0x10
    out DEBUG_EXIT, al
    jmp loader_halt

.advance:
    mov eax, [fixture_tag_size]
    add eax, 7
    and eax, 0xfffffff8
    mov esi, [fixture_cursor]
    add esi, eax
    mov [fixture_cursor], esi
    cmp esi, [fixture_end]
    jb .tag
    jmp fixture_bad_end

fixture_bad_magic:
    mov si, msg_bad_magic
    jmp fixture_fail
fixture_bad_address:
    mov si, msg_bad_address
    jmp fixture_fail
fixture_bad_total:
    mov si, msg_bad_total
    jmp fixture_fail
fixture_bad_bounds:
    mov si, msg_bad_bounds
    jmp fixture_fail
fixture_bad_size:
    mov si, msg_bad_size
    jmp fixture_fail
fixture_bad_mmap:
    mov si, msg_bad_mmap
    jmp fixture_fail
fixture_bad_acpi:
    mov si, msg_bad_acpi
    jmp fixture_fail
fixture_bad_framebuffer:
    mov si, msg_bad_framebuffer
    jmp fixture_fail
fixture_bad_end:
    mov si, msg_bad_end

fixture_fail:
    call serial_print
    mov al, 0x11
    out DEBUG_EXIT, al
    jmp loader_halt

print_hex64:
    push eax
    mov eax, edx
    call print_hex32
    pop eax
    call print_hex32
    ret

print_hex32:
    push eax
    push bx
    push cx
    mov cx, 8
.nibble:
    rol eax, 4
    mov bl, al
    and bl, 0x0f
    cmp bl, 10
    jb .digit
    add bl, 'A' - 10
    jmp .put
.digit:
    add bl, '0'
.put:
    push ax
    mov al, bl
    call serial_putc
    pop ax
    loop .nibble
    pop cx
    pop bx
    pop eax
    ret

print_hex8:
    push ax
    push bx
    mov bl, al
    shr al, 4
    call print_nibble
    mov al, bl
    and al, 0x0f
    call print_nibble
    pop bx
    pop ax
    ret

print_nibble:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp serial_putc
.digit:
    add al, '0'
    jmp serial_putc

print_space:
    push ax
    mov al, ' '
    call serial_putc
    pop ax
    ret

print_newline:
    push ax
    mov al, 13
    call serial_putc
    mov al, 10
    call serial_putc
    pop ax
    ret

fixture_total: dd 0
fixture_end: dd 0
fixture_cursor: dd 0
fixture_tag_type: dd 0
fixture_tag_size: dd 0
fixture_seen_mmap: db 0
fixture_seen_usable: db 0
fixture_seen_end: db 0

msg_begin: db 'MB2 BEGIN', 13, 10, 0
msg_total: db 'MB2 TOTAL ', 0
msg_tag: db 'MB2 TAG ', 0
msg_mmap: db 'MB2 MMAP ', 0
msg_acpi: db 'MB2 ACPI ', 0
msg_fb: db 'MB2 FB ', 0
msg_end: db 'MB2 END', 13, 10, 0
msg_pass: db 'MB2 RESULT PASS', 13, 10, 0
msg_bad_magic: db 'MB2 RESULT FAIL magic', 13, 10, 0
msg_bad_address: db 'MB2 RESULT FAIL address', 13, 10, 0
msg_bad_total: db 'MB2 RESULT FAIL total', 13, 10, 0
msg_bad_bounds: db 'MB2 RESULT FAIL bounds', 13, 10, 0
msg_bad_size: db 'MB2 RESULT FAIL zero-or-small-size', 13, 10, 0
msg_bad_mmap: db 'MB2 RESULT FAIL memory-map', 13, 10, 0
msg_bad_acpi: db 'MB2 RESULT FAIL acpi', 13, 10, 0
msg_bad_framebuffer: db 'MB2 RESULT FAIL framebuffer', 13, 10, 0
msg_bad_end: db 'MB2 RESULT FAIL end-not-last', 13, 10, 0
