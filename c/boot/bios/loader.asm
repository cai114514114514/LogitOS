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
%define PHDR_BUFFER          0x3000
%define PHDR_BUFFER_BYTES    (MMAP_BUFFER - PHDR_BUFFER)
%define BOUNCE_SEGMENT       0x7000
%define BOUNCE_BUFFER        0x70000
%define BOUNCE_BYTES         0x8000
%define KERNEL_LOAD_BASE     0x02000000
%define LOADER_PHYSICAL      0x10000
%define KERNEL_PATCH_OFFSET  0x1ff0

%define GDT_CODE32           0x08
%define GDT_DATA32           0x10
%define GDT_CODE16           0x18
%define GDT_DATA16           0x20

%ifndef LOADER_HANDOFF
%define LOADER_HANDOFF load_kernel_and_enter
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
    mov [boot_drive], dl

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

; The ELF64 header and program-header table are data even though the entry
; contract is 32-bit protected mode.  Read the table from the CD instead of
; baking in either the linked entry or segment layout: linker changes must
; move the bytes described by PT_LOAD, not require a matching loader edit.
load_kernel_and_enter:
    call read_kernel_headers
    jc kernel_load_failed

    mov word [phdr_cursor], PHDR_BUFFER
    mov ax, [kernel_phnum]
    mov [phdr_left], ax
    mov byte [loaded_segment_count], 0
%ifdef LOADER_NEGCTL_SHORT_SEGMENT
    mov byte [short_segment_applied], 0
%endif

.next_phdr:
    cmp word [phdr_left], 0
    je .segments_done
    xor ax, ax
    mov es, ax
    mov bx, [phdr_cursor]
    cmp dword [es:bx], 1                  ; ELF PT_LOAD
    jne .advance

    cmp dword [es:bx + 12], 0            ; p_offset high half
    jne kernel_elf_failed
    cmp dword [es:bx + 28], 0            ; p_paddr high half
    jne kernel_elf_failed
    cmp dword [es:bx + 36], 0            ; p_filesz high half
    jne kernel_elf_failed
    cmp dword [es:bx + 44], 0            ; p_memsz high half
    jne kernel_elf_failed
    mov eax, [es:bx + 8]
    mov [segment_offset], eax
    mov eax, [es:bx + 24]
    mov [segment_address], eax
    mov eax, [es:bx + 32]
    mov [segment_filesz], eax
    mov eax, [es:bx + 40]
    mov [segment_memsz], eax

    cmp eax, [segment_filesz]
    jb kernel_elf_failed
    mov edx, [segment_address]
    cmp edx, KERNEL_LOAD_BASE
    jb kernel_elf_failed
    add edx, eax
    jc kernel_elf_failed
    mov eax, [segment_offset]
    add eax, [segment_filesz]
    jc kernel_elf_failed
    cmp eax, [kernel_file_limit]
    ja kernel_elf_failed

    inc byte [loaded_segment_count]
    mov eax, [segment_filesz]
    mov [segment_copy_bytes], eax
%ifdef LOADER_NEGCTL_SHORT_SEGMENT
    ; Gate-only mutation: damage exactly one loadable segment while leaving its
    ; declared BSS boundary untouched.  Zeroing must not accidentally repair
    ; the omitted file-backed page and turn this into a different control.
    cmp byte [short_segment_applied], 0
    jne .copy_segment
    cmp eax, 4096
    jb .copy_segment
    sub dword [segment_copy_bytes], 4096
    mov byte [short_segment_applied], 1
    mov si, short_segment_control
    call serial_print
%endif
.copy_segment:
    call load_segment_file
    jc kernel_read_failed

    mov eax, [segment_memsz]
    sub eax, [segment_filesz]
    jz .advance
%ifdef LOADER_NEGCTL_SKIP_BSS_ZERO
    ; Gate-only mutation.  On zero-filled QEMU RAM this can remain invisible;
    ; the oracle must report that as SKIP rather than pretending a failure.
    mov si, bss_zero_control
    call serial_print
