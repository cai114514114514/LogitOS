# 浏览器（c/apps/browser）缺陷审计 — 2026-09-16

范围：`c/apps/browser` 全部 .c/.h/.inc（约 10.6 万行，105,790 行按 `wc -l` 实测）。
方法：先读 `docs/CODE_AUDIT.md`、`docs/BUG_BACKLOG.md` 与 CLAUDE.md 的浏览器章节，
对已确认/已修复的旧条目逐一到 HEAD 核对（避免重复上报与上报已修项），然后按风险分级通读。
本报告全部结论为**代码级确认**，无 guest 测量——凡需要实测才能定级的，已在条目里注明。

## 方法与覆盖

**深读（逐行）**：`html_tokenizer.c`、`html_tree.c`（含 adoption agency、forster parenting、
foreign content、模板/表格全部插入模式）、`dom.c`/`dom.h`、`js_dom.c` 的基础设施
（wrap/wrapper 图/serial 句柄/失效记录/插入与脚本路径/事件分发/classList/CSSStyleDeclaration）、
`iframe_policy.c`、`cookie_persistence.c`、`cookie_store_guest.inc`、`http_cache.c`、
`js_frame.c`、`js_worker.c` 的任务与消息边界（task_add/cancel/postMessage/reap）、
`css_vars.c` 全文、`layout_flex.c` 的收敛循环（resolve_line/round_line/pack_offset）、
`js_canvas.c` 的路径构建与定点数学（fx/arg_fx/arc/arcTo/rect）、`passive_frame.c` 的
缓冲与 strcpy 守卫、`js_events.c` 头注与分发层、`js_page.c` 定时器部分、`js_anim.c`
过渡快照缓冲、`browser.c` 的 collect_scripts/load、`css_engine.c` 的 h_node_classes。

**中等（目标读法：找除零/死循环/截断/递归/绑定错误模式后核读命中处）**：`layout.c`
（除法/取模点、image_ratio_size、atoi_、flex_resolve 注释）、`layout_text.c`（UTF-8
边界回退循环）、`js_select.c`（JS 解析器 depth>8 拒绝、匹配器迭代化注释）、
`dom_serialize.c`（递归序列化器签名）、`css_engine.c` 其余部分（grep 级）。

**skim（grep 级危险模式扫描 + 结构统计，未逐行）**：`browser_paint.c`、`browser_rt.c`、
`css_extra.c`、`css_interp.c`、`css_report.c`、`css_import.c`、`js_platform.c`、
`js_webapi.c`、`js_cssom.c`、`js_semantics.c`、`js_anim.c`（非过渡部分）、`js_media.c`、
`js_media_src.c`、`forms.c`、`js_forms.c`、`js_module.c`、`js_wasm.c`、`js_idb.c`、
`js_domparser.c`、`js_url.c`（编码/解码/punycode 命中处）、`js_urlbind.c`、
`js_websocket.c`、`js_tokenlist.c`、`js_stall.c`、`js_devtools.c`、`js_subtle.c`、
`js_intl.c`、`js_swreg.c`、`js_ports.c`、`js_reflect.c`、`js_cache.c`、`js_download.c`、
`tabs.c`、`focus.c`、`storage_backend.c`、`page_runtime.c`，以及全部 `.inc`
（`dom_mutation.inc`/`dom_text_mutation.inc` 为列表级，其余为 grep 级）。

**明确未覆盖**：`c/net/http/`（http1/http2/hpool/cookies——按任务范围属浏览器目录之外，
且 `docs/NET_BUG_REVIEW_2026-09-09.md` 已有专门批次）；`third_party/quickjs`、
`third_party/css`（vendored，本仓库文档已声明其边界）。

## 发现

### [medium] [CONFIRMED] 网页缓存替换路径绕过 8 MiB 字节上限；OOM 时字节计数双重递减

位置：`c/apps/browser/http_cache.c:516-544`（`wacache_store`）

```c
struct wac_ent *e = find_ent_h(url, ckh);
if (e) { wac_bytes -= e->len; free(e->body); e->body = 0; e->content_type[0]=e->content_disposition[0]=0; }
else {
    while (wac_bytes + len > WAC_MAX_BYTES) {
        struct wac_ent *v = lru_victim();
        if (!v) break;
        drop_ent(v);
    }
    e = alloc_ent();
    ...
}
...
unsigned char *copy = (unsigned char *)malloc((size_t)len + 1);
if (!copy) { drop_ent(e); return -1; }
memcpy(copy, body, (size_t)len);
copy[len] = 0;
e->body = copy; e->len = len;
wac_bytes += len;
```

两个叠加缺陷，同一段代码：

