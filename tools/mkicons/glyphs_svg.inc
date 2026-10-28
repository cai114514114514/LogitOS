/* glyphs_svg.inc -- the 13 icon glyphs as SVG path data (design_language.md 3.7.4, 3.7.5).
 *
 * GENERATED ONCE from a helper that computed the gear, dial ticks and circles; from
 * here the strings are the source of truth: edit them, rebuild, look at the sheet
 * (`make BUILD=build $(BUILD)/icons/sheet.png`). Included by mkicons.c only.
 *
 * Every path lives in a 100 x 100 box, y down. The renderer places that box at 56 % of
 * the 256 px tile, centred (143.36 px). A layer is one fill or one stroke:
 *   d        path data (M L H V C Q Z, absolute and relative; see svgpath.c)
 *   role     R_INK = the app's glyph colour, R_ACCENT = its one coloured detail,
 *            R_RGB = .rgb (tabulated per-glyph art colour)
 *   alpha    0..255, 0 means 255;  stroke > 0 strokes the path (round caps, round
 *            joins) with that width in box units, else the path is filled
 *   grad     "y0, y1": a vertical gradient between those box y values, from .rgb /
 *            .alpha to .rgb2 / .alpha2 (role colours are used when .rgb is 0)
 *   rot sc tx ty  rotation (degrees, clockwise), scale (0 means 1) about the box
 *            centre, then translation, all in box units. */

static const struct glyph_layer GL_NONE[] = { { 0 } };

static const struct glyph_layer GL_FOLDER_TWOTONE[] = {
    { .d = "M8 22 Q8 15 15 15 H37 Q41 15 43.5 17.5 L48 22 H85 Q92 22 92 29 V74 Q92 82 84 82 H16 Q8 82 8 74 Z",
      .role = R_INK,
      .alpha = 170 },
    { .d = "M8 36 Q8 31 14 31 H86 Q92 31 92 37 V75 Q92 83 85 83 H15 Q8 83 8 75 Z",
      .role = R_RGB,
      .rgb = 0xFFFFFF,
      .rgb2 = 0xD6E8FF,
      .grad = 1, .gy0 = 30, .gy1 = 84 },
    { 0 }
};

static const struct glyph_layer GL_GEAR_RING[] = {
    { .d = "M42.86 17.78 L44.26 6.38 L55.74 6.38 L57.14 17.78 L60.83 18.83 L64.38 20.3 L67.73 22.17 L76.79 15.09 L84.91 23.21 L77.83 32.27 L79.7 35.62 L81.17 39.17 L82.22 42.86 L93.62 44.26 L93.62 55.74 L82.22 57.14 L81.17 60.83 L79.7 64.38 L77.83 67.73 L84.91 76.79 L76.79 84.91 L67.73 77.83 L64.38 79.7 L60.83 81.17 L57.14 82.22 L55.74 93.62 L44.26 93.62 L42.86 82.22 L39.17 81.17 L35.62 79.7 L32.27 77.83 L23.21 84.91 L15.09 76.79 L22.17 67.73 L20.3 64.38 L18.83 60.83 L17.78 57.14 L6.38 55.74 L6.38 44.26 L17.78 42.86 L18.83 39.17 L20.3 35.62 L22.17 32.27 L15.09 23.21 L23.21 15.09 L32.27 22.17 L35.62 20.3 L39.17 18.83 Z M69 50 C69 60.49 60.49 69 50 69 C39.51 69 31 60.49 31 50 C31 39.51 39.51 31 50 31 C60.49 31 69 39.51 69 50 Z M60.5 50 C60.5 55.8 55.8 60.5 50 60.5 C44.2 60.5 39.5 55.8 39.5 50 C39.5 44.2 44.2 39.5 50 39.5 C55.8 39.5 60.5 44.2 60.5 50 Z",
      .role = R_INK,
      .evenodd = 1 },
    { 0 }
};