%else
    mov [physical_count], eax
    mov edx, [segment_address]
    add edx, [segment_filesz]
    mov [physical_destination], edx
    mov byte [physical_operation], 1
    call protected_memory_operation
%endif

.advance:
    add word [phdr_cursor], 56
    dec word [phdr_left]
    jmp .next_phdr

.segments_done:
    cmp byte [loaded_segment_count], 0
    je kernel_elf_failed
%ifdef LOADER_NEGCTL_SHORT_SEGMENT
    cmp byte [short_segment_applied], 1
    jne kernel_elf_failed
%endif
    mov si, kernel_load_ok
    call serial_print
    mov si, kernel_enter_marker
    call serial_print

    ; This is the final transition, not a copy round-trip: paging and
    ; interrupts remain off, and the flat 32-bit selectors are the state the
    ; existing boot.asm entry contract requires.
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword GDT_CODE32:(LOADER_PHYSICAL + protected_kernel_entry)

kernel_load_failed:
    mov si, kernel_load_fail
    call serial_print
    jmp loader_halt

kernel_elf_failed:
    mov si, kernel_elf_fail
    call serial_print
    jmp loader_halt

kernel_read_failed:
    mov si, kernel_read_fail
    call serial_print
    jmp loader_halt

; Read and validate enough leading native-CD blocks to contain every ELF64
; program header, then copy the table away from the bounce buffer before that
; buffer is reused for segment data.  The explicit low-memory ceiling prevents
; a malformed e_phnum from overwriting the E820 apparatus at 0x5000.
read_kernel_headers:
    movzx eax, word [kernel_block_count]
    test eax, eax
    jz .bad
    shl eax, 11
    mov [kernel_file_limit], eax
    xor eax, eax
    mov cx, 1
    call read_kernel_blocks
    jc .read_bad

    mov ax, BOUNCE_SEGMENT
    mov es, ax
    cmp dword [es:0], 0x464c457f
    jne .bad
    cmp byte [es:4], 2                    ; ELFCLASS64
    jne .bad
    cmp byte [es:5], 1                    ; little endian
    jne .bad
    cmp byte [es:6], 1                    ; current ELF version
    jne .bad
    cmp word [es:18], 0x3e                ; EM_X86_64
    jne .bad
    cmp dword [es:28], 0                  ; e_entry high half
    jne .bad
    cmp dword [es:36], 0                  ; e_phoff high half
    jne .bad
    cmp word [es:54], 56                  ; sizeof(Elf64_Phdr)
    jne .bad
    cmp word [es:56], 0
    je .bad
    mov eax, [es:24]
    mov [kernel_entry_address], eax
    mov eax, [es:32]
    mov [kernel_phoff], eax
    mov ax, [es:56]
    mov [kernel_phnum], ax
    movzx eax, ax
    imul eax, 56
    cmp eax, PHDR_BUFFER_BYTES
    ja .bad
    mov [kernel_phbytes], ax
    mov edx, [kernel_phoff]
    add edx, eax
    jc .bad
    cmp edx, BOUNCE_BYTES
    ja .bad
    cmp edx, [kernel_file_limit]
    ja .bad
    mov eax, edx
    add eax, 2047
    jc .bad
    shr eax, 11
    mov cx, ax
    xor eax, eax
    call read_kernel_blocks
    jc .read_bad

    mov ax, BOUNCE_SEGMENT
    mov ds, ax
    mov si, [cs:kernel_phoff]
    xor ax, ax
    mov es, ax
    mov di, PHDR_BUFFER
    mov cx, [cs:kernel_phbytes]
    rep movsb
    mov ax, cs
    mov ds, ax
    clc
    ret
.read_bad:
    stc
    ret
.bad:
    mov ax, cs
    mov ds, ax
    stc
    ret