1. **替换路径不驱逐**。命中同 (url, Cookie-hash) 键时直接减旧增新，不走
   `while (wac_bytes + len > WAC_MAX_BYTES)` 的驱逐循环。每个 body 自身被
   `len > WAC_MAX_BYTES` 拒绝（≤ 8 MiB），但 WAC_N=64 个不同键各自"原地换大"后，
   `wac_bytes` 合法地达到 64 × 8 MiB = 512 MiB——文件头自己写明 8 MiB 上限是
   "the real bound"，且注释（第 36-40 行）按"与浏览器 arena 峰值共存的预算"论证过这个数。
   触发不需要攻击者：一个页面反复 fetch 同一 URL、每次响应更大即可；一系列
   `fetch(u).then(r=>r.blob())` 换取逐次增大的响应是最短复现。
2. **malloc 失败路径双重递减**。替换路径第 517 行已 `wac_bytes -= e->len`，但
   `e->len` 未清零；随后 `malloc` 失败走 `drop_ent(e)`，其中
   `wac_bytes -= e->len` 用**旧长度**再减一次 → `wac_bytes` 下溢（负值/wrap）。
   此后 `while (wac_bytes + len > WAC_MAX_BYTES)` 在很久之内不再触发，与缺陷 1
   叠加后缓存对内存预算彻底失明。注释文化对照：`drop_ent` 是唯一的回收原语，
   替换路径却手工内联了它的一半。

后果：缓存内存无界（对"8 MiB 是硬预算"的承诺静默失效），页面只需正常 fetch 即可
把浏览器 arena 推向 OOM；OOM 时计数器损坏使驱逐彻底停摆。

修复建议：替换路径也走统一的"腾地方"入口——先 `drop_ent(e)`（或先把 e->len 归零、
wac_bytes 结清）再走与新建相同的 `while (wac_bytes + len > WAC_MAX_BYTES)` 驱逐；
`malloc` 失败分支复用同一个入口，保证 `wac_bytes` 只在一处结算。

### [medium] [CONFIRMED] DOM 写入路径没有树深防线，递归消费者可被脚本深度打穿栈

位置：
- 防线只在解析器侧：`c/apps/browser/dom.h:285`（`DOM_MAX_TREE_DEPTH 512`），
  挂在 `html_tree.c:611-617` 的 `insert_element()`；
- DOM API 插入无深度检查：`c/apps/browser/js_dom.c:1112-1120`（`can_insert`），
  只有环检查、跨文档检查、容器类型检查；
- 递归消费者：`js_dom.c:688-693`（`find_sel`，querySelector 路径）、
  `js_dom.c:1224-1256`（`offer_scripts`，插入脚本发现路径）、
  `layout.c:3550`（`layout_block`，dom.h 自己注明"最坏帧 ~1 KiB"）、
  `css_engine.c:3130`（`style_node`）、`dom_serialize.c` 的 `ser_node`。

```c
/* js_dom.c:1112 -- 插入门禁，注意没有深度项 */
static int can_insert(struct node *p, struct node *c)
{
    if (!p || !c || p == c) return 0;
    if (c->type == N_DOCUMENT || c->type == N_DOCTYPE) return 0;
    if (p->type != N_ELEM && p->type != N_DOCUMENT) return 0;
    if (p->doc != c->doc) return 0;
    if (is_ancestor(c, p)) return 0;                            /* would make a cycle */
    return 1;
}
```

dom.h 对 512 上限的论证（第 279-285 行）是"消费者在 8 MiB 栈上递归，所以树构建器
必须封顶"——但同一个论证对脚本经 `appendChild`/`insertBefore` 逐层套娃建出的树
同样成立，而这条路径没有对应防线：

```js
let d = document.body, n = 0;
for (;;) { let e = document.createElement("div"); d.appendChild(e); d = e;
           if (++n >= 20000) break; }        // 之后任何一次 restyle/布局/查询
```

解析器路径被封在 512；这条路径没有任何一处封顶。一旦该子树连入文档，
下一次 `layout_page()`/`style_node`（~1 KiB/帧 × 8 MiB 栈 ≈ 8 千层即到顶），
或页面自己调 `document.querySelector("div")`（`find_sel` 递归）、或插入任一
`<script>`（`offer_scripts` 递归），就是 ring-3 栈溢出——本进程内所有标签页共用
一个浏览器进程，不是单页静默失败。同文件对 `gather_text` 的注释（"Iterative: a
script can build a tree far deeper than a recursive gather would survive on the
browser's stack", js_dom.c:626-627）恰好证明作者知道这个威胁模型，但只有那一个
遍历被迭代化了。

后果：页面级脚本可使整个浏览器进程栈溢出（UB，非优雅错误）。严重度定 medium
而非 high：触发者是页面自身的脚本，跨页面攻击需要先有同源注入。

