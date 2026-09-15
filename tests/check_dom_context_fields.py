#!/usr/bin/env python3
"""Audit actual C declarations, including included interface state.

Regex only locates the explicit ownership macro; clang identifies declarations.
Unknown function-static caches fail too, even if their name is not prefixed g_.
The allowlist is ownership policy, not a blanket exemption for pointer-to-const.
"""
import argparse
import json
from pathlib import Path
import re


def audit(ast, source, kind="dom"):
    macro = kind.upper() + "_CONTEXT_FIELDS"
    match = re.search(r"#define " + macro + r"\(X\)(.*?)\n\n", source, re.S)
    if not match:
        raise ValueError(macro + " missing")
    owned = set(re.findall(r"X\((\w+)\)", match[1]))
    shared = {
        # One process-wide class identity, registered separately in each runtime.
        "elem_cid", "live_list_cid", "token_cid", "cssd_cid", "event_cid",
        # Runtime-independent native function tables, never mutated after init.
        "elem_class", "live_list_class", "live_list_exotic", "token_class",
        "token_exotic", "cssd_class", "event_class",
        # Context switcher's own bookkeeping, not page state.
        "dom_default_context", "dom_active_context",
        # Function-local table of three immutable property names.
        "THREE",
    }
    if kind == "page":
        shared = {"g_clock", "page_default_queue", "page_default_context", "page_active_context"}
    if kind == "webapi":
        shared = {
            "g_net", "g_net_env_checked",  # shared transport service
            "g_fetch_realms", "g_fetch_realm_serial", "g_fetch", "g_fetch_live",
            "g_jar", "g_jar_ready", "g_cookie_store", "g_pfc",
            "storage_cid", "storage_class",  # stable process-wide class ID
            "g_blob_generation",  # globally unique, never reset by navigation
            "PRELUDE", "WORKER_FETCH_PRELUDE",  # immutable source pointers
            "webapi_default_context", "webapi_active_context",
            "buf", "cookie",  # synchronous Cookie scratch; never retained
        }
    if kind == "platform":
        shared = {
            "g_s0", "g_s1", "g_seeded", "g_rng_kernel",  # shared entropy service
            "PLATFORM_PRELUDE", "mo_default_subscription",
            "platform_default_context", "platform_active_context",
        }
    seen, unknown = set(), []
    filename = None

    def constant_type(typ):
        # const T arrays/scalars, T *const, and arrays of const pointers.
        # A `const char *` variable is NOT immutable.
        if "*" in typ:
            return bool(re.match(r"const(?:\s|\[|\)|$)", typ.rsplit("*", 1)[1].lstrip()))
        return typ.startswith("const ")

    def visit(node, local=False):
        if node.get("kind") == "VarDecl" and node.get("storageClass") == "static":
            name, typ = node["name"], node["type"]["qualType"]
            if name in owned and not local:
                seen.add(name)
            elif name not in shared and not constant_type(typ):
                unknown.append(name + ": " + typ)
        for child in node.get("inner", []):
            visit(child, local or node.get("kind") == "FunctionDecl")

    for node in ast.get("inner", []):
        loc = node.get("loc", {})
        loc = loc.get("expansionLoc", loc)
        if "file" in loc:
            filename = loc["file"]
        files = {"js_" + kind + ".c", "js_" + kind + "_iface.inc"}
        if kind == "platform":
            files.update(("js_native_mo.inc", "js_rejections.inc", "js_bootstrap_scan.inc"))
        if filename and Path(filename).name in files:
            visit(node)
    if unknown or owned - seen:
        raise ValueError("unowned mutable state: " + ", ".join(unknown) +
                         "; missing declarations: " + ", ".join(sorted(owned - seen)))
    return len(seen)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ast", type=Path)
    p.add_argument("source", type=Path)
    p.add_argument("--kind", choices=("dom", "page", "webapi", "platform"), default="dom")
    args = p.parse_args()
    ast = json.loads(args.ast.read_text())
    source = args.source.read_text()
    count = audit(ast, source, args.kind)
    # Self-negative: removing one ownership entry must be caught by the AST,
    # not accepted as a smaller but equally 'complete' list.
    field = {"dom": "g_root", "page": "g_page", "webapi": "g_hist", "platform": "mo_head"}[args.kind]
    try:
        audit(ast, source.replace("X(" + field + ")", "", 1), args.kind)
    except ValueError as error:
        if field + ":" not in str(error):
            raise
    else:
        raise ValueError("negative control failed to detect unowned " + field)
    print(f"{args.kind} context inventory: {count} owned fields; negative control caught {field}")


if __name__ == "__main__":
    main()