static const struct glyph_layer GL_GLOBE_COMPASS[] = {
    { .d = "M90 50 C90 72.09 72.09 90 50 90 C27.91 90 10 72.09 10 50 C10 27.91 27.91 10 50 10 C72.09 10 90 27.91 90 50 Z",
      .role = R_INK,
      .alpha = 235,
      .stroke = 6 },
    { .d = "M50 10 C32 24 32 76 50 90 C68 76 68 24 50 10 Z",
      .role = R_INK,
      .alpha = 200,
      .stroke = 4 },
    { .d = "M10 50 H90 M17 30 H83 M17 70 H83",
      .role = R_INK,
      .alpha = 200,
      .stroke = 4 },
    { .d = "M50 14 V86",
      .role = R_INK,
      .alpha = 170,
      .stroke = 3.4 },
    { .d = "M50 18 L61 50 L50 82 L39 50 Z",
      .role = R_RGB,
      .rgb = 0x4A82FF,
      .stroke = 7.5,
      .rot = 45 },
    { .d = "M50 18 L60.5 50 L39.5 50 Z",
      .role = R_ACCENT,
      .rot = 45 },
    { .d = "M39.5 50 L60.5 50 L50 82 Z",
      .role = R_INK,
      .rot = 45 },
    { 0 }
};

static const struct glyph_layer GL_DIAL[] = {
    { .d = "M92 50 C92 73.2 73.2 92 50 92 C26.8 92 8 73.2 8 50 C8 26.8 26.8 8 50 8 C73.2 8 92 26.8 92 50 Z",
      .role = R_INK,
      .alpha = 235,
      .stroke = 4 },
    { .d = "M50 19 L50 13.5 M66.75 20.99 L68.25 18.39 M79.01 33.25 L81.61 31.75 M81 50 L86.5 50 M79.01 66.75 L81.61 68.25 M66.75 79.01 L68.25 81.61 M50 81 L50 86.5 M33.25 79.01 L31.75 81.61 M20.99 66.75 L18.39 68.25 M19 50 L13.5 50 M20.99 33.25 L18.39 31.75 M33.25 20.99 L31.75 18.39 ",
      .role = R_INK,
      .stroke = 3 },
    { .d = "M50 50 L32.59 38.26",
      .role = R_INK,
      .stroke = 5.4 },
    { .d = "M50 50 L72.29 29.93",
      .role = R_INK,
      .stroke = 4 },
    { .d = "M53.08 41.54 L38.71 81.01",
      .role = R_ACCENT,
      .stroke = 2.2 },
    { .d = "M54.4 50 C54.4 52.43 52.43 54.4 50 54.4 C47.57 54.4 45.6 52.43 45.6 50 C45.6 47.57 47.57 45.6 50 45.6 C52.43 45.6 54.4 47.57 54.4 50 Z",
      .role = R_ACCENT },
    { 0 }
};

/* 活动监视器: a centred sparkline (optical bbox on the tile centre), the line ~5 % of the
 * tile so it reads at 54 px, its fill strong enough to show at that size. */
static const struct glyph_layer GL_SPARKLINE[] = {
    { .d = "M8 31 H92 M8 53 H92 M8 75 H92",
      .role = R_INK,
      .alpha = 34,
      .stroke = 1.8 },
    { .d = "M12 69 L27.5 55 L40.5 62 L56.5 39 L69.5 49 L85.5 31 L85.5 84 L12 84 Z",
      .role = R_ACCENT,
      .alpha = 170,
      .rgb2 = 0x34D17A,
      .fade = 1,
      .grad = 1, .gy0 = 31, .gy1 = 84 },
    { .d = "M12 69 L27.5 55 L40.5 62 L56.5 39 L69.5 49 L85.5 31",
      .role = R_ACCENT,
      .stroke = 9 },
    { .d = "M93 31 C93 35.14 89.64 38.5 85.5 38.5 C81.36 38.5 78 35.14 78 31 C78 26.86 81.36 23.5 85.5 23.5 C89.64 23.5 93 26.86 93 31 Z",
      .role = R_INK },
    { .d = "M89.5 31 C89.5 33.21 87.71 35 85.5 35 C83.29 35 81.5 33.21 81.5 31 C81.5 28.79 83.29 27 85.5 27 C87.71 27 89.5 28.79 89.5 31 Z",
      .role = R_ACCENT },
    { 0 }
};

/* 预览: two tilted photos filling the 56 % box (3.7.4). Each has a soft two-layer contact
 * shadow and a #1D1D23 @ 40 outline so the white frames stay distinct from the white tile
 * at 32-54 px, and thin polaroid borders so the saturated sky and sunset carry small sizes. */