修复建议：与解析器同一裁决处收口——在 `insert_run()`（js_dom.c:1130）里对
`c` 的子树深度做一次迭代测深（或维护 O(1) 的节点 depth 字段），超过
`DOM_MAX_TREE_DEPTH` 时拒绝插入并 return JS_NULL（与本文件"一个坏调用只损失
这一次调用"的家规一致）；`find_sel` 改迭代（同文件已有 gather_text 的现成模板）。

### [low] [CONFIRMED] CSSStyleDeclaration.setProperty 的值参数转换失败会静默删除已有声明

位置：`c/apps/browser/js_dom.c:2896-2922`（`cssd_setProperty`）

```c
const char *nm = JS_ToCString(ctx, argv[0]);
const char *vl = argc > 1 ? JS_ToCString(ctx, argv[1]) : 0;
const char *pr = argc > 2 ? JS_ToCString(ctx, argv[2]) : 0;
...
if (nm && vl && cssd_refuses(nm, vl)) { /* dropped */ }
else if (nm) style_set(n, nm, vl, pr && (pr[0] == 'i' || pr[0] == 'I'));
```

`vl == NULL` 在本文件约定里是"删除该声明"（同文件 `cssd_removeProperty`，
js_dom.c:2932-2933，就是用 `style_set(n, nm, 0, 0)` 删除）。因此
`el.style.setProperty("color", someSymbol)` 这类**值参数不可转换**的调用：
`JS_ToCString(argv[1])` 失败（异常已挂起）→ `vl == NULL` → 命中
`else if (nm)` → **把 color 已有声明删掉**，然后带着挂起异常返回 JS_UNDEFINED。
真实浏览器抛 TypeError 且原地不动。一个本应"只报错、无副作用"的调用产生了
破坏性副作用，且异常被吞（返回值不是 JS_EXCEPTION），页面的 try/catch 收不到。
`pr` 同理失败时 importance 被静默丢弃，属同一形状。

修复建议：任一参数转换失败即立刻 `return JS_EXCEPTION`（保留已转换者的
FreeCString），不走写路径；至少把 `vl == NULL` 与"调用方本意就是删"的
removeProperty 分开判（argc 不足 2 才是删）。

### [low] [CONFIRMED] js_frame：`__frameGlobal` 先行采用会把 execute_scripts=0 固化，随后正式采用不再运行该文档的脚本

位置：`c/apps/browser/js_frame.c:360-365`（`frame_adopt` 早退）、`js_frame.c:407-424`
（`js__frameAdopt` / `js__frameGlobal` 两个入口共用同一张表）

```c
static struct jsframe *frame_adopt(JSContext *pctx,JSValueConst value,int execute_scripts)
{
    struct dom_doc *doc = js_domparser_doc_of(value);
    if (!doc) return NULL;
    struct jsframe *existing=find_by_doc(doc);
    if(existing)return existing->owner==pctx?existing:NULL;   /* execute_scripts 不更新 */
```

同一 frame 文档先经 `contentWindow`/`__frameGlobal(doc)` 路径被惰性采用
（`execute_scripts=0`，这是 js_platform.c 的 settle() 决定运行脚本的**之前**就能走到的门），
之后页面再调 `__frameAdopt(doc)` 请求"正式"采用并希望其中的内联 `<script>` 运行时，
命中 existing 早退，`execute_scripts` 永远停在 0——该文档里已有的脚本（srcdoc、
同源响应体里的）被静默跳过，无任何日志（被拒的分支都有 printf，这条没有）。
取决于 js_platform.c settle() 的调用顺序，Cloudflare 式 bootstrap（先读
contentDocument 再 adopt）可能正落在这条序上；是否实际可达需要 guest 验证，
代码路径本身是确定的。

修复建议：existing 命中时 `if (execute_scripts) existing->execute_scripts = 1;`
（升级方向单行安全；降级方向维持现状即可），或在此分支补一条与"refused"同风格的
printf，使静默路径至少可 grep。

### [low] [SUSPECTED] "in table" 的 `<form>` 未按参考实现 foster 出表

位置：`c/apps/browser/html_tree.c:2294-2300`（`in_table`，`case HTAG_FORM`）

```c
case HTAG_FORM: {
    if (stack_has_html_tag(tb, HTAG_TEMPLATE) || tb->form_elem) return;
    struct node *f = insert_html_element(tb, t);
    tb->form_elem = f;
    stack_pop(tb);
    return;
}
```

