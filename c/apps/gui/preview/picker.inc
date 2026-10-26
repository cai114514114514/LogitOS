/* Launched from the Dock there is no file to open, so offer what is there:
 * /media is where the shipped clips live. One level of subdirectory too,
 * because the image fixtures ride in /media/img.
 *
 * The list is also PRINTED, with its indices. A keyboard-driven list is the
 * only part of this app a test can steer, and a test that counts Down presses
 * against a directory order it guessed is a test that breaks the day a file is
 * added. So the app says what it is showing. */
#define PICK_MAX  48
#define PICK_Y0 112
#define PICK_DY 36
#define PICK_ROWS ((WINH - PICK_Y0 - 54) / PICK_DY)
static char pick_names[PICK_MAX][80];
static int  pick_n;

static void pick_scan(const char *dir, const char *prefix, int depth)
{
    int cnt = dir_count(dir);
    for (int i = 0; i < cnt && pick_n < PICK_MAX; i++) {
        char b[64];
        int rc = dir_name(dir, i, b);
        if (rc == -1) continue;
        if (rc == -2) {
            if (depth <= 0) continue;
            char sub[160], sp[64];
            snprintf(sub, sizeof sub, "%s/%s", dir, b);
            snprintf(sp, sizeof sp, "%s%s/", prefix, b);
            pick_scan(sub, sp, depth - 1);
            continue;
        }
        char nm[80];
        snprintf(nm, sizeof nm, "%s%s", prefix, b);
        int dup = 0;
        for (int k = 0; k < pick_n; k++) if (!strcmp(pick_names[k], nm)) { dup = 1; break; }
        if (!dup) snprintf(pick_names[pick_n++], sizeof pick_names[0], "%s", nm);
    }
}

/* THE SCAN IS RUN SEVERAL TIMES AND THE RESULTS MERGED -- entries are added,
 * never dropped -- and that is a workaround for a bug one layer down rather
 * than defensive habit.
 *
 * logitfs's dir_count_live() and dir_nth() walk a directory's blocks and
 * `break` out of the loop when bread() fails, so a block read that loses a
 * race does not surface as an error: it surfaces as a SHORTER DIRECTORY. A
 * /media of fifteen entries spans two 4 KiB blocks at ten dirents each, and
 * when the second one is lost the app is told there are ten files and has no
 * way to know otherwise. Preview meets this and `ls` does not, because Preview
 * enumerates in the first second after being launched -- exactly when the
 * compositor is busiest -- while `ls` runs at an idle prompt. Measured on
 * commit 13756e78: `ls /media` lists 15, Preview's dir_count answers 10 or 15
 * depending on the run.
 *
 * Handed to the filesystem line. Merging repeated reads is not a fix; it just
 * stops the file list being silently wrong. The list is also rebuilt every
 * time it is shown, so returning to it is another chance. */
static void pick_build(void)
{
    for (int t = 0; t < 6; t++) {
        int before = pick_n;
        pick_scan("/media", "", 1);
        if (t && pick_n == before) break;      /* two reads agreed */
    }
}

static int pick_kind[PICK_MAX], pick_ready[PICK_MAX];
static long pick_bytes[PICK_MAX];
static const char *pick_format[PICK_MAX];
static const char *const pick_groups[] = {"All media", "Images", "Audio", "Video & containers", "Other files"};
static const unsigned pick_colors[] = {0x658EFF, 0x568DF0, 0xA37DE8, 0xD89B54, 0x8C98AB};