static const struct glyph_layer GL_PHOTOS[] = {
    { .d = "M26 18 H74 Q79 18 79 23 V77 Q79 82 74 82 H26 Q21 82 21 77 V23 Q21 18 26 18 Z",
      .role = R_RGB,
      .rgb = 0x000000,
      .alpha = 16,
      .stroke = 5,
      .rot = -8,
      .sc = 1.12,
      .tx = -11,
      .ty = -0.5 },
    { .d = "M26 18 H74 Q79 18 79 23 V77 Q79 82 74 82 H26 Q21 82 21 77 V23 Q21 18 26 18 Z",
      .role = R_RGB,
      .rgb = 0x000000,
      .alpha = 40,
      .rot = -8,
      .sc = 1.12,
      .tx = -11,
      .ty = -0.8 },
    { .d = "M26 18 H74 Q79 18 79 23 V77 Q79 82 74 82 H26 Q21 82 21 77 V23 Q21 18 26 18 Z",
      .role = R_RGB,
      .rgb = 0xFFFFFF,
      .rot = -8,
      .sc = 1.12,
      .tx = -11,
      .ty = -2 },
    { .d = "M26 18 H74 Q79 18 79 23 V77 Q79 82 74 82 H26 Q21 82 21 77 V23 Q21 18 26 18 Z",
      .role = R_INK,
      .alpha = 40,
      .stroke = 1.4,
      .rot = -8,
      .sc = 1.12,
      .tx = -11,
      .ty = -2 },
    { .d = "M27.5 22 H72.5 Q75 22 75 24.5 V70.5 Q75 73 72.5 73 H27.5 Q25 73 25 70.5 V24.5 Q25 22 27.5 22 Z",
      .role = R_RGB,
      .rgb = 0x4FC3FF,
      .rgb2 = 0x1F6FEB,
      .rot = -8,
      .sc = 1.12,
      .tx = -11,
      .ty = -2,
      .grad = 1, .gy0 = 22, .gy1 = 73 },
    { .d = "M25 61 L37 47 L47 57 L56 49 L75 65 V70.5 Q75 73 72.5 73 H27.5 Q25 73 25 70.5 Z",
      .role = R_RGB,
      .rgb = 0x1B7F45,
      .rot = -8,
      .sc = 1.12,
      .tx = -11,
      .ty = -2 },
    { .d = "M67.5 32.5 C67.5 35.26 65.26 37.5 62.5 37.5 C59.74 37.5 57.5 35.26 57.5 32.5 C57.5 29.74 59.74 27.5 62.5 27.5 C65.26 27.5 67.5 29.74 67.5 32.5 Z",
      .role = R_RGB,
      .rgb = 0xFFFFFF,
      .alpha = 235,
      .rot = -8,
      .sc = 1.12,
      .tx = -11,
      .ty = -2 },
    { .d = "M26 18 H74 Q79 18 79 23 V77 Q79 82 74 82 H26 Q21 82 21 77 V23 Q21 18 26 18 Z",
      .role = R_RGB,
      .rgb = 0x000000,
      .alpha = 16,
      .stroke = 5,
      .rot = 6,
      .sc = 1.12,
      .tx = 11,
      .ty = 4.5 },
    { .d = "M26 18 H74 Q79 18 79 23 V77 Q79 82 74 82 H26 Q21 82 21 77 V23 Q21 18 26 18 Z",
      .role = R_RGB,
      .rgb = 0x000000,
      .alpha = 40,
      .rot = 6,
      .sc = 1.12,
      .tx = 11,
      .ty = 4.2 },
    { .d = "M26 18 H74 Q79 18 79 23 V77 Q79 82 74 82 H26 Q21 82 21 77 V23 Q21 18 26 18 Z",
      .role = R_RGB,
      .rgb = 0xFFFFFF,
      .rot = 6,
      .sc = 1.12,
      .tx = 11,
      .ty = 3 },
    { .d = "M26 18 H74 Q79 18 79 23 V77 Q79 82 74 82 H26 Q21 82 21 77 V23 Q21 18 26 18 Z",
      .role = R_INK,
      .alpha = 40,
      .stroke = 1.4,
      .rot = 6,
      .sc = 1.12,
      .tx = 11,
      .ty = 3 },
    { .d = "M27.5 22 H72.5 Q75 22 75 24.5 V70.5 Q75 73 72.5 73 H27.5 Q25 73 25 70.5 V24.5 Q25 22 27.5 22 Z",
      .role = R_RGB,
      .rgb = 0xFFC15C,
      .rgb2 = 0xFF4F6E,
      .rot = 6,
      .sc = 1.12,
      .tx = 11,
      .ty = 3,
      .grad = 1, .gy0 = 22, .gy1 = 73 },
    { .d = "M25 61 L37 47 L47 57 L56 49 L75 65 V70.5 Q75 73 72.5 73 H27.5 Q25 73 25 70.5 Z",
      .role = R_RGB,
      .rgb = 0x6B2A8F,
      .rot = 6,
      .sc = 1.12,
      .tx = 11,
      .ty = 3 },
    { .d = "M67.5 32.5 C67.5 35.26 65.26 37.5 62.5 37.5 C59.74 37.5 57.5 35.26 57.5 32.5 C57.5 29.74 59.74 27.5 62.5 27.5 C65.26 27.5 67.5 29.74 67.5 32.5 Z",
      .role = R_RGB,
      .rgb = 0xFFFFFF,
      .alpha = 240,
      .rot = 6,
      .sc = 1.12,
      .tx = 11,
      .ty = 3 },
    { 0 }
};

