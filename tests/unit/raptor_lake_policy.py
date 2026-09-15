#!/usr/bin/env python3
"""Write one deliberately broken cpu_platform.c for an observable control."""
from pathlib import Path
import argparse
import re


ROOT = Path(__file__).resolve().parents[2]


def replace_once(text: str, needle: str, replacement: str, mode: str) -> str:
    if text.count(needle) != 1:
        raise SystemExit(f"NEGCTL FAIL: {mode} mutation seam count={text.count(needle)}")
    return text.replace(needle, replacement, 1)


def check_report() -> int:
    text = (ROOT / "c/crypto/cpu_report.c").read_text()
    checks = (
        (
            r'put\("\[cpu\] topology: source="\);\s*'
            r'if \(p->topology\.leaf_1f\) put\("cpuid\.0x1f"\);\s*'
            r'else if \(p->topology\.leaf_b\) put\("cpuid\.0xb"\);',
            "report identifies CPUID.1F before CPUID.0B",
        ),
        (
            r'put\(" cpuid-addressable/package="\);\s*'
            r'put_u\(p->topology\.logical_per_package\);',
            "report labels CPUID topology count as addressable capacity",
        ),
        (
            r'put\(" cores/package="\);\s*'
            r'if \(p->topology\.core_count_exact\) '
            r'put_u\(p->topology\.cores_per_package\);\s*'
            r'else put\("unknown"\);',
            "report does not label hybrid uniform-SMT inference as a core count",
        ),
    )
    failed = [label for pattern, label in checks if not re.search(pattern, text)]
    for label in failed:
        print(f"FAIL: {label}")
    print(f"raptor_lake_report: {len(checks)} checks, {len(failed)} failed")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check-report", action="store_true")
    parser.add_argument("mode", nargs="?",
                        choices=("uniform-smt", "leaf-b-first", "model-range",
                                 "malformed-topology"))
    parser.add_argument("output", nargs="?", type=Path)
    args = parser.parse_args()
    if args.check_report:
        if args.mode is not None or args.output is not None:
            parser.error("--check-report does not take a mutation or output")
        return check_report()
    if args.mode is None or args.output is None:
        parser.error("mutation mode and output are required")

    text = (ROOT / "c/kernel/cpu/cpu_platform.c").read_text()

    if args.mode == "uniform-smt":
        text = replace_once(
            text,
            "top->cores_per_package = hybrid ? 0 : package_count / smt_count;",
            "top->cores_per_package = package_count / smt_count;",
            args.mode,
        )
    elif args.mode == "leaf-b-first":
        text = replace_once(
            text,
            "if (max_leaf >= 0x1fu &&\n"
            "        decode_extended_topology(ops, 0x1fu, hybrid, top)) return;",
            "if (max_leaf >= 0x0bu &&\n"
            "        decode_extended_topology(ops, 0x0bu, hybrid, top)) return;",
            args.mode,
        )
    elif args.mode == "model-range":
        text = replace_once(
            text,
            "(out->model == 0xbfu &&\n"
            "                  (out->stepping == 2u || out->stepping == 5u))",
            "(out->model == 0xbfu)",
            args.mode,
        )
    else:
        text = replace_once(
            text,
            "if (!count) return 0;",
            "if (!count) { terminated = 1; break; }",
            args.mode,
        )
        text = replace_once(
            text,
            "if (level != sub || shift >= 32u ||\n"
            "            (have_level &&\n"
            "             (shift < previous_shift || count < previous_count)) ||\n"
            "            (uint64_t)count > (1ull << shift))\n"
            "            return 0;",
            "(void)level; (void)previous_count; (void)previous_shift;\n"
            "        if (shift >= 32u) return 0;",
            args.mode,
        )
        text = replace_once(
            text,
            "if (have_smt || have_package) return 0;",
            "(void)have_smt; (void)have_package;",
            args.mode,
        )
        text = replace_once(
            text,
            "if (!have_smt || have_core) return 0;",
            "(void)have_core;",
            args.mode,
        )
        text = replace_once(
            text,
            "if (!terminated || !have_smt || !have_core || !have_package || !smt_count ||\n"
            "        package_count < smt_count || smt_shift > package_shift)\n"
            "        return 0;",
            "(void)terminated;",
            args.mode,
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)
    print(f"wrote {args.mode} negative-control source")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
