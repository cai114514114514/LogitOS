/* SPDX-License-Identifier: GPL-3.0-or-later */
static void rgb_to_hsl(unsigned c, int *h, int *s, int *l)
{
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int L = (mx + mn) * 100 / 510;
    int S = 0, H = 0;
    if (mx != mn) {
        int d = mx - mn;
        S = (mx + mn) <= 255 ? d * 100 / (mx + mn) : d * 100 / (510 - mx - mn);
        if (mx == r)      H = (g - b) * 60 / d + (g < b ? 360 : 0);
        else if (mx == g) H = (b - r) * 60 / d + 120;
        else              H = (r - g) * 60 / d + 240;
    }
    *h = H < 0 ? H + 360 : H; *s = S; *l = L;
}

static unsigned accent_now(void) { return aui_hsl(v_accent_h, v_accent_s, v_accent_l); }
