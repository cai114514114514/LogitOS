/* SPDX-License-Identifier: MIT */
/* Native presentation adapter. The application engine owns documents,
 * projects, persistence, language results and child processes. */
#include "../../../lib/agent/gui.h"
#include "../../studio/engine.h"
#include "../../studio/highlight.h"
#include "aui.h"
#include <stdio.h>

static StEngine *engine;
static const StState *state;
static int work_open, focus, dragging, tree_top, output_top, dialog, completion_selected;
static char query[160], replacement[160];
static int edit_x, edit_y, edit_w, edit_h, out_y, work_x;
static uint64_t completion_due;
#include "../../studio/studio_render.inc"

static const StDocument *document(void)
{
    return st_engine_document(engine);
}

static const char *basename_(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void set_notice(const char *s)
{
    st_engine_notice(engine, s);
}

static void reveal(const StDocument *d)
{
    if (!d) {
        return;
    }
    int row = st_caret_line(d), vis = edit_h / ST_ROW, top = d->top, left = d->left;
    if (vis < 1) {
        vis = 1;
    }
    if (row < top) {
        top = row;
    }
    if (row >= top + vis) {
        top = row - vis + 1;
    }
    int px = line_width(d, st_line_start(d, row), d->caret);
    if (px < left) {
        left = px;
    }
    if (px > left + edit_w - 70) {
        left = px - edit_w + 70;
    }
    st_engine_viewport(engine, top, left);
}

static int open_document(const char *path)
{
    reset_ink();
    completion_due = 0;
    int r = st_engine_open(engine, path);
    if (!r) {
        focus = 0;
        completion_selected = 0;
    }
    return r;
}

static void save_current(void)
{
    st_engine_save(engine);
}

static void start_job(int check)
{
    output_top = 0;
    st_engine_start(engine, check);
}

static void complete(void)
{
    completion_due = 0;
    completion_selected = 0;
    st_engine_complete(engine);
}

static void select_problem(int index)
{
    if (st_engine_select_problem(engine, index) == 0) {
        focus = 0;
        reveal(document());
    }
}

static void close_tab(int index)
{
    reset_ink();
    completion_due = 0;
    st_engine_close(engine, index);
}

static uint64_t host_now(void *ctx)
{
    (void)ctx;
    return monotonic_ms();
}

static int host_get(void *ctx, const char *key, char *out, int capacity)
{
    (void)ctx;
    return setting_get(key, out, capacity);
}

static void host_set(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    setting_set(key, value, 0);
}

static void host_commit(void *ctx)
{
    (void)ctx;
    setting_commit();
}

#include "../../studio/studio_project.inc"
#include "../../studio/studio_editor.inc"
#include "../../studio/studio_completion.inc"
#include "../../studio/studio_code.inc"
#include "../../studio/studio_view.inc"

void app_main(void)
{
    signal(SIGPIPE, SIG_IGN);
    StHost host = {.now = host_now, .get = host_get, .set = host_set, .commit = host_commit};
    engine = st_engine_create(&host);
    if (!engine) {
        app_exit(1);
    }
    state = st_engine_state(engine);
    gui_create("Code Studio", 1120, 700);
    aui_set_size(1120, 700);
    studio_theme();
    trace_paint = setting_int("app.studio.trace_paint", 0);
    char arg[128] = {0};
    get_arg(arg, sizeof arg);
    st_engine_restore(engine, arg);
    draw();
    for (;;) {
        int changed = 0;
        struct logit_event e;
        while (poll_event(&e)) {
            if (ag_gui_event(&e)) {
                continue;
            }
            if (e.type == EV_CLOSE) {
                if (st_engine_shutdown(engine) < 0) {
                    changed = 1;
                    continue;
                }
                st_engine_destroy(engine);
                app_exit(0);
            }
            if (e.type == EV_RESIZE) {
                aui_set_size(e.a, e.b);
                memset(widths, 0, sizeof widths);
                changed = 1;
                continue;
            }
            if (e.type == EV_THEME) {
                changed = 1;
                continue;
            }
            if (e.type == EV_WHEEL) {
                completion_due = 0;
                if (dialog == 1) {
                    picker_top += e.wheel * 3;
                    if (picker_top < 0) {
                        picker_top = 0;
                    }
                    if (picker_top >= picker.count) {
                        picker_top = picker.count ? picker.count - 1 : 0;
                    }
                    changed = 1;
                    continue;
                }
                if (dialog) {
                    continue;
                }
                if (e.a < edit_x) {
                    tree_top += e.wheel * 3;
                    if (tree_top < 0) {
                        tree_top = 0;
                    }
                    if (tree_top >= state->tree_count) {
                        tree_top = state->tree_count ? state->tree_count - 1 : 0;
                    }
                } else if (e.b >= out_y) {
                    output_top += e.wheel * 3;
                    if (output_top < 0) {
                        output_top = 0;
                    }
                } else if (document()) {
                    st_engine_viewport(engine, document()->top + e.wheel * 3, document()->left);
                }
                st_engine_dismiss_completion(engine);
                changed = 1;
                continue;
            }
            if (e.type == EV_KEY) {
                if (dialog) {
                    if ((dialog == 2 || dialog == 3) && (e.a == '\r' || e.a == '\n')) {
                        find_next();
                        changed = 1;
                        continue;
                    }
                    if (e.a == 27) {
                        dialog = focus = 0;
                        changed = 1;
                        continue;
                    }
                    aui_feed(&e);
                    draw();
                    aui_feed_done();
                    changed = 0;
                    continue;
                }
                int can_fast = !context_open && !state->completion_count && e.a != 19 &&
                               e.a != 18 && e.a != KEY_F5 && e.a != KEY_F6 && e.a != KEY_F8;
                key_event((int)e.a, e.mods);
                if (!can_fast || dialog || state->completion_count) {
                    changed = 1;
                } else if (changed != 1) {
                    changed = 2;
                }
                continue;
            }
            if ((e.type == EV_MOUSE_R || (e.type == EV_MOUSE && e.button == EV_BTN_RIGHT)) &&
                !dialog && e.a < edit_x && e.b >= ST_TREE_Y) {
                completion_due = 0;
                st_engine_dismiss_completion(engine);
                int at = tree_top + (e.b - ST_TREE_Y) / ST_ROW;
                select_tree(at < state->tree_count ? at : -1);
                context_open = 1;
                changed = 1;
                continue;
            }
            if (dialog) {
                aui_feed(&e);
                if (aui_want_repaint() || changed) {
                    draw();
                    changed = 0;
                }
                aui_feed_done();
                continue;
            }
            if (context_open && e.type == EV_MOUSE) {
                if (e.a < edit_x && e.b >= ST_TREE_Y && e.b < ST_TREE_Y + 96) {
                    int item = (e.b - ST_TREE_Y) / 32;
                    if (item == 2) {
                        delete_entry_dialog();
                    } else {
                        new_entry(item == 1);
                    }
                    changed = 1;
                    continue;
                }
                context_open = 0;
                changed = 1;
                continue;
            }
            if (e.type == EV_MOUSE) {
                completion_due = 0;
                changed = 1;
                int x = (int)e.a, y = (int)e.b;
                if (click_completion(x, y)) {
                    dragging = 0;
                    draw();
                    changed = 0;
                    continue;
                }
                if (x < edit_x && y >= ST_TREE_Y && y < aui_height() - 30) {
                    int index = tree_top + (y - ST_TREE_Y) / ST_ROW;
                    select_tree(index < state->tree_count ? index : -1);
                    if (tree_selected >= 0) {
                        if (state->files[index].directory) {
                            st_engine_toggle_folder(engine, index);
                        } else {
                            open_document(state->files[index].path);
                        }
                    }
                    focus = 0;
                } else if (y >= 42 && y < 72 && x >= edit_x && x < edit_x + edit_w &&
                           state->count) {
                    int tabw = edit_w / state->count;
                    if (tabw > 180) {
                        tabw = 180;
                    }
                    int index = (x - edit_x) / tabw;
                    if (index < state->count) {
                        if ((x - edit_x) % tabw >= tabw - 20) {
                            close_tab(index);
                        } else {
                            st_engine_activate(engine, index);
                            st_engine_dismiss_completion(engine);
                        }
                    }
                    focus = 0;
                } else if (x >= edit_x && x < edit_x + edit_w && y >= edit_y && y < out_y) {
                    place_caret(x, y, e.mods & EV_MOD_SHIFT);
                    dragging = 1;
                } else if (state->runner.mode && x >= edit_x && x < edit_x + edit_w &&
                           y >= out_y + 35) {
                    select_problem((y - out_y - 35) / ST_ROW + output_top);
                }
            }
            if (e.type == EV_MOUSE_UP) {
                dragging = 0;
            }
            if (e.type == EV_MOUSE_MOVE && dragging && e.a >= edit_x && e.a < edit_x + edit_w &&
                e.b >= edit_y && e.b < out_y) {
                place_caret(e.a, e.b, 1);
                changed = 1;
            }
            /* Immediate-mode controls must consume each press, but a pointer
             * moving across code is not a reason to paint the whole window.
             * A consumed frame also clears dirty: do not paint it twice. */
            aui_feed(&e);
            if (changed || aui_want_repaint()) {
                draw();
                changed = 0;
            }
            aui_feed_done();
        }
        /* Drain input before timers so an already queued key can postpone
         * completion/checkpoint/check work instead of waiting behind it. */
        if (completion_due && monotonic_ms() >= completion_due) {
            complete();
            changed = 1;
        }
        if (st_engine_tick(engine)) {
            changed = 1;
        }
        if (changed == 2 && !dialog && !context_open && document()) {
            draw_editor();
        } else if (changed) {
            draw();
        }
        int delay = st_engine_wait_ms(engine);
        if (completion_due && (!delay || delay > 50)) {
            delay = 50;
        }
        wait_idle(delay);
    }
}

const char *ag_gui_context(unsigned *bytes)
{
    const StDocument *d = document();
    *bytes = d ? (unsigned)d->length : 0;
    return d ? d->text : "";
}
