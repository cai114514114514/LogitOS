# M17 L1 — 渲染管线下放到 ring-3 browser app 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: 用 superpowers:executing-plans 或
> subagent-driven-development 逐任务执行。步骤用 `- [ ]` checkbox 跟踪。

**Goal:** 把 HTML→DOM→CSS→layout→paint 整条管线从内核搬进 ring-3 的
`browser.aex`,内核退化为原语提供者(fetch / 字体度量 / 绘制),`example.com`
在 ring-3 管线渲染并与旧的内核渲染一致,内核 `SYS_PAGE_*` 退役。

**Architecture:** 现有 `net/dom.c` + `net/css.c` + `net/layout.c` 直接编进 app
(逻辑不动,只把 `kmalloc/kfree`→`malloc/free`、`text_measure/res_fetch`→syscall);
paint 在 app 端用绘制原语重写。新增 6 个 syscall 作为 IPC 原语;`SYS_HTTP_GET`
改为只 fetch,body 经 `SYS_HTTP_BODY` 拷给 app。DOM 在 app 内,`<style>`/`<script>`
的收集和 hit-test 也随之移进 app。

**Tech Stack:** C freestanding (`clang --target=x86_64-elf -msse2`),mini-libc
(24MiB arena malloc),QEMU 集成验证 + host 回归单测(`tools/t/*`)。

---

## 设计:新增 / 改动 / 退役的 syscall

ABI 现有最大号 = 35 (`SYS_PAGE_SCRIPTS`)。新增 36–41:

```c
#define SYS_HTTP_BODY    36  /* (buf, max) -> 拷上次 http_get 的响应体, 返回长度 */
#define SYS_TEXT_MEASURE 37  /* (s, len, (px<<1)|mono) -> 文本像素宽度 */
#define SYS_GUI_TEXT_RUN 38  /* (struct aether_run*) 画一段定长文本(px/mono/color) */
#define SYS_RES_FETCH    39  /* (src, buf, max) -> 取子资源原始字节, 返回长度或 <0 */
#define SYS_GUI_BLIT     40  /* (struct aether_blit*) 把 RGBA 位图贴进窗口 surface */
#define SYS_GUI_CLIP     41  /* ((x<<16)|y, (w<<16)|h) 设窗口裁剪; (0,0,0,0)=清除 */
```

结构体(放 `aether_abi.h`,内核 + app 共用):

```c
struct aether_run  { int x, y, px, mono; unsigned color; const char *s; int len; };
struct aether_blit { int x, y, w, h; const unsigned char *rgba; int sw, sh; };
```

- **改语义**:`SYS_HTTP_GET (26)` 只做 DNS+TCP+TLS+HTTP(+重定向),不再建 DOM/布局。
- **退役并删除**:`SYS_PAGE_RENDER(31)` `SYS_PAGE_HEIGHT(32)` `SYS_PAGE_HITTEST(33)`
  `SYS_PAGE_LOAD_IMAGES(34)` `SYS_PAGE_SCRIPTS(35)`(逻辑全部移进 app)。
- **保留**:`SYS_HTTP_STATUS(27)`。

---

## Task 1: ABI — syscall 号 + 结构体 + 用户态封装

**Files:**
- Modify: `include/aether_abi.h`(加 36–41 与两个 struct;删 31–35 注释行)
- Modify: `user/aether.h`(加封装,删 `page_*` 封装)

- [ ] **Step 1:** `include/aether_abi.h`:把 `SYS_PAGE_RENDER`..`SYS_PAGE_SCRIPTS`
  这 5 行替换为上面 36–41 的 6 个定义;在文件结尾(`#endif` 前)加两个 struct。
- [ ] **Step 2:** `user/aether.h`:删 `page_render/page_height/page_hittest/page_load_images/page_scripts`
  封装;加:

```c
static inline int http_body(char *buf, int max) { return (int)_sys(SYS_HTTP_BODY,(long)buf,max,0); }
static inline int text_measure_px(const char *s,int len,int px,int mono)
{ return (int)_sys(SYS_TEXT_MEASURE,(long)s,len,((long)px<<1)|(mono&1)); }
static inline void gui_text_run(int x,int y,int px,int mono,unsigned color,const char *s,int len)
{ struct aether_run r={x,y,px,mono,color,s,len}; _sys(SYS_GUI_TEXT_RUN,(long)&r,0,0); }
static inline int res_fetch_raw(const char *src,unsigned char *buf,int max)
{ return (int)_sys(SYS_RES_FETCH,(long)src,(long)buf,max); }
static inline void gui_blit(int x,int y,int w,int h,const unsigned char *rgba,int sw,int sh)
{ struct aether_blit b={x,y,w,h,rgba,sw,sh}; _sys(SYS_GUI_BLIT,(long)&b,0,0); }
static inline void gui_clip(int x,int y,int w,int h)
{ _sys(SYS_GUI_CLIP,((long)(x&0xFFFF)<<16)|(y&0xFFFF),((long)(w&0xFFFF)<<16)|(h&0xFFFF),0); }
```

