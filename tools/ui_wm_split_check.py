#!/usr/bin/env python3
"""Static check of the window manager's split into per-surface files.

Usage: ui_wm_split_check.py [--root .] [--fixture]

wm.c was split into wm_<surface>.c files (ui-wm-split) so later work can edit
one surface without touching the others. This keeps the split honest. Every
failure prints `FAIL <check> <detail>`; the exit status is 1 if any did.

  placement  each listed function is defined exactly once, in its own file,
             and wm.c defines none of the surfaces' functions; the functions
             that must stay in wm.c are defined there.
  lie        the source-text negative controls (tests/qmp/qmp_damage.py,
             qmp_window.py, qmp_motion.py, qmp_flash.py) build by replacing
             one #define line in wm.c: each needle is in wm.c exactly once,
             nowhere else under c/kernel/gui/wm*, and every #if that reads it
             is in wm.c too (and there is at least one).
  glass      no fb_liquid_glass* / profiled_glass* call in c/kernel/gui/wm*.c
             outside wm_glass.c: every piece of chrome glass goes through
             wm_glass_panel.

--fixture runs the same checks over a scratch copy of the sources in which
wm.c also defines draw_dock -- the mistake the split exists to prevent -- and
must print `FAIL placement draw_dock` (tests/ui-wm-split.mk's control).
"""
import argparse
import os
import re
import shutil
import sys
import tempfile

GUI = "c/kernel/gui"

# file -> the functions it alone defines (design: backlog ui-wm-split item 2,
# plus the neutral hooks of items 4-6).
PLACEMENT = {
    "wm_wallpaper.c": "bg_color draw_wallpaper draw_background wm_wallpaper_path_for_theme "
                      "wm_desktop_context wm_wallpaper_tick",
    "wm_glass.c": "profiled_glass profiled_glass_cut wm_glass_panel wm_glass_app "
                  "wm_glass_regions wm_glass_tick",
    "wm_menubar.c": "menu_title_hit menu_bar_layout draw_menubar draw_clock wm_damage_menubar "
                    "wm_menubar_glass_rect wm_menubar_pointer wm_menubar_click "
                    "wm_menubar_ime_hit wm_menubar_next_wake wm_menubar_tick",
    "wm_menu.c": "menu_model_sync menu_key_text menu_report_open menu_close menu_key "
                 "draw_menu_dropdown menu_dropdown_box menu_item_fire menu_open "
                 "wm_menu_glass_rect wm_menu_pointer wm_menu_click wm_menu_tick",
    "wm_dock.c": "dock_geom dock_icon_box dock_panel_box dock_tile dock_hover_at dirty_dock "
                 "dirty_dock_hov dock_bounce_off wm_dock_autohide_tick draw_dock scan_apps "
                 "dock_publish dock_slot_for dock_file_for_ext dock_launch_bounce "
                 "wm_dock_glass_rect wm_dock_pointer wm_dock_click wm_dock_tick",
    "wm_overlay.c": "overlay_box overlay_buttons overlay_close overlay_open fmt_u "
                    "draw_overlay_panel draw_switcher sw_open sw_commit sw_tick mru_touch "
                    "wm_overlay_glass_rect wm_overlay_pointer wm_overlay_click wm_overlay_tick",
    "wm_chrome.c": "draw_titlebar_sep draw_frame_body draw_frame win_chrome_strip win_corner_r "
                   "content_group_begin content_group_end win_glass_box wm_chrome_glass_titlebar "
                   "wm_chrome_glass_rects wm_chrome_pointer wm_chrome_click wm_chrome_tick",
    "wm_place.c": "win_min_frame win_set_frame win_fit_work_area win_set_zoom win_break_zoom "
                  "snap_set win_snap draw_snap_preview resize_hit wm_place_cascade",
    "wm_anim.c": "ease_out anim_elapsed min_prog win_fly_sweep win_set_min ghost_take draw_ghost "
                 "win_open_scale anim_trace wm_anim_open_tick wm_anim_close_tick wm_anim_fly_tick",
    "wm_expose.c": "ex_state ex_layout ex_slot ex_prog ex_enter ex_leave wm_hotcorner_tick "
                   "wm_expose_pointer wm_expose_click wm_expose_anim_tick wm_expose_tick",
    "wm_panel.c": "panel_show wm_panel_pointer wm_panel_tick",
    "wm_reveal.c": "wm_reveal_begin wm_reveal_tick wm_reveal_note_flush wm_reveal_active "
                   "wm_reveal_present",
    "wm_theme.c": "wm_theme_refresh",
    # what stays: the compositor, the damage contract and the dispatchers
    "wm.c": "wm_process_mouse wm_anim_tick anim_damage wm_anim_sweep_damage "
            "wm_resize_damage_old dirty_win dirty_win_content dmg_expand render_layers "
            "wm_render_full_into wm_set_dark is_draw_call wm_domain_call wm_gui_dispatch wm_run",
}