static void pick_metadata(void)
{
    for (int i = 0; i < pick_n; i++) {
        if (pick_ready[i]) continue;
        char path[192]; unsigned char bytes[SNIFF_PREFIX];
        snprintf(path, sizeof path, "/media/%s", pick_names[i]);
        FILE *file = fopen(path, "rb");
        pick_format[i] = "Unavailable"; pick_kind[i] = 4;
        if (!file) continue;
        fseek(file, 0, SEEK_END); pick_bytes[i] = ftell(file); rewind(file);
        int n = (int)fread(bytes, 1, sizeof bytes, file);
        fclose(file);
        int kind = sniff_id(bytes, n), group = sniff_class(kind);
        pick_format[i] = is_ico(bytes, n) ? "ICO/CUR" : sniff_name(kind);
        /* Classification is deliberately header-based like the player. A
         * renamed PNG belongs under Images even when its suffix says .mp4.
         * Containers stay a combined group: their headers alone do not prove
         * they contain a video track (an M4A may contain only audio). */
        if (media_sniff(bytes, n) != MEDIA_CONT_UNKNOWN) pick_kind[i] = 3;
        else if (is_image_magic(bytes, n)) pick_kind[i] = 1;
        else {
            audio_format audio = audio_sniff(bytes, n);
            pick_kind[i] = audio != AUDIO_FMT_UNKNOWN ? 2 : group == SNC_VIDEO ? 3 : 4;
            /* The generic terminal sniffer calls some ADTS headers MP3.
             * Ask the player's audio library so its label agrees with playback. */
            if (audio != AUDIO_FMT_UNKNOWN) pick_format[i] = fmt_name(audio);
        }
        pick_ready[i] = 1;
    }
}
static void pick_frame(const int *items, int count, int selected, int top, int filter)
{
    view_theme(); gui_clear(ui_bg);
    gui_rect(0, 0, 164, WINH, ui_panel);
    gui_rect(163, 0, 1, WINH, ui_line);
    /* The same packaged raster asset is used by the Dock and Finder. Keep
     * the native fallback usable when opening an older disk without artwork. */
    if (gui_app_icon("preview.aex", 16, 18, 36) < 0)
        gui_icon(GICON_IMAGE, 20, 24, 26, ui_accent);
    label(60, 23, 20, ui_text, "Preview");
    label(20, 76, 10, ui_muted, "LIBRARY");
    for (int group = 0; group < 5; group++) {
        int y = 102 + group * 42;
        if (group == filter) gui_rrect(10, y, 144, 34, 8, view_dark ? 0x344365 : 0xE4ECFE);
        clipped_label(22, y + 10, 132, 12, group == filter ? ui_accent : ui_text, pick_groups[group]);
    }
    label(20, WINH - 50, 11, ui_muted, "Local collection");
    label(20, WINH - 30, 11, ui_muted, "/media");
    label(188, 25, 24, ui_text, pick_groups[filter]);
    char info[96];
    snprintf(info, sizeof info, "%d files   /   Select a file to open", count);
    label(188, 59, 12, ui_muted, info);
    label(204, 91, 10, ui_muted, "NAME");
    label(WINW - 144, 91, 10, ui_muted, "FORMAT / SIZE");
    plog("preview: library filter=%d top=%d rows=%d count=%d\n", filter, top, PICK_ROWS, count);
    for (int row = 0; row < PICK_ROWS && top + row < count; row++) {
        int idx = items[top + row], y = PICK_Y0 + row * PICK_DY;
        if (top + row == selected)
            gui_rrect(184, y, WINW - 204, 34, 7, view_dark ? 0x344365 : 0xE4ECFE);
        else if (row % 2 == 0) gui_rrect(184, y, WINW - 204, 34, 7, ui_panel);
        gui_rrect(194, y + 7, 22, 22, 6, pick_colors[pick_kind[idx]]);
        gui_icon(pick_kind[idx] == 1 ? GICON_IMAGE : GICON_DOC, 198, y + 11, 14, 0xFFFFFF);
        clipped_label(226, y + 10, WINW - 392, 13, ui_text, pick_names[idx]);
        snprintf(info, sizeof info, "%s / %ld KB", pick_format[idx], (pick_bytes[idx] + 1023) / 1024);
        clipped_label(WINW - 146, y + 12, 126, 10, ui_muted, info);
        plog("preview: row %d %d %d %d %d\n", idx, 184, y, WINW - 204, 34);
    }
    if (!count) {
        label(212, 180, 19, ui_text, "No files in this category");
        label(212, 218, 12, ui_muted, "Choose All media to browse the collection.");
    }
    if (count > PICK_ROWS) {
        int h = PICK_ROWS * PICK_DY;
        gui_rrect(WINW - 12, PICK_Y0, 4, h, 2, ui_line);
        int thumb = h * PICK_ROWS / count;
        gui_rrect(WINW - 12, PICK_Y0 + (h - thumb) * top / (count - PICK_ROWS), 4, thumb, 2, ui_accent);
    }
    gui_rect(184, WINH - 44, WINW - 204, 1, ui_line);
    label(188, WINH - 28, 11, ui_muted, "Click to open   /   Arrow keys to select   /   Enter to open");
    /* List-only probe: never paint an instrument over the viewed artwork.
     * Only on a disk that asks for it: qmp_probe.h. */
    if (qmp_probes_on()) gui_rect(0, 0, 6, 6, 0xFF0080);
    gui_flush();
}
static int pick_from_media(char *out, int outmax)
{
    pick_build(); pick_metadata();
    static int selected, filter;
    int top = 0;
    for (int i = 0; i < pick_n; i++) plog("preview: pick %d %s\n", i, pick_names[i]);
    for (;;) {
        int items[PICK_MAX], count = 0;
        for (int i = 0; i < pick_n; i++) if (!filter || filter == pick_kind[i]) items[count++] = i;
        if (selected >= count) selected = count ? count - 1 : 0;
        if (selected < top) top = selected;
        if (selected >= top + PICK_ROWS) top = selected - PICK_ROWS + 1;
        if (top > count - PICK_ROWS) top = count - PICK_ROWS;
        if (top < 0) top = 0;
        pick_frame(items, count, selected, top, filter);
        struct logit_event e;
        /* Old code yielded continuously to work around a historical lost-wake
         * bug. The window event wait now preserves queued input and avoids an
         * idle library continuously entering the kernel. Guest navigation is
         * the acceptance check for this change, not a host timer. */
        for (;;) {
            while (!poll_event(&e)) wait_idle(100);
            if (ag_gui_event(&e)) continue;
            /* No hover state is painted here. Pointer reports alone must not
             * redraw the whole collection or repeat its diagnostic inventory. */
            if (e.type == EV_MOUSE_MOVE || e.type == EV_MOUSE_UP) continue;
            break;
        }
        if (e.type == EV_CLOSE || (e.type == EV_KEY && e.a == 27)) return 1;
        if (e.type == EV_RESIZE) { view_width = e.a; view_height = e.b; }
        int open = 0;
        if (e.type == EV_MOUSE) {
            plog("preview: click %d %d\n", e.a, e.b);
            if (e.a < 164 && e.b >= 102 && e.b < 312) {
                filter = (e.b - 102) / 42; selected = top = 0;
            } else if (e.a >= 184 && e.a < WINW - 20 && e.b >= PICK_Y0) {
                int row = (e.b - PICK_Y0) / PICK_DY;
                if (row < PICK_ROWS && top + row < count) { selected = top + row; open = 1; }
            }
        }
        if (e.type == EV_WHEEL) {
            selected -= e.wheel * 3;
            if (selected < 0) selected = 0;
        }
        if (e.type == EV_KEY) {
            if (e.a == KEY_UP && selected > 0) selected--;
            if (e.a == KEY_DOWN && selected < count - 1) selected++;
            if (e.a == KEY_HOME) selected = 0;
            if (e.a == KEY_END) selected = count - 1;
            if (e.a == KEY_PGDN) selected += PICK_ROWS;
            if (e.a == KEY_PGUP) { selected -= PICK_ROWS; if (selected < 0) selected = 0; }
            if (e.a == '\n' || e.a == '\r') open = 1;
        }
        if (open && count) {
            snprintf(out, outmax, "/media/%s", pick_names[items[selected]]);
            return 0;
        }
    }
}