- [ ] **Step 3:** 编译冒烟:`make build/clock.aex`(任意 app 引用 aether.h 即可验证头不报错)
  —— 期望成功。Commit:`git commit -am "m17 L1: ABI for ring-3 render pipeline (syscalls 36-41)"`

## Task 2: 内核 — 新 syscall 实现 + SYS_HTTP_GET 改为只 fetch + 退役 SYS_PAGE_*

**Files:**
- Modify: `kernel/wm.c`

- [ ] **Step 1:** 删与页面相关的内核态:`page_root` / `page_built` / `page_reset()` /
  `collect_style()` / `collect_scripts()`(42–82 行附近),以及 `#include "dom.h"`
  `"css.h"` `"layout.h"` `"paint.h"` `"img.h"`(保留 `http.h` `text.h` `fb.h`)。
- [ ] **Step 2:** `SYS_HTTP_GET` case 改成只 fetch:

```c
case SYS_HTTP_GET: {
    char url[USER_URL_MAX];
    if (user_copy_string(url, sizeof url, (const char *)a) < 0) return -1;
    __asm__ volatile ("sti");
    int grc = http_get(url);
    __asm__ volatile ("cli");
    return grc;
}
```

- [ ] **Step 3:** 删 `SYS_PAGE_RENDER/HEIGHT/HITTEST/LOAD_IMAGES/SCRIPTS` 五个 case。
- [ ] **Step 4:** 在 `SYS_HTTP_STATUS` 之后加新 case:

```c
case SYS_HTTP_BODY: {
    char *buf = (char *)a; int max = (int)b;
    if (max <= 0 || !user_range_ok(buf, (uint64_t)max, 1)) return -1;
    int blen; const char *body = http_body(&blen);
    if (!body || blen <= 0) return 0;
    int n = blen < max ? blen : max;
    memcpy(buf, body, (size_t)n);
    return n;
}
case SYS_TEXT_MEASURE: {
    const char *s = (const char *)a; int len = (int)b;
    int px = (int)((c >> 1) & 0x7FFFFFFF), mono = (int)(c & 1);
    if (len < 0 || len > USER_TEXT_MAX) return 0;
    char tmp[USER_TEXT_MAX];
    if (len > 0) { if (!user_range_ok(s, (uint64_t)len, 0)) return -1; memcpy(tmp, s, (size_t)len); }
    return text_measure(tmp, len, px, mono);
}
case SYS_GUI_TEXT_RUN: {
    struct win *w = app_window(ap); if (!w) return -1;
    struct aether_run r;
    if (!user_range_ok((void *)a, sizeof r, 0)) return -1;
    memcpy(&r, (void *)a, sizeof r);
    int len = r.len; if (len < 0 || len > USER_TEXT_MAX - 1) len = USER_TEXT_MAX - 1;
    char tmp[USER_TEXT_MAX];
    if (len > 0) { if (!user_range_ok(r.s, (uint64_t)len, 0)) return -1; memcpy(tmp, r.s, (size_t)len); }
    tmp[len] = 0;
    fb_target(&w->surf);
    text_draw_run(r.x, r.y, tmp, len, r.px, r.mono, r.color);
    fb_target(NULL);
    return 0;
}
case SYS_RES_FETCH: {
    char src[USER_URL_MAX];
    if (user_copy_string(src, sizeof src, (const char *)a) < 0) return -1;
    char *buf = (char *)b; int max = (int)c;
    if (max <= 0 || !user_range_ok(buf, (uint64_t)max, 1)) return -1;
    __asm__ volatile ("sti");
    uint8_t *rb; int rl; int rc = res_fetch(src, &rb, &rl);
    __asm__ volatile ("cli");
    if (rc != 0) return -1;
    int n = rl < max ? rl : max;
    memcpy(buf, rb, (size_t)n);
    kfree(rb);
    return n;
}
case SYS_GUI_BLIT: {
    struct win *w = app_window(ap); if (!w) return -1;
    struct aether_blit bl;
    if (!user_range_ok((void *)a, sizeof bl, 0)) return -1;
    memcpy(&bl, (void *)a, sizeof bl);
    if (bl.sw <= 0 || bl.sh <= 0 || bl.sw > 4096 || bl.sh > 4096) return -1;
    if (!user_range_ok(bl.rgba, (uint64_t)bl.sw * bl.sh * 4, 0)) return -1;
    fb_target(&w->surf);
    fb_blit_rgba(bl.x, bl.y, bl.w, bl.h, bl.rgba, bl.sw, bl.sh);
    fb_target(NULL);
    return 0;
}
case SYS_GUI_CLIP: {
    struct win *w = app_window(ap); if (!w) return -1;
    int x = (int)((a >> 16) & 0xFFFF), y = (int)(a & 0xFFFF);
    int cw2 = (int)((b >> 16) & 0xFFFF), ch2 = (int)(b & 0xFFFF);
    if (cw2 == 0 && ch2 == 0) fb_clear_clip();
    else fb_set_clip(x, y, cw2, ch2);
    return 0;
}
```