# needle (replaced in a copy of wm.c by the named driver) -> the macro it sets
LIES = {
    "#define WM_DAMAGE_LIE 0": ("WM_DAMAGE_LIE", "tests/qmp/qmp_damage.py"),
    "#define WM_RESIZE_DAMAGE_LIE 0": ("WM_RESIZE_DAMAGE_LIE", "tests/qmp/qmp_window.py"),
    "#define WM_ANIM_DAMAGE_LIE 0": ("WM_ANIM_DAMAGE_LIE", "tests/qmp/qmp_motion.py"),
    "#define WM_MIDFRAME_GUARD 1": ("WM_MIDFRAME_GUARD", "tests/qmp/qmp_flash.py"),
}

GLASS_CALL = re.compile(r"\b(fb_liquid_glass\w*|profiled_glass\w*)\s*\(")


def code(s):
    """Comments and string/char literals blanked; newlines kept."""
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                  lambda m: re.sub(r"[^\n]", " ", m[0]), s, flags=re.S)


def definitions(src, name):
    """Lines (1-based) where `name` is DEFINED at file scope (a body follows)."""
    out = []
    pat = re.compile(r"^[A-Za-z_][\w \t\*]*?\b%s\s*\(([^;{}]|\n)*?\)\s*\{" % re.escape(name), re.M)
    for m in pat.finditer(src):
        out.append(src[:m.start()].count("\n") + 1)
    return out


def check(root):
    bad = []

    def fail(what, detail):
        bad.append((what, detail))
        print("FAIL %s %s" % (what, detail))

    gui = os.path.join(root, GUI)
    names = sorted(f for f in os.listdir(gui) if re.match(r"wm(_\w+)?\.[ch]$", f))
    raw = {f: open(os.path.join(gui, f), encoding="utf-8", errors="replace").read() for f in names}
    src = {f: code(t) for f, t in raw.items()}
    cfiles = [f for f in names if f.endswith(".c")]

    # placement
    nfun = 0
    for owner, fns in PLACEMENT.items():
        if owner not in src:
            fail("placement", "%s missing" % owner)
            continue
        for fn in fns.split():
            nfun += 1
            where = {f: definitions(src[f], fn) for f in cfiles}
            here = where.get(owner, [])
            others = sorted(f for f, l in where.items() if l and f != owner)
            if len(here) != 1 or others:
                fail("placement", "%s (want once in %s; %s)" % (
                    fn, owner, ", ".join("%s x%d" % (f, len(l)) for f, l in where.items() if l) or "defined nowhere"))
    # lie needles
    for needle, (macro, driver) in LIES.items():
        users = {}
        for f in names:
            n = raw[f].count(needle)
            if n and f != "wm.c":
                fail("lie", "%s also in %s (%s rewrites wm.c only)" % (needle, f, driver))
            for m in re.finditer(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b[^\n]*\b%s\b" % macro, src[f], re.M):
                users.setdefault(f, 0)
                users[f] += 1
        if raw.get("wm.c", "").count(needle) != 1:
            fail("lie", "'%s' is not in wm.c exactly once (%s builds its control from it)" % (needle, driver))
        for f in users:
            if f != "wm.c":
                fail("lie", "%s read by an #if in %s, not wm.c" % (macro, f))
        if not users.get("wm.c"):
            fail("lie", "%s has no #if user in wm.c" % macro)
    # glass
    for f in cfiles:
        if f == "wm_glass.c":
            continue
        for m in GLASS_CALL.finditer(src[f]):
            fail("glass", "%s:%d calls %s outside wm_glass.c" % (f, src[f][:m.start()].count("\n") + 1, m[1]))
    if not bad:
        print("ok ui-wm-split: %d functions placed in %d files, %d control needles in wm.c, "
              "chrome glass only in wm_glass.c" % (nfun, len(PLACEMENT), len(LIES)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=".")
    ap.add_argument("--fixture", action="store_true",
                    help="check a copy in which wm.c also defines draw_dock (must FAIL placement draw_dock)")
    a = ap.parse_args()
    if not a.fixture:
        return check(a.root)
    tmp = tempfile.mkdtemp(prefix="ui-wm-split-")
    try:
        dst = os.path.join(tmp, GUI)
        os.makedirs(dst)
        for f in os.listdir(os.path.join(a.root, GUI)):
            if re.match(r"wm(_\w+)?\.[ch]$", f):
                shutil.copy(os.path.join(a.root, GUI, f), dst)
        with open(os.path.join(dst, "wm.c"), "a", encoding="utf-8") as fh:
            fh.write("\nstatic void draw_dock(void)\n{\n}\n")
        return check(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
