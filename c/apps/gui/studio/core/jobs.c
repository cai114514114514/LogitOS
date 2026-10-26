/* SPDX-License-Identifier: MIT */
#include "internal.h"

void st_engine_start(StEngine *e, int check)
{
    StDocument *d = st_current(e);
    if (!d) {
        e->check_pending = 0;
        return;
    }
    if (st_runner_busy(&e->state.runner)) {
        if (check) {
            e->check_pending = 1;
        } else {
            st_engine_notice(e, "启动新进程前请先停止正在运行的进程。");
        }
        return;
    }
    if (!check) {
        for (int i = 0; i < e->state.count; i++) {
            if (st_dirty(&e->state.docs[i])) {
                int r = st_save_document(&e->state.docs[i]);
                if (r < 0) {
                    e->state.active = i;
                    st_engine_notice(e, r == -2 ? "运行已停止：磁盘上的文件已更改。"
                                                : "运行已停止：有文档无法存储。");
                    return;
                }
            }
        }
    }
    if (st_runner_start(&e->state.runner, e->state.docs, e->state.count, e->state.active, check,
                        e->compiler, e->state.project) < 0) {
        st_engine_notice(e, "无法启动编译器进程。");
        e->check_pending = 0;
        return;
    }
    e->runner_seen = 0;
    e->check_pending = 0;
    /* Background checking starts after a typing pause, while the user is
     * choosing a completion. It does not change the source or caret, so it
     * must not dismiss that choice. Edits and navigation invalidate it. */
    if (!check) {
        st_engine_dismiss_completion(e);
    }
    st_engine_notice(e, check ? "正在检查 A3 项目快照…" : "正在运行 /bin/as…");
}
