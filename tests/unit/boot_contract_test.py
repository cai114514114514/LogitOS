#!/usr/bin/env python3
"""Source gate for the native boot block shared by BIOS, UEFI, and the kernel.

This is deliberately a narrow parser for today's source shapes.  It understands
packed structs made from fixed-width integers/nested structs, direct NASM
``mov [es:di + OFFSET]`` writes in ``build_boot_info``, and direct C member
writes/checks.  Every extraction has a match floor, so changing those shapes
must make this gate red until the parser is taught the new form.

What can still slip past: a second BIOS block builder added under another label
is outside the named-function scan, and the opaque E820 entry copy is checked
for its 24-byte stride but not for the firmware meaning of its payload bytes.
Those require a live producer/consumer oracle, not another source spelling.
"""

import argparse
import re
import sys
from pathlib import Path


failures = 0


def fail(message: str) -> None:
    global failures
    failures += 1
    print(f"FAIL: {message}", file=sys.stderr)


def read_source(path: str) -> str:
    try:
        return Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"cannot open {path}: {exc}")
        return ""


def strip_c_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def numeric_defines(text: str, path: str) -> dict[str, int]:
    found: dict[str, int] = {}
    for match in re.finditer(
        r"(?m)^\s*#define\s+(LOGIT_BOOT_[A-Z0-9_]+)\s+"
        r"(0[xX][0-9a-fA-F]+|[0-9]+)\s*$",
        strip_c_comments(text),
    ):
        found[match.group(1)] = int(match.group(2), 0)
    if len(found) < 35:
        fail(f"{path} yielded {len(found)} literal LOGIT_BOOT defines; require at least 35")
    return found


