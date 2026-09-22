/* HTTP static-file semantics shared by the guest server and host checks.
 * RFC 9110: ranges are optional; malformed/multipart/unknown ranges fall
 * back to full GET, while a valid but unsatisfiable byte range is 416. */
#ifndef LOGIT_HTTPD_PROTOCOL_H
#define LOGIT_HTTPD_PROTOCOL_H
static int hd_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int hd_equal(const char *a, const char *b)
{ while (*a && hd_lower(*a) == hd_lower(*b)) { a++; b++; } return *a == *b; }
static const char *mime_of(const char *path)
{
    const char *e = path;
    for (const char *p = path; *p; p++) if (*p == '.' || *p == '/') e = p + 1;
    static const struct { const char *ext, *type; } types[] = {
        {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"}, {"css", "text/css"},
        {"js", "text/javascript"}, {"mjs", "text/javascript"},
        {"json", "application/json"}, {"wasm", "application/wasm"},
        {"png", "image/png"}, {"gif", "image/gif"}, {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"}, {"svg", "image/svg+xml"}, {"webp", "image/webp"},
        {"ico", "image/vnd.microsoft.icon"}, {"pdf", "application/pdf"},
        {"mp4", "video/mp4"}, {"webm", "video/webm"}, {"mp3", "audio/mpeg"},
        {"wav", "audio/wav"}, {"ogg", "audio/ogg"}, {"woff", "font/woff"},
        {"woff2", "font/woff2"}, {"zip", "application/zip"}
    };
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++)
        if (hd_equal(e, types[i].ext)) return types[i].type;
    return "application/octet-stream";
}
static int hd_hex(int c)
{ c = hd_lower(c); return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : -1; }
static int safe_path(const char *root, const char *req, char *out, int max)
{
    if (*root != '/' || *req != '/' || max < 2) return -1;
    int n = 0;
    while (*root) { if (n >= max - 1) return -1; out[n++] = *root++; }
    while (n && out[n-1] == '/') n--;
    int base = n, seg = n + 1;
    for (int i = 0; req[i] && req[i] != '?' && req[i] != '#'; i++) {
        unsigned char c = (unsigned char)req[i];
        if (c == '%') {
            if (!req[i+1] || !req[i+2]) return -1;
            int a = hd_hex(req[i+1]), b = hd_hex(req[i+2]);
            if (a < 0 || b < 0) return -1;
            c = (unsigned char)(a * 16 + b); i += 2;
        }
        if (c < 32 || c == 127 || c == '\\') return -1;
        if (c == '/') {
            if (n-seg == 2 && out[seg] == '.' && out[seg+1] == '.') return -1;
            seg = n + 1;
        }
        if (n >= max - 1) return -1;
        out[n++] = (char)c;
    }
    if (n-seg == 2 && out[seg] == '.' && out[seg+1] == '.') return -1;
    if (n == base + 1 || (n > base && out[n-1] == '/')) {
        const char *index = "index.html";
        while (*index) { if (n >= max-1) return -1; out[n++] = *index++; }
    }
    out[n] = 0;
    return 0;
}
/* A complete head is bounded by the caller. Duplicate fields are ignored as
 * a unit rather than accidentally choosing one Range from several. */
static int hd_header(const char *req, const char *name, char *out, int cap)
{
    int found = 0;
    while (*req && *req != '\n') req++;
    while (*req) {
        req++;
        if (!*req || *req == '\r' || *req == '\n') break;
        const char *p = req, *k = name;
        while (*k && hd_lower(*p) == hd_lower(*k)) { p++; k++; }
        if (!*k && *p == ':') {
            if (found) return -1;
            p++; while (*p == ' ' || *p == '\t') p++;
            int n = 0;
            while (*p && *p != '\r' && *p != '\n') {
                if (n >= cap-1) return -1;
                out[n++] = *p++;
            }
            while (n && (out[n-1] == ' ' || out[n-1] == '\t')) n--;
            out[n] = 0; found = 1;
        }
        while (*req && *req != '\n') req++;
    }
    return found;
}
static int hd_decimal(const char **p, long *out)
{
    long v = 0; const char *s = *p;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        int digit = *s++ - '0';
        if (v > (0x7fffffffffffffffL - digit) / 10) return -1;
        v = v * 10 + digit;
    }
    *p = s; *out = v; return 0;
}
/* 0 full response; 1 partial; -1 valid but no selected bytes. */
static int hd_range(const char *value, long size, long *first, long *count)
{
    *first = 0; *count = size;
    const char *p = value, *unit = "bytes=";
    while (*unit) if (hd_lower(*p++) != *unit++) return 0;
    long a = 0, b = 0;
    if (*p == '-') {
        p++;
        if (hd_decimal(&p, &b) < 0 || *p) return 0;
        if (!b || !size) return -1;
        if (b > size) b = size;
        *first = size - b; *count = b; return 1;
    }
    if (hd_decimal(&p, &a) < 0 || *p++ != '-') return 0;
    int has_end = *p != 0;
    if (has_end && (hd_decimal(&p, &b) < 0 || *p || b < a)) return 0;
    if (a >= size) return -1;
    if (!has_end || b >= size) b = size - 1;
    *first = a; *count = b - a + 1; return 1;
}
#endif