- [ ] **Step 5:** `kfree` 原型:`SYS_RES_FETCH` 用到 `kfree`;wm.c 已有 `kheap.h`(含
  `kmalloc/kfree`)。确认无误。
- [ ] **Step 6:** `make`(只编内核)——期望成功;`grep -n "page_root\|paint_viewport\|SYS_PAGE" kernel/wm.c`
  应无输出。**注意**:此时 app 还在调旧 `page_*`,disk 暂不可用,先不跑 QEMU。
  Commit:`git commit -am "m17 L1: kernel render primitives; SYS_HTTP_GET fetch-only; retire SYS_PAGE_*"`

## Task 3: app 端运行时 shim(kmalloc/text_measure/res_fetch 等)

**Files:**
- Create: `user/browser_rt.c`

- [ ] **Step 1:** 写 shim,把管线对内核符号的依赖映射到 app:

```c
#include "aether.h"
#include <stddef.h>
#include "img.h"
void *malloc(size_t); void free(void *);
void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }
int text_measure(const char *s, int len, int px, int mono) { return text_measure_px(s, len, px, mono); }
int res_fetch(const char *src, unsigned char **buf, int *len) {
    static unsigned char tmp[262144];           /* 256K: 单张图上限 */
    int n = res_fetch_raw(src, tmp, sizeof tmp);
    if (n <= 0) return -1;
    unsigned char *b = malloc((size_t)n); if (!b) return -1;
    for (int i = 0; i < n; i++) b[i] = tmp[i];
    *buf = b; *len = n; return 0;
}
/* 图片解码:Task 7 接真 lib/img;先 stub 让链接通过(example.com 无图) */
int  img_decode(const unsigned char *p, int n, struct image *o) { (void)p;(void)n;(void)o; return -1; }
void img_free(struct image *o) { (void)o; }
```

- [ ] **Step 2:** 暂不单独编译(由 Task 6 Makefile 接入)。本任务只落文件。

## Task 4: app 端 paint + hit-test 重写

**Files:**
- Create: `user/browser_paint.c`
- Create: `user/browser_paint.h`

- [ ] **Step 1:** `user/browser_paint.h`:

```c
#ifndef BROWSER_PAINT_H
#define BROWSER_PAINT_H
void browser_paint(int vx, int vy, int vw, int vh, int scroll);
int  browser_hittest(int x, int y, int scroll, char *buf, int max);
#endif
```

- [ ] **Step 2:** `user/browser_paint.c`:走 `layout_items()`/`layout_count()`,
  用绘制原语画(对照内核 `net/paint.c` 的逻辑,把 `fb_*`→`gui_*`):

```c
#include "aether.h"
#include "layout.h"
#include "browser_paint.h"

void browser_paint(int vx, int vy, int vw, int vh, int scroll)
{
    const struct item *it = layout_items();
    int n = layout_count();
    gui_clip(vx, vy, vw, vh);
    for (int i = 0; i < n; i++) {
        const struct item *e = &it[i];
        int top = e->y - scroll;
        if (top + e->h < 0 || top > vh) continue;
        int sx = vx + e->x, sy = vy + top;
        if (e->type == IT_RECT) {
            if (e->has_bg) gui_rect(sx, sy, e->w, e->h, e->bg);
            if (e->border_w > 0) {
                int bw = e->border_w;
                gui_rect(sx, sy, e->w, bw, e->border_color);
                gui_rect(sx, sy + e->h - bw, e->w, bw, e->border_color);
                gui_rect(sx, sy, bw, e->h, e->border_color);
                gui_rect(sx + e->w - bw, sy, bw, e->h, e->border_color);
            }
        } else if (e->type == IT_TEXT) {
            gui_text_run(sx, sy, e->font_px, e->mono, e->color, e->text, e->len);
            if (e->underline) {
                int uy = sy + e->font_px + (e->font_px > 20 ? 3 : 2);
                gui_rect(sx, uy, e->w, 1, e->color);
            }
        } else if (e->type == IT_IMAGE && e->img) {
            gui_blit(sx, sy, e->w, e->h, e->img->rgba, e->img->w, e->img->h);
        }
    }
    gui_clip(0, 0, 0, 0);
}

int browser_hittest(int x, int y, int scroll, char *buf, int max)
{
    if (max <= 0) return 0;
    const struct item *it = layout_items();
    int n = layout_count();
    int dy = y + scroll;
    for (int i = n - 1; i >= 0; i--) {
        const struct item *e = &it[i];
        if (!e->href) continue;
        if (x >= e->x && x < e->x + e->w && dy >= e->y && dy < e->y + e->h) {
            int o = 0; while (e->href[o] && o < max - 1) { buf[o] = e->href[o]; o++; }
            buf[o] = 0; return 1;
        }
    }
    return 0;
}
```