def packed_layouts(text: str, path: str) -> tuple[dict[tuple[str, str], int], dict[str, int]]:
    clean = strip_c_comments(text)
    layouts: dict[tuple[str, str], int] = {}
    sizes: dict[str, int] = {}
    matches = list(
        re.finditer(
            r"struct\s+(logit_boot_[a-z0-9_]+)\s*\{(.*?)\}\s*"
            r"__attribute__\s*\(\(packed\)\)\s*;",
            clean,
            flags=re.S,
        )
    )
    if len(matches) < 5:
        fail(f"{path} yielded {len(matches)} packed boot structs; require at least 5")

    widths = {"uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "uint64_t": 8}
    for match in matches:
        struct_name = match.group(1)
        offset = 0
        field_count = 0
        for declaration in match.group(2).split(";"):
            declaration = declaration.strip()
            if not declaration:
                continue
            field = re.fullmatch(
                r"(uint(?:8|16|32|64)_t|struct\s+logit_boot_[a-z0-9_]+)\s+(.+)",
                declaration,
            )
            if not field:
                fail(f"cannot parse field declaration in {path}: {declaration}")
                continue
            type_name, declarators = field.groups()
            if type_name.startswith("struct "):
                nested = type_name.split()[1]
                if nested not in sizes:
                    fail(f"{path} uses {nested} before its packed size is known")
                    width = 0
                else:
                    width = sizes[nested]
            else:
                width = widths[type_name]
            for declarator in declarators.split(","):
                declarator = declarator.strip()
                name_match = re.fullmatch(r"([a-zA-Z_][a-zA-Z0-9_]*)(\[\])?", declarator)
                if not name_match:
                    fail(f"cannot parse declarator in {path}: {declarator}")
                    continue
                name, flexible = name_match.groups()
                layouts[(struct_name, name)] = offset
                field_count += 1
                if not flexible:
                    offset += width
        if field_count < 2:
            fail(f"{path} struct {struct_name} yielded {field_count} fields; require at least 2")
        sizes[struct_name] = offset
    return layouts, sizes


FIELD_OFFSETS = [
    ("logit_boot_header", "magic", "LOGIT_BOOT_HEADER_MAGIC_OFFSET", "header.magic"),
    ("logit_boot_header", "version", "LOGIT_BOOT_HEADER_VERSION_OFFSET", "header.version"),
    ("logit_boot_header", "header_size", "LOGIT_BOOT_HEADER_HEADER_SIZE_OFFSET", "header.header_size"),
    ("logit_boot_header", "identity_map_bytes", "LOGIT_BOOT_HEADER_IDENTITY_MAP_BYTES_OFFSET", "header.identity_map_bytes"),
    ("logit_boot_header", "total_size", "LOGIT_BOOT_HEADER_TOTAL_SIZE_OFFSET", "header.total_size"),
    ("logit_boot_header", "reserved", "LOGIT_BOOT_HEADER_RESERVED_OFFSET", "header.reserved"),
    ("logit_boot_tag", "type", "LOGIT_BOOT_TAG_TYPE_OFFSET", "tag.type"),
    ("logit_boot_tag", "size", "LOGIT_BOOT_TAG_SIZE_OFFSET", "tag.size"),
    ("logit_boot_mmap_tag", "entry_size", "LOGIT_BOOT_MMAP_TAG_ENTRY_SIZE_OFFSET", "mmap.entry_size"),
    ("logit_boot_mmap_tag", "entry_version", "LOGIT_BOOT_MMAP_TAG_ENTRY_VERSION_OFFSET", "mmap.entry_version"),
    ("logit_boot_mmap_tag", "entries", "LOGIT_BOOT_MMAP_TAG_ENTRIES_OFFSET", "mmap.entries"),
    ("logit_boot_mmap_entry", "addr", "LOGIT_BOOT_MMAP_ENTRY_ADDR_OFFSET", "mmap_entry.addr"),
    ("logit_boot_mmap_entry", "len", "LOGIT_BOOT_MMAP_ENTRY_LEN_OFFSET", "mmap_entry.len"),
    ("logit_boot_mmap_entry", "type", "LOGIT_BOOT_MMAP_ENTRY_TYPE_OFFSET", "mmap_entry.type"),
    ("logit_boot_mmap_entry", "reserved", "LOGIT_BOOT_MMAP_ENTRY_RESERVED_OFFSET", "mmap_entry.reserved"),
    ("logit_boot_framebuffer_tag", "addr", "LOGIT_BOOT_FRAMEBUFFER_TAG_ADDR_OFFSET", "framebuffer.addr"),
    ("logit_boot_framebuffer_tag", "pitch", "LOGIT_BOOT_FRAMEBUFFER_TAG_PITCH_OFFSET", "framebuffer.pitch"),
    ("logit_boot_framebuffer_tag", "width", "LOGIT_BOOT_FRAMEBUFFER_TAG_WIDTH_OFFSET", "framebuffer.width"),
    ("logit_boot_framebuffer_tag", "height", "LOGIT_BOOT_FRAMEBUFFER_TAG_HEIGHT_OFFSET", "framebuffer.height"),
    ("logit_boot_framebuffer_tag", "bpp", "LOGIT_BOOT_FRAMEBUFFER_TAG_BPP_OFFSET", "framebuffer.bpp"),
    ("logit_boot_framebuffer_tag", "framebuffer_type", "LOGIT_BOOT_FRAMEBUFFER_TAG_TYPE_OFFSET", "framebuffer.framebuffer_type"),
    ("logit_boot_framebuffer_tag", "reserved", "LOGIT_BOOT_FRAMEBUFFER_TAG_RESERVED_OFFSET", "framebuffer.reserved"),
    ("logit_boot_framebuffer_tag", "red_position", "LOGIT_BOOT_FRAMEBUFFER_TAG_RED_POSITION_OFFSET", "framebuffer.red_position"),
    ("logit_boot_framebuffer_tag", "red_mask_size", "LOGIT_BOOT_FRAMEBUFFER_TAG_RED_SIZE_OFFSET", "framebuffer.red_mask_size"),
    ("logit_boot_framebuffer_tag", "green_position", "LOGIT_BOOT_FRAMEBUFFER_TAG_GREEN_POSITION_OFFSET", "framebuffer.green_position"),
    ("logit_boot_framebuffer_tag", "green_mask_size", "LOGIT_BOOT_FRAMEBUFFER_TAG_GREEN_SIZE_OFFSET", "framebuffer.green_mask_size"),
    ("logit_boot_framebuffer_tag", "blue_position", "LOGIT_BOOT_FRAMEBUFFER_TAG_BLUE_POSITION_OFFSET", "framebuffer.blue_position"),
    ("logit_boot_framebuffer_tag", "blue_mask_size", "LOGIT_BOOT_FRAMEBUFFER_TAG_BLUE_SIZE_OFFSET", "framebuffer.blue_mask_size"),
]


SIZE_MACROS = [
    ("logit_boot_header", "LOGIT_BOOT_HEADER_SIZE"),
    ("logit_boot_tag", "LOGIT_BOOT_TAG_SIZE"),
    ("logit_boot_mmap_entry", "LOGIT_BOOT_MMAP_ENTRY_SIZE"),
    ("logit_boot_mmap_tag", "LOGIT_BOOT_MMAP_TAG_HEADER_SIZE"),
    ("logit_boot_framebuffer_tag", "LOGIT_BOOT_FRAMEBUFFER_TAG_SIZE"),
]


def compare_header_layout(header: str, path: str) -> tuple[dict[str, int], dict[tuple[str, str], int]]:
    defines = numeric_defines(header, path)
    layouts, sizes = packed_layouts(header, path)
    for struct_name, field, macro, display in FIELD_OFFSETS:
        actual = layouts.get((struct_name, field))
        declared = defines.get(macro)
        if actual is None:
            fail(f"field {display} was not extracted from {path}")
        if declared is None:
            fail(f"{path} does not define {macro}")
        elif actual is not None and actual != declared:
            fail(f"field {display} offset: C={actual} BIOS={declared}")
    for struct_name, macro in SIZE_MACROS:
        actual = sizes.get(struct_name)
        declared = defines.get(macro)
        if actual is None or declared is None:
            fail(f"cannot compare sizeof({struct_name}) with {macro}")
        elif actual != declared:
            fail(f"sizeof({struct_name}): C={actual} BIOS={declared}")
    assertions = len(re.findall(r"\b_Static_assert\s*\(", strip_c_comments(header)))
    if assertions < 5:
        fail(f"{path} yielded {assertions} static layout assertions; require at least 5")
    return defines, layouts


def compact(text: str) -> str:
    return re.sub(r"\s+", "", text)


def require_compact(text: str, needle: str, label: str, minimum: int = 1) -> None:
    matches = compact(text).count(compact(needle))
    if matches < minimum:
        fail(f"{label} yielded {matches} matches; require at least {minimum}")


def asm_builder(text: str, path: str) -> str:
    match = re.search(r"(?ms)^build_boot_info:\s*(.*?)^align_di_8:", text)
    if not match:
        fail(f"cannot extract build_boot_info from {path}")
        return ""
    return re.sub(r";[^\n]*", "", match.group(1))


ASM_REQUIRED = [
    ("mov dword [es:di + LOGIT_BOOT_HEADER_MAGIC_OFFSET], LOGIT_BOOT_MAGIC", "BIOS header.magic write"),
    ("mov word [es:di + LOGIT_BOOT_HEADER_HEADER_SIZE_OFFSET], LOGIT_BOOT_HEADER_SIZE", "BIOS header.header_size write"),
    ("mov dword [es:di + LOGIT_BOOT_HEADER_IDENTITY_MAP_BYTES_OFFSET], LOGIT_BOOT_IDENTITY_MAP_BYTES", "BIOS header.identity_map_bytes low write"),
    ("mov dword [es:di + LOGIT_BOOT_HEADER_IDENTITY_MAP_BYTES_OFFSET + 4], 0", "BIOS header.identity_map_bytes high write"),
    ("mov dword [es:di + LOGIT_BOOT_HEADER_TOTAL_SIZE_OFFSET], 0", "BIOS header.total_size initialization"),
    ("mov dword [es:di + LOGIT_BOOT_HEADER_RESERVED_OFFSET], 0", "BIOS header.reserved write"),
    ("mov dword [es:di + LOGIT_BOOT_TAG_TYPE_OFFSET], BOOT_TAG_MMAP", "BIOS mmap tag.type write"),
    ("mov [es:di + LOGIT_BOOT_TAG_SIZE_OFFSET], eax", "BIOS variable tag.size writes", 2),
    ("mov dword [es:di + LOGIT_BOOT_MMAP_TAG_ENTRY_SIZE_OFFSET], MMAP_ENTRY_BYTES", "BIOS mmap.entry_size write"),
    ("mov dword [es:di + LOGIT_BOOT_MMAP_TAG_ENTRY_VERSION_OFFSET], 0", "BIOS mmap.entry_version write"),
    ("mov dword [es:di + LOGIT_BOOT_TAG_TYPE_OFFSET], BOOT_TAG_ACPI_NEW", "BIOS ACPI-new tag.type write"),
    ("mov dword [es:di + LOGIT_BOOT_TAG_TYPE_OFFSET], BOOT_TAG_ACPI_OLD", "BIOS ACPI-old tag.type write"),
    ("mov dword [es:di + LOGIT_BOOT_TAG_TYPE_OFFSET], BOOT_TAG_FRAMEBUFFER", "BIOS framebuffer tag.type write"),
    ("mov dword [es:di + LOGIT_BOOT_TAG_SIZE_OFFSET], LOGIT_BOOT_FRAMEBUFFER_TAG_SIZE", "BIOS framebuffer tag.size write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_ADDR_OFFSET], eax", "BIOS framebuffer.addr low write"),
    ("mov dword [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_ADDR_OFFSET + 4], 0", "BIOS framebuffer.addr high write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_PITCH_OFFSET], eax", "BIOS framebuffer.pitch write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_WIDTH_OFFSET], eax", "BIOS framebuffer.width write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_HEIGHT_OFFSET], eax", "BIOS framebuffer.height write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_BPP_OFFSET], al", "BIOS framebuffer.bpp write"),
    ("mov byte [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_TYPE_OFFSET], 1", "BIOS framebuffer.framebuffer_type write"),
    ("mov word [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_RESERVED_OFFSET], 0", "BIOS framebuffer.reserved write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_RED_POSITION_OFFSET], al", "BIOS framebuffer.red_position write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_RED_SIZE_OFFSET], al", "BIOS framebuffer.red_mask_size write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_GREEN_POSITION_OFFSET], al", "BIOS framebuffer.green_position write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_GREEN_SIZE_OFFSET], al", "BIOS framebuffer.green_mask_size write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_BLUE_POSITION_OFFSET], al", "BIOS framebuffer.blue_position write"),
    ("mov [es:di + LOGIT_BOOT_FRAMEBUFFER_TAG_BLUE_SIZE_OFFSET], al", "BIOS framebuffer.blue_mask_size write"),
    ("mov dword [es:di + LOGIT_BOOT_TAG_TYPE_OFFSET], BOOT_TAG_END", "BIOS end tag.type write"),
    ("mov dword [es:di + LOGIT_BOOT_TAG_SIZE_OFFSET], LOGIT_BOOT_TAG_SIZE", "BIOS end tag.size write"),
    ("mov [es:BOOT_INFO + LOGIT_BOOT_HEADER_TOTAL_SIZE_OFFSET], eax", "BIOS final header.total_size write"),
]


def normal_control_branch(text: str, directive: str, symbol: str) -> str:
    pattern = re.compile(
        rf"(?ms)^\s*{re.escape(directive)}\s+{re.escape(symbol)}\s*$"
        rf".*?^\s*(?:#else|%else)\s*$"
        rf"(.*?)^\s*(?:#endif|%endif)\s*$"
    )
    return pattern.sub(lambda match: match.group(1), text)


def eval_version(expression: str, defines: dict[str, int]) -> int | None:
    expression = expression.strip()
    match = re.fullmatch(r"LOGIT_BOOT_VERSION(?:\s*([+-])\s*(0[xX][0-9a-fA-F]+|[0-9]+))?", expression)
    if match:
        value = defines.get("LOGIT_BOOT_VERSION")
        if value is None:
            return None
        if match.group(1):
            delta = int(match.group(2), 0)
            value = value + delta if match.group(1) == "+" else value - delta
        return value
    if re.fullmatch(r"0[xX][0-9a-fA-F]+|[0-9]+", expression):
        return int(expression, 0)
    return None


def check_versions(defines: dict[str, int], layouts: dict[tuple[str, str], int],
                   asm: str, asm_path: str, efi: str, efi_path: str,
                   kernel: str, kernel_path: str) -> None:
    wanted = defines.get("LOGIT_BOOT_VERSION")
    if wanted is None:
        fail("LOGIT_BOOT_VERSION was not extracted from the header")
        return

    normal_asm = normal_control_branch(asm, "%ifdef", "LOADER_NEGCTL_NATIVE_BAD_VERSION")
    asm_sites = re.findall(
        r"(?m)^\s*mov\s+word\s+\[es:di\s*\+\s*(LOGIT_BOOT_[A-Z0-9_]+)\]\s*,\s*"
        r"(LOGIT_BOOT_VERSION(?:\s*[+-]\s*(?:0[xX][0-9a-fA-F]+|[0-9]+))?)\s*$",
        re.sub(r";[^\n]*", "", normal_asm),
    )
    if len(asm_sites) < 1:
        fail(f"{asm_path} yielded {len(asm_sites)} normal protocol-version writes; require at least 1")
    for offset_name, expression in asm_sites:
        used_offset = defines.get(offset_name)
        c_offset = layouts.get(("logit_boot_header", "version"))
        if used_offset is None:
            fail(f"BIOS version write uses unresolved offset {offset_name}")
        elif c_offset is not None and used_offset != c_offset:
            fail(f"field header.version offset: C={c_offset} BIOS={used_offset}")
        value = eval_version(expression, defines)
        if value is None:
            fail(f"cannot evaluate BIOS protocol version expression {expression}")
        elif value != wanted:
            fail(f"protocol version BIOS-writer=0x{value:04x} header=0x{wanted:04x}")

    normal_efi = normal_control_branch(efi, "#ifdef", "EFI_NATIVE_BAD_VERSION")
    efi_sites = re.findall(r"native_header->version\s*=\s*([^;]+);", strip_c_comments(normal_efi))
    if len(efi_sites) < 1:
        fail(f"{efi_path} yielded {len(efi_sites)} normal protocol-version writes; require at least 1")
    for expression in efi_sites:
        value = eval_version(expression, defines)
        if value is None:
            fail(f"cannot evaluate UEFI protocol version expression {expression.strip()}")
        elif value != wanted:
            fail(f"protocol version UEFI-writer=0x{value:04x} header=0x{wanted:04x}")

    kernel_sites = re.findall(r"header->version\s*!=\s*([^\)\{\n]+)", strip_c_comments(kernel))
    if len(kernel_sites) < 1:
        fail(f"{kernel_path} yielded {len(kernel_sites)} protocol-version checks; require at least 1")
    for expression in kernel_sites:
        value = eval_version(expression, defines)
        if value is None:
            fail(f"cannot evaluate kernel protocol version expression {expression.strip()}")
        elif value != wanted:
            fail(f"protocol version kernel-check=0x{value:04x} header=0x{wanted:04x}")


def check_asm(asm: str, path: str) -> str:
    body = asm_builder(asm, path)
    require_compact(asm, "%define MMAP_ENTRY_BYTES LOGIT_BOOT_MMAP_ENTRY_SIZE",
                    "BIOS memory-map entry stride")
    require_compact(asm,
                    "mov dword [es:di + LOGIT_BOOT_MMAP_ENTRY_RESERVED_OFFSET], 1",
                    "BIOS E820 extended-attribute request")
    require_compact(asm,
                    "mov dword [es:di + LOGIT_BOOT_MMAP_ENTRY_RESERVED_OFFSET], 0",
                    "BIOS native mmap-entry reserved write")
    for requirement in ASM_REQUIRED:
        needle, label, *floor = requirement
        require_compact(body, needle, label, floor[0] if floor else 1)
    return body


def check_efi(efi: str, path: str) -> None:
    clean = strip_c_comments(efi)
    requirements = [
        ("native_header->magic = LOGIT_BOOT_MAGIC", "UEFI header.magic write"),
        ("native_header->header_size = LOGIT_BOOT_HEADER_SIZE", "UEFI header.header_size write"),
        ("native_header->identity_map_bytes = LOGIT_BOOT_IDENTITY_MAP_BYTES", "UEFI header.identity_map_bytes write"),
        ("native_header->total_size = (UINT32)ib.off", "UEFI header.total_size write"),
        ("ib_take(&ib, sizeof(*mm) + n_desc * sizeof(mm->entries[0]))", "UEFI mmap framed allocation"),
        ("mm->tag.size = (UINT32)(sizeof(*mm) + n_desc * sizeof(mm->entries[0]))", "UEFI mmap tag.size write"),
        ("mm->entry_size = sizeof(mm->entries[0])", "UEFI mmap entry-size write"),
        ("struct logit_boot_tag *end = ib_take(&ib, sizeof(*end))", "UEFI end-tag allocation"),
        ("end->size = sizeof(*end)", "UEFI end tag.size write"),
    ]
    for needle, label in requirements:
        require_compact(clean, needle, label)


def check_kernel(kernel: str, path: str) -> None:
    clean = strip_c_comments(kernel)
    requirements = [
        ("header->header_size != LOGIT_BOOT_HEADER_SIZE", "kernel header-size check"),
        ("header->total_size < LOGIT_BOOT_HEADER_SIZE + sizeof(struct logit_boot_tag)", "kernel minimum block extent"),
        ("uint8_t *cursor = base + header->header_size", "kernel tag-list start"),
        ("while (cursor + sizeof(struct logit_boot_tag) <= end)", "kernel tag-header bound"),
        ("tag->size < sizeof(*tag)", "kernel minimum tag size"),
        ("((uint64_t)tag->size + 7u) & ~7ull", "kernel eight-byte tag rounding"),
        ("tag->size != sizeof(*tag)", "kernel end-tag size check"),
        ("cursor != end", "kernel exact tag-list extent"),
        ("base + LOGIT_BOOT_HEADER_TOTAL_SIZE_OFFSET", "kernel canonical-view start"),
        ("header->total_size - LOGIT_BOOT_HEADER_TOTAL_SIZE_OFFSET", "kernel canonical-view size"),
        ("info_addr + LOGIT_BOOT_HEADER_TOTAL_SIZE_OFFSET", "kernel canonical-view return"),
    ]
    for needle, label in requirements:
        require_compact(clean, needle, label)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", default="include/abi/logit_boot.h")
    parser.add_argument("--asm", default="c/boot/bios/loader.asm")
    parser.add_argument("--efi", default="c/boot/efi/loader.c")
    parser.add_argument("--kernel", default="c/kernel/core/bootinfo.c")
    args = parser.parse_args()

    header = read_source(args.header)
    asm = read_source(args.asm)
    efi = read_source(args.efi)
    kernel = read_source(args.kernel)
    defines, layouts = compare_header_layout(header, args.header)
    check_asm(asm, args.asm)
    check_efi(efi, args.efi)
    check_kernel(kernel, args.kernel)
    check_versions(defines, layouts, asm, args.asm, efi, args.efi, kernel, args.kernel)

    if failures:
        print(f"FAIL: native boot contract ({failures} assertion{'s' if failures != 1 else ''})", file=sys.stderr)
        return 1
    print("PASS: native boot contract -- 28 field offsets, BIOS/UEFI writers, kernel framing, version 0x%04x" % defines["LOGIT_BOOT_VERSION"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