; Copy one PT_LOAD segment in at most 32 KiB pieces.  INT 13h executes only in
; real mode into the 0x70000 bounce buffer; protected_memory_operation then
; copies above 1 MiB and returns for the next firmware call.  A persistent
; unreal-mode cache was rejected because firmware is not required to preserve
; its hidden segment state across INT 13h, a failure that would be silent.
load_segment_file:
    mov eax, [segment_offset]
    mov [load_file_offset], eax
    mov eax, [segment_address]
    mov [load_destination], eax
    mov eax, [segment_copy_bytes]
    mov [load_remaining], eax
.chunk:
    cmp dword [load_remaining], 0
    je .done
    mov eax, [load_file_offset]
    mov edx, eax
    and edx, 2047
    mov [load_skip], dx
    mov ecx, BOUNCE_BYTES
    sub ecx, edx
    cmp ecx, [load_remaining]
    jbe .size_ready
    mov ecx, [load_remaining]
.size_ready:
    mov [load_chunk_bytes], ecx
    mov eax, edx
    add eax, ecx
    add eax, 2047
    shr eax, 11
    mov cx, ax
    mov eax, [load_file_offset]
    shr eax, 11
    call read_kernel_blocks
    jc .read_bad

    movzx eax, word [load_skip]
    add eax, BOUNCE_BUFFER
    mov [physical_source], eax
    mov eax, [load_destination]
    mov [physical_destination], eax
    mov eax, [load_chunk_bytes]
    mov [physical_count], eax
    mov byte [physical_operation], 0
    call protected_memory_operation

    mov eax, [load_chunk_bytes]
    add [load_file_offset], eax
    add [load_destination], eax
    sub [load_remaining], eax
    jmp .chunk
.done:
    clc
    ret
.read_bad:
    stc
    ret

; EAX is a sector offset relative to the patched kernel LBA; CX is a count of
; 2,048-byte native CD blocks.  The same ten-byte L2P descriptor format used by
; preload names this payload too; the legacy magic spelling is retained so the
; image has one patch mechanism rather than a second almost-identical protocol.
read_kernel_blocks:
    test cx, cx
    jz .bad
    cmp cx, BOUNCE_BYTES / 2048
    ja .bad
    mov [kernel_dap + 2], cx
    add eax, [kernel_lba]
    mov [kernel_dap + 8], eax
    mov dword [kernel_dap + 12], 0
    mov dl, [boot_drive]
    mov si, kernel_dap
    mov ah, 0x42
    int 0x13
    pushf
    mov ax, cs
    mov ds, ax
    popf
    ret
.bad:
    stc
    ret

; Copying through protected mode is slightly more transition work than unreal
; mode, but it makes the 32 MiB write independent of undocumented BIOS segment
; cache preservation.  Record the post-CALL SP: restoring the stack top instead
; silently pops address zero and restarts loader_entry after every chunk.
protected_memory_operation:
    mov [protected_return_sp], sp
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword GDT_CODE32:(LOADER_PHYSICAL + protected_memory_entry)

BITS 32
protected_memory_entry:
    mov ax, GDT_DATA32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7c00
    cld
    mov edi, [LOADER_PHYSICAL + physical_destination]
    mov ecx, [LOADER_PHYSICAL + physical_count]
    cmp byte [LOADER_PHYSICAL + physical_operation], 0
    jne .zero
    mov esi, [LOADER_PHYSICAL + physical_source]
    rep movsb
    jmp .return_real
.zero:
    xor eax, eax
    rep stosb
.return_real:
    jmp word GDT_CODE16:protected_return_16

protected_kernel_entry:
    mov ax, GDT_DATA32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7c00
    cld
    mov edx, [LOADER_PHYSICAL + kernel_entry_address]
%ifdef LOADER_NEGCTL_BAD_MB2_MAGIC
    mov eax, 0x0badc0de
%else
    mov eax, MB2_MAGIC
%endif
    mov ebx, MB2_INFO
    jmp edx