## Task 5: 改写 browser.c 使用 ring-3 管线

**Files:**
- Modify: `user/browser.c`

- [ ] **Step 1:** 顶部 include 改为:`#include "aether.h"` + `"dom.h"` `"css.h"`
  `"layout.h"` `"browser_paint.h"`。删 http_get 错误码本地宏(用 aether.h/ABI)。
- [ ] **Step 2:** 把 `collect_style` / `collect_scripts`(从旧 wm.c 搬来)作为 static
  函数加入 browser.c —— 遍历本地 DOM,签名 `int collect_style(struct node*,char*,int,int)`
  / `int collect_scripts(...)`(代码同旧 wm.c 版本)。
- [ ] **Step 3:** 加全局 DOM:`static struct node *g_root;` 启动时 `css_init();`。
- [ ] **Step 4:** 重写 `load(u)`:

```c
static char bodybuf[65536];
static char author_css[16384];

static void load(const char *u)
{
    set_status("loading...");
    if (g_root) { dom_free(g_root); g_root = 0; }
    layout_free();
    int rc = http_get(u);
    if (rc < 0 || http_status() != 2) { /* 同旧错误分支 */ ph = 0; scroll = 0; set_status("load failed"); return; }
    int blen = http_body(bodybuf, sizeof bodybuf);
    g_root = dom_parse(bodybuf, blen);
    if (!g_root) { set_status("parse failed"); ph = 0; scroll = 0; return; }
    int css_len = collect_style(g_root, author_css, 0, (int)sizeof author_css);
    css_apply(g_root, author_css, css_len);
    layout_page(g_root, WINW);
    scroll = 0; ph = layout_height();
    set_status("loaded");
    redraw(0);
    if (layout_load_images(3) > 0) { ph = layout_height(); redraw(0); }
    /* inline <script> */
    jslen = 0; jsout[0] = 0;
    static char scr[16384];
    int sn = collect_scripts(g_root, scr, 0, sizeof scr);
    if (sn > 1) { run_js(scr); /* 同旧:把 jsout 写进 status */ redraw(0); }
}
```

- [ ] **Step 5:** `redraw()` 里 `page_render(...)` → `browser_paint(0,BARH,WINW,VIEW_H,scroll)`。
- [ ] **Step 6:** 鼠标命中:`page_hittest(...)` → `browser_hittest(mx, my-BARH, scroll, nu)`。
- [ ] **Step 7:** 删除对 `page_height()` 的引用(用 `layout_height()` / 本地 `ph`)。

## Task 6: Makefile — 把管线编进 browser.aex,从内核排除

**Files:**
- Modify: `Makefile`

- [ ] **Step 1:** 内核源排除 4 个已下放文件:

```make
C_SRC := $(filter-out net/dom.c net/css.c net/layout.c net/paint.c,\
         $(wildcard kernel/*.c drivers/*.c lib/*.c fs/*.c net/*.c crypto/*.c))
```

- [ ] **Step 2:** 在 browser 规则附近加 app 端管线对象:

```make
BROWSER_PIPE := net/dom.c net/css.c net/layout.c user/browser_rt.c user/browser_paint.c
BROWSER_OBJ  := $(patsubst %.c,$(BUILD)/browserobj/%.o,$(BROWSER_PIPE))
$(BUILD)/browserobj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -Iuser -c $< -o $@
```

- [ ] **Step 3:** `browser.elf` 链接加 `$(BROWSER_OBJ)`:

