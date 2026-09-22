/* ---------------------------------------------------------- audio UI ---- */
/* An audio-only file still has to put something on the screen, and "nothing"
 * is not it. Format, position against duration, and a level meter reading the
 * samples the card is actually being handed. */
/* Three segments, not a per-pixel loop: every gui_rect is an int 0x80, and a
 * 700-pixel bar drawn a column at a time would cost 700 syscalls per redraw. */
static void meter_bar(int x, int y, int w, int h, int peak)
{
    gui_rect(x, y, w, h, rgb(38, 38, 46));
    int fill = peak > 0 ? (int)((long)w * peak / 32767) : 0;
    if (fill > w) fill = w;
    int g = w * 70 / 100, amb = w * 90 / 100;
    if (fill > 0)   gui_rect(x, y, fill < g ? fill : g, h, rgb(70, 210, 120));
    if (fill > g)   gui_rect(x + g, y, (fill < amb ? fill : amb) - g, h, rgb(240, 200, 80));
    if (fill > amb) gui_rect(x + amb, y, fill - amb, h, rgb(240, 90, 80));
}

static void draw_audio_ui(aplay *a, const char *name, const char *what,
                          long long pos_ns, long long total_ns)
{
    char line[192];
    view_clear();
    int x = 48, y = 92, width = WINW - 96;
    gui_rrect(x, y, width, WINH - 168, 18, ui_panel);
    gui_rrect(x + 28, y + 24, 58, 58, 15, 0x745CB5);
    gui_icon(GICON_DOC, x + 45, y + 41, 24, 0xFFFFFF);
    label(x + 106, y + 27, 12, ui_muted, "AUDIO PLAYBACK");
    clipped_label(x + 106, y + 52, width - 136, 20, ui_text, name);
    snprintf(line, sizeof line, "%s   /   %d Hz   /   %s", what, a->rate,
             a->ch == 1 ? "Mono" : "Stereo");
    label(x + 28, y + 106, 13, ui_muted, line);

    long long p = pos_ns < 0 ? 0 : pos_ns;
    if (total_ns > 0 && p > total_ns) p = total_ns;
    snprintf(line, sizeof line, "%lld:%02lld", p / 60000000000LL, (p / 1000000000LL) % 60);
    label(x + 28, y + 149, 30, ui_text, line);
    if (total_ns > 0) {
        snprintf(line, sizeof line, "/ %lld:%02lld", total_ns / 60000000000LL,
                 (total_ns / 1000000000LL) % 60);
        label(x + 150, y + 162, 14, ui_muted, line);
    }
    int bw = width - 56;
    gui_rrect(x + 28, y + 199, bw, 6, 3, ui_line);
    if (total_ns > 0 && p > 0)
        gui_rrect(x + 28, y + 199, (int)((long long)bw * p / total_ns), 6, 3, ui_accent);

    int level_y = WINH - 178;
    label(x + 28, level_y, 10, ui_muted, "OUTPUT LEVEL");
    label(x + 28, level_y + 24, 11, ui_muted, a->ch > 1 ? "L" : "M");
    meter_bar(x + 52, level_y + 26, width - 80, 8, a->peak[0]);
    if (a->ch > 1) {
        label(x + 28, level_y + 44, 11, ui_muted, "R");
        meter_bar(x + 52, level_y + 46, width - 80, 8, a->peak[1]);
    }
    /* These bars consume actual decoded PCM peaks, never a decorative wave.
     * Progress is display-only: this decoder API has no reliable seek. */
    for (int c = 0; c < 2; c++) a->peak[c] = a->peak[c] * 3 / 4;
    if (a->why) clipped_label(x + 28, WINH - 98, width - 56, 11, 0xD99E64, a->why);
}