BITS 16
protected_return_16:
    mov ax, GDT_DATA16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and eax, 0xfffffffe
    mov cr0, eax
    jmp 0x1000:protected_return_real

protected_return_real:
    mov ax, cs
    mov ds, ax
    xor ax, ax
    mov es, ax
    mov ss, ax
    mov sp, [protected_return_sp]
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

boot_drive: db 0
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

kernel_file_limit: dd 0
kernel_entry_address: dd 0
kernel_phoff: dd 0
kernel_phnum: dw 0
kernel_phbytes: dw 0
phdr_cursor: dw 0
phdr_left: dw 0
loaded_segment_count: db 0
short_segment_applied: db 0
segment_offset: dd 0
segment_address: dd 0
segment_filesz: dd 0
segment_memsz: dd 0
segment_copy_bytes: dd 0
load_file_offset: dd 0
load_destination: dd 0
load_remaining: dd 0
load_chunk_bytes: dd 0
load_skip: dw 0
physical_source: dd 0
physical_destination: dd 0
physical_count: dd 0
physical_operation: db 0
protected_return_sp: dw 0

align 4
kernel_dap:
    db 0x10, 0
    dw 0
    dw 0
    dw BOUNCE_SEGMENT
    dq 0

align 8
gdt:
    dq 0
    dw 0xffff, 0x0000
    db 0x00, 0x9a, 0xcf, 0x00           ; flat 32-bit code
    dw 0xffff, 0x0000
    db 0x00, 0x92, 0xcf, 0x00           ; flat 32-bit data
    dw 0xffff, LOADER_PHYSICAL & 0xffff
    db (LOADER_PHYSICAL >> 16) & 0xff, 0x9a, 0x00, (LOADER_PHYSICAL >> 24) & 0xff
    dw 0xffff, 0x0000
    db 0x00, 0x92, 0x00, 0x00           ; 64 KiB data for PM-to-real return
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt - 1
    dd LOADER_PHYSICAL + gdt

rsdp_signature: db 'RSD PTR '
loader_marker: db 'LOGIT_BIOS_LOADER_MB2', 13, 10, 0
a20_ok: db 'LOADER A20 VERIFY PASS', 13, 10, 0
a20_fail: db 'LOADER A20 VERIFY FAIL', 13, 10, 0
a20_control_unavailable: db 'LOADER CONTROL A20 PRE_ENABLED', 13, 10, 0
e820_fail: db 'LOADER E820 FAIL', 13, 10, 0
e820_overflow: db 'LOADER E820 OVERFLOW', 13, 10, 0
rsdp_control_rejected: db 'LOADER CONTROL RSDP CHECKSUM REJECTED', 13, 10, 0
mb2_fail: db 'LOADER MB2 OVERFLOW', 13, 10, 0
kernel_load_ok: db 'LOADER KERNEL LOAD OK', 13, 10, 0
kernel_enter_marker: db 'LOADER ENTER KERNEL', 13, 10, 0
kernel_load_fail: db 'LOADER KERNEL LOAD FAIL', 13, 10, 0
kernel_elf_fail: db 'LOADER KERNEL ELF FAIL', 13, 10, 0
kernel_read_fail: db 'LOADER KERNEL READ FAIL', 13, 10, 0
short_segment_control: db 'LOADER CONTROL PT_LOAD ONE PAGE SHORT', 13, 10, 0
bss_zero_control: db 'LOADER CONTROL BSS ZERO SKIPPED', 13, 10, 0

; mkiso.py patches this with the kernel's native-CD LBA and block count using
; the exact descriptor format preload uses for loader.  Pinning it away from
; executable bytes lets the writer reject moved or duplicate magic instead of
; guessing at a byte sequence inside code or strings.
times KERNEL_PATCH_OFFSET - ($ - $$) db 0
kernel_patch_magic: db 'L2P!'
kernel_lba: dd 0
kernel_block_count: dw 0