static const struct glyph_layer GL_CODE[] = {
    { .d = "M34 29 L13 50 L34 71",
      .role = R_INK,
      .stroke = 8.4 },
    { .d = "M66 29 L87 50 L66 71",
      .role = R_INK,
      .stroke = 8.4 },
    { .d = "M57.5 22 L42.5 78",
      .role = R_INK,
      .stroke = 8.4 },
    { 0 }
};

static const struct glyph_layer GL_LOGIT_SPARKLE[] = {
    { .d = "M91 50 C91 72.64 72.64 91 50 91 C27.36 91 9 72.64 9 50 C9 27.36 27.36 9 50 9 C72.64 9 91 27.36 91 50 Z",
      .role = R_INK,
      .stroke = 11.4,
      .sc = 0.84,
      .tx = -7,
      .ty = 7 },
    { .d = "M24.4 70.5 C42.3 70.5 45.5 60.2 50 50 C54.5 39.8 57.7 29.5 75.6 29.5",
      .role = R_INK,
      .stroke = 12.6,
      .sc = 0.84,
      .tx = -7,
      .ty = 7 },
    { .d = "M80 3 Q80 22 99 22 Q80 22 80 41 Q80 22 61 22 Q80 22 80 3 Z",
      .role = R_INK },
    { 0 }
};

static const struct glyph_layer GL_PROMPT[] = {
    { .d = "M20 25 L49 50 L20 75",
      .role = R_INK,
      .stroke = 12.5 },
    { .d = "M61 61 H86 Q90 61 90 65 V71 Q90 75 86 75 H61 Q57 75 57 71 V65 Q57 61 61 61 Z",
      .role = R_ACCENT },
    { 0 }
};

static const struct glyph_layer GL_RULED_PEN[] = {
    { .d = "M19 8 H63 Q70 8 70 15 V85 Q70 92 63 92 H19 Q12 92 12 85 V15 Q12 8 19 8 Z",
      .role = R_INK,
      .alpha = 90,
      .stroke = 3 },
    { .d = "M19 8 H63 Q70 8 70 15 V85 Q70 92 63 92 H19 Q12 92 12 85 V15 Q12 8 19 8 Z",
      .role = R_RGB,
      .rgb = 0xFFFFFF },
    { .d = "M24 26 H47",
      .role = R_INK,
      .alpha = 170,
      .stroke = 4.6 },
    { .d = "M24 43 H59 M24 58 H59 M24 73 H44",
      .role = R_INK,
      .alpha = 100,
      .stroke = 3.4 },
    { .d = "M42 4 L58 4 L58 64 L50 80 L42 64 Z",
      .role = R_RGB,
      .rgb = 0xFFFFFF,
      .stroke = 9,
      .rot = 38,
      .tx = 23,
      .ty = 11 },
    { .d = "M42 6 Q42 4 44 4 H56 Q58 4 58 6 V19 H42 Z",
      .role = R_INK,
      .alpha = 235,
      .rot = 38,
      .tx = 23,
      .ty = 11 },
    { .d = "M42 19 H58 V63 H42 Z",
      .role = R_ACCENT,
      .rot = 38,
      .tx = 23,
      .ty = 11 },
    { .d = "M42 63 H58 L50 80 Z",
      .role = R_RGB,
      .rgb = 0xF2C99A,
      .rot = 38,
      .tx = 23,
      .ty = 11 },
    { .d = "M46.8 72.5 H53.2 L50 80 Z",
      .role = R_INK,
      .rot = 38,
      .tx = 23,
      .ty = 11 },
    { 0 }
};