```make
$(BUILD)/browser.elf: $(ENGINE_OBJ) $(BUILD)/jsobj/user/browser.o $(BROWSER_OBJ) $(BUILD)/js.crt0.o
	$(LD) -nostdlib -e _start -Ttext=0x45000000 -o $@ $(BUILD)/js.crt0.o $(ENGINE_OBJ) $(BUILD)/jsobj/user/browser.o $(BROWSER_OBJ)
```

- [ ] **Step 4:** `browser.o`(jsobj 规则)的依赖加 dom/css/layout/browser_paint 头;
  jsobj 规则已用 `-Iinclude`,但 browser.c 现 `#include "dom.h"` 经 `-Iinclude` 可达,
  `"browser_paint.h"` 在 `user/`(quote-include 相对 user/browser.c 可达)。无需改 JS_INC。
- [ ] **Step 5:** `make && make build/disk.img` —— 期望内核 + browser.aex 都编过。

## Task 7: 图片(SYS_RES_FETCH 闭环 + 真解码)

**Files:**
- Modify: `user/browser_rt.c`(删 img stub)
- Modify: `Makefile`(BROWSER_PIPE 追加 `lib/inflate.c lib/png.c lib/gif.c lib/img.c`)
- Modify: `user/browser.c`(`app_main` 起始调 `img_init();`)

- [ ] **Step 1:** `browser_rt.c` 删 `img_decode`/`img_free` 两个 stub。
- [ ] **Step 2:** Makefile `BROWSER_PIPE += lib/inflate.c lib/png.c lib/gif.c lib/img.c`。
- [ ] **Step 3:** browser.c `app_main` 开头加 `img_init();`(注册 PNG/GIF 解码器),声明
  `void img_init(void);`(或 include img.h)。
- [ ] **Step 4:** `make build/disk.img` 成功。

## Task 8: 验证

- [ ] **Step 1: host 回归**(确认下放的逻辑没改坏):
```sh
clang -Iinclude tools/t/dom_test.c    net/dom.c                          -o /tmp/d && /tmp/d
clang -Iinclude tools/t/css_test.c    net/css.c net/dom.c                -o /tmp/c && /tmp/c
clang -Iinclude tools/t/layout_test.c net/layout.c net/css.c net/dom.c   -o /tmp/l && /tmp/l
clang -Iinclude tools/t/page_test.c   net/layout.c net/css.c net/dom.c   -o /tmp/p && /tmp/p
```
期望全 `ok:`,无 `FAIL`。
- [ ] **Step 2: 构建** `make && make build/disk.img` 全绿。
- [ ] **Step 3: QEMU** `make run`,Browser 打开 `http://example.com/`:标题/正文/链接
  渲染,与内核版一致;链接可点;滚动正常;文字不溢出地址栏/状态栏(验证 clip)。
  或 `python3 tools/qmp_browser.py` 自动截图比对。
- [ ] **Step 4:** `https://en.wikipedia.org` 冒烟:文本 + 图片(走 Task 7 路径)显示。
- [ ] **Step 5:** Commit:`git commit -am "m17 L1: render pipeline runs in ring-3 browser; example.com verified"`

## Self-review 检查

- **Spec 覆盖**:SYS_HTTP_BODY ✅(T1/T2) · SYS_TEXT_MEASURE ✅(T1/T2) ·
  app 链接 dom/css/layout ✅(T6) · paint 走 gui_* ✅(T4) · example.com 一致 ✅(T8) ·
  SYS_PAGE_* 退役 ✅(T2)。gui_blit/图片 ✅(T7,spec L1 提到 gui_blit)。
- **类型一致**:`text_measure_px`/`gui_text_run`/`res_fetch_raw` 在 T1 定义,T3/T4 使用一致;
  `browser_paint`/`browser_hittest` 在 T4 定义,T5 使用一致。
- **风险**:每词一次 `SYS_TEXT_MEASURE`(int 0x80 往返)对大页偏慢——L1 可接受,
  spec 已记后续可批量度量或把 TTF 下放 ring-3。

## L2–L4 路线(L1 落地后各自出详细 plan)

- **L2 集成 LibCSS**:`third_party/css`(libwapcaplet+libparserutils+libcss)编进
  browser(含 perl/python 代码生成的 Makefile 规则);写 `css_select_handler` 适配本地
  DOM;`layout.c` 改读 `css_computed_style`;删 `net/css.c`。
- **L3 加深 HTML**:`dom.c` → 更完整 HTML5(隐式 tbody/head/body、错误恢复、更多具名
  实体、class/id/属性选择器所需的属性解析),喂饱 LibCSS 选择器。
- **L4 JS↔DOM 绑定**:QuickJS 内建 document/element,`getElementById`/`querySelector`/
  `textContent`/`innerHTML`/`style`,改 DOM 后触发重排重绘。