本实现的 foster-parenting 是一个显式 `tb->foster` 开关，只在"anything else →
in_body"的转发处（html_tree.c:2327-2332、flush_table_text）打开；此处是普通
`insert_html_element`，foster 关着，`<form>` 落进 `<table>` 内部。参考实现
（html5lib 及各浏览器）在 in-table 模式保持 insertFromTable 开启，`<form>`
按"appropriate place"算法被 foster 到表**之前**成为 `<table>` 的兄弟。
这不是内存安全问题，是树形状偏差——后果是 `<table><form>...` 型老页面的
表单控件被布局进表格内部。标 SUSPECTED：本机无法跑 html5lib 语料核对该 case
是否已在基线失败清单里（94.8% ratchet 的既有失败可能已含它）。

修复建议：该分支仿照第 25 步 AAA 的写法，在插入前后临时置
`tb->foster = 1`（`struct ins_place p = ...` 同款保存/恢复），再用
html5lib 的 webkit02/tests 外加一例 `<table><form>` 快照钉住。

## 已知问题（未重复上报）

以下旧条目均已到 HEAD 核对为**已修**，不再列入发现：

- `browser.c` collect_scripts 不写 NUL（CODE_AUDIT 中危）——现每条内联脚本独立
  malloc 精确长度并 `e->data[o] = 0`（browser.c:1761-1770 区域）。
- `css_vars.c` var_subst fallback 递归无上限——现 `VAR_MAX_DEPTH 32`
  （css_vars.c:335），且值不整存则标 unusable 的设计已封死失衡拼接类。
- `css_engine.c` h_node_classes 每调用泄漏（M11）——现返回节点自身 token 数组并
  逐项 ref（css_engine.c:115 起）。
- `dom.c` newnode() 返回值未检查——现 node_alloc 全路径检查（elem_new、
  dom_create_text、dom_create_comment 等）。
- `layout.c` atoi_ 溢出与 `ih*s2/iw` 乘法溢出——atoi_ 现有 `n>100000` 钳制
  （layout.c:1212）；ratio 数学走 long long 且调用方保证分母 > 0
  （layout.c:988-991、1056、1060）。
- `layout.c` layout_free 不重置 page_has_bg——现 layout_page 入口清零
  （layout.c:5874）。
- js_dom 静态 class id 跨 runtime（H-20）与 wrapper UAF（H-21）——前者由
  js_dom_init 去 guard + "frame 不建第二 DOM"的架构裁决关闭（js_frame.c:37-54
  的长注释）；后者由 per-node serial 句柄取代全局 epoch（js_dom.c:307-315），
  `removeChild`/`replaceChild` 亦已改 detach 语义（js_dom.c:1282-1338）。
- CLAUDE.md 记录的 `Node.isEqualNode` 缺失 → React 水合失败——已实现并有教训记录。
- http1/hpool/TLS 等 c/net 侧条目——见 `docs/NET_BUG_REVIEW_2026-09-09.md`，
  不在本报告范围。

## 检查过且认为 sound 的点（防下一位重查）

- tokenizer 实体表二分（`html_ref_lookup`）与数字引用钳制（`num_fixup` 对
  0x0/surrogate/>0x10FFFF 全部归 FFFD）；流式 rule 3 的整token重放依赖当前唯一
  调用方（html_parse 全量缓冲、eof=1），`html_tok_feed` 树内无第二个用户。
- adoption agency 的 furthestBlock 取"fe 之上第一个 special"，与生成期望树的
  html5lib 参考实现一致（非 spec 文字"topmost"的字面歧义）；negative control
  （HTML_AAA_NAIVE）在树内且被 gate 观测。
- `iframe_policy.c:119` 的 `char scheme[8]; strcpy(...)`：入口 `ip_url` 已限定
  scheme 恰为 "http:"/"https:"，第 120 行 `n>5` 拒绝后最大写索引为 6——按构造安全。
- `passive_frame.c` 全部 strcpy 均有调用点 `strlen < sizeof` 守卫（parent_url/
  parent_csp/f->src/image/final 逐一核对）。
- js_worker 的 `task_add` 头插与 postMessage 端口路径 `queued = g_tasks` 的
  假设一致；worker 私有 watchdog 与页面 watchdog 按 worker 上下文隔离（260-262 注释）。
- `http_cache.c` 的 FNV cookie-key：碰撞论证（2^-53/启动）与注释一致；非加密哈希
  的威胁模型论证成立（cookie 行来自浏览器自有 jar）。
- cookie_persistence 的双槽 CRC/世代/readback、时钟回拨 rebase 对 400 天上限的
  交互（GLOBAL_CEILING 对照分支）自洽。
- js_canvas 的定点/非有限值处理（`isfinite` 而非 NaN 检查、`fx` 饱和）、
  `gfx_path_matrix` 中途调用禁忌的绕行（路径持设备坐标、CTM 逐点变换）符合
  CLAUDE.md 记录的两条引擎契约；arc 角度差在饱和 24.8 域内无 int 溢出。