static const struct glyph_layer GL_GRID4[] = {
    { .d = "M26.5 18 H38.5 Q47 18 47 26.5 V38.5 Q47 47 38.5 47 H26.5 Q18 47 18 38.5 V26.5 Q18 18 26.5 18 Z",
      .role = R_INK },
    { .d = "M61.5 18 H73.5 Q82 18 82 26.5 V38.5 Q82 47 73.5 47 H61.5 Q53 47 53 38.5 V26.5 Q53 18 61.5 18 Z",
      .role = R_INK,
      .alpha = 225 },
    { .d = "M26.5 53 H38.5 Q47 53 47 61.5 V73.5 Q47 82 38.5 82 H26.5 Q18 82 18 73.5 V61.5 Q18 53 26.5 53 Z",
      .role = R_INK,
      .alpha = 225 },
    { .d = "M61.5 53 H73.5 Q82 53 82 61.5 V73.5 Q82 82 73.5 82 H61.5 Q53 82 53 73.5 V61.5 Q53 53 61.5 53 Z",
      .role = R_INK },
    { 0 }
};

static const struct glyph_layer GL_TILES3[] = {
    { .d = "M26 16 H46 Q56 16 56 26 V74 Q56 84 46 84 H26 Q16 84 16 74 V26 Q16 16 26 16 Z",
      .role = R_INK },
    { .d = "M70 16 H76 Q84 16 84 24 V40 Q84 48 76 48 H70 Q62 48 62 40 V24 Q62 16 70 16 Z",
      .role = R_INK,
      .alpha = 230 },
    { .d = "M70 52 H76 Q84 52 84 60 V76 Q84 84 76 84 H70 Q62 84 62 76 V60 Q62 52 70 52 Z",
      .role = R_INK,
      .alpha = 200 },
    { 0 }
};

static const struct glyph_layer GL_LOGIT_MARK[] = {
    { .d = "M91 50 C91 72.64 72.64 91 50 91 C27.36 91 9 72.64 9 50 C9 27.36 27.36 9 50 9 C72.64 9 91 27.36 91 50 Z",
      .role = R_INK,
      .stroke = 9.8 },
    { .d = "M24.4 70.5 C42.3 70.5 45.5 60.2 50 50 C54.5 39.8 57.7 29.5 75.6 29.5",
      .role = R_INK,
      .stroke = 11 },
    { 0 }
};

static const struct glyph_def { int id; const struct glyph_layer *layers; } GLYPH_DEFS[] = {
    { LOGIT_GLYPH_FOLDER_TWOTONE, GL_FOLDER_TWOTONE },
    { LOGIT_GLYPH_GEAR_RING, GL_GEAR_RING },
    { LOGIT_GLYPH_GLOBE_COMPASS, GL_GLOBE_COMPASS },
    { LOGIT_GLYPH_DIAL, GL_DIAL },
    { LOGIT_GLYPH_SPARKLINE, GL_SPARKLINE },
    { LOGIT_GLYPH_PHOTOS, GL_PHOTOS },
    { LOGIT_GLYPH_CODE, GL_CODE },
    { LOGIT_GLYPH_LOGIT_SPARKLE, GL_LOGIT_SPARKLE },
    { LOGIT_GLYPH_PROMPT, GL_PROMPT },
    { LOGIT_GLYPH_RULED_PEN, GL_RULED_PEN },
    { LOGIT_GLYPH_GRID4, GL_GRID4 },
    { LOGIT_GLYPH_TILES3, GL_TILES3 },
    { LOGIT_GLYPH_LOGIT_MARK, GL_LOGIT_MARK },
};
