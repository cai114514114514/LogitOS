# 独立文档运行时：已实现的基础与验收边界

本轮继续补 Google 人机验证所缺少的通用浏览器能力，落地的是**独立 DOM 与核心 JS 运行时/事件队列**。没有完成联网交互 iframe，没有获得 Google 验证通过或真实搜索结果。本轮没有重试 Google 搜索、修改客户端身份、处理验证令牌或代答验证码。

续进说明：下面保留前一轮证据；本次新增的独立网络和平台状态、回归结果见文末“第二阶段”。旧的“子上下文网络/平台均缺席”只描述默认核心模式，不再描述显式启用后的子文档。

## 代码与所有权

- `c/apps/browser/js_dom.c/.h`：每个文档独立保存 32 个字段，包含 DOM 根、包装器、原型/atom、监听器、脏区域、滚动位置、激活与回调。类 ID 改为进程内稳定编号，但仍在每个 QuickJS runtime 注册；垃圾回收不再依赖当前页面的类编号。原生 DOM 通知临时进入所属文档，包装器释放计数归属原文档。
- `c/apps/browser/js_page.c/.h`：每个核心文档独立保存 40 个字段，拥有自己的 QuickJS runtime/context、console、watchdog、导航世代和任务队列。继续使用现有 `page_runtime` 与 `js_page_run_due`，没有新增第二套定时器调度器。队列地址稳定，切换的是指针，不能复制队列本体让任务 token 指向同一全局地址。
- JS 正在执行时拒绝切换/销毁。重入关闭只请求取消，待调用栈退出后再销毁。调度器额外检查队列 context 与当前 context 一致，拒绝用错误 runtime 调用回调。
- `tests/check_dom_context_fields.py` 用 Clang AST 检查字段列表与实际声明（含 DOM interface include），并自测删除所有权条目必然失败。新增静态状态不能悄悄脱离隔离。

`js_page_context_create/activate/destroy` 是 native embedder API，不是网页中的 iframe 或 WindowProxy。NULL 表示原有顶层上下文。新建上下文目前仅安装 DOM、timers、rAF、microtasks、console 和 watchdog；网络、存储、媒体等可选模块仍是单例，子上下文不安装、不轮询、不关闭它们。顶层保留原有全部安装器。

每个 live context 必须使用独立 JSRuntime 与 DOM arena。不能跨 runtime 传 JSValue，不能把父 document 暴露成子 document。该 API 不切换 CSS/layout，调用方还必须协调已有布局上下文；更没有自动挂接到 `passive_frame.c` / `js_frame.c`。

## 可复现检查

```sh
make BUILD=build-google-home/work test-dom-context-asan test-page-context-asan
make BUILD=build-google-home/work test-page-timers test-dom-iface test-dom-wrapper-lifetime
make BUILD=build-google-home/work test-parser-script-order test-parser-script-events
make BUILD=build-google-home/work test-frame-bootstrap-wiring test-legacy-home test-mk-wired
make BUILD=build-google-home/work test-browser-close-load-sanitize
```

- DOM contexts：86/86；父子同 ID 节点、原型与监听器独立，滚动状态恢复，非当前子 runtime GC，原生节点销毁，父定时器/点击在子销毁后仍工作。
- Page contexts：34/34；完整平台父页面和两个核心子页面共存，定时器/rAF/微任务独立，导航后旧任务不执行，激活不继承，watchdog 与取消隔离，销毁非当前兄弟上下文仍恢复正确页面。
- 两组 ASan/UBSan 运行无诊断。该表述不等同于全浏览器无泄漏或所有并发场景已验证。
- 忽略 viewport 保存的负对照：86 项中 8 项正确失败。误用共享队列的负对照：34 项中 3 项正确失败，均关联父 timer 丢失；错 runtime 执行由新增一致性检查拒绝。
- 旧检查：DOM interface 58、wrapper lifetime 25、page timers 32、legacy home 95 通过；脚本顺序、脚本事件、iframe bootstrap 通过。加载途中关闭的 7 个场景及 sanitizer 运行通过，各自负对照为正测试前置条件。
- `test-mk-wired`：365 fragments，364 reachable，1 declared exception。这是当前共享工作树观察值，不是本轮新增 3 个片段；本轮复用既有 mk 文件。

日志在 `build-active-frame/context-asan.log`、`regressions-final.log`、`close-regression.log`。本轮未提交 Git，保留其它并行修改。

## 客机证据：仅为顶层回归

新浏览器与独立系统盘位于 `build-active-frame/work/`，使用既有 ISO，不宣称本轮全内核构建。mkfs 保留 5 个用户状态 inode。

本地 fixture 是 `tests/fixtures/page-context/index.html`，使用普通 HTML/JS。运行时可另开终端：

```sh
python3 -m http.server 9876 --bind 127.0.0.1 --directory tests/fixtures/page-context
SITE_LOAD=90 SITE_PAINT=30 python3 tests/qmp/qmp_site.py \
  --iso build-active-frame/work/logit.iso --disk build-active-frame/work/disk.img \
  --name document-runtime-final --url 'http://10.0.2.2:9876/index.html' \
  --out build-active-frame/guest-final.json --boxes \
  --input-id entry --input-text runtime-ok --keep
```

客机报告 PAINTED，页面普通定时器及 Promise 更新可见文字，串口记录 `RUNTIME-GUEST-PASS` 和 `RUNTIME-INPUT:runtime-ok`，没有 JS/定时器/module 异常。截图 `build-active-frame/document-runtime-final.input.png`。这是新构建的**顶层 DOM/定时器/输入**回归，不是独立子上下文已接入客机 iframe 的证据，更不是搜索验收。

本轮测试启动的本地 HTTP 服务在完成后停止，复现时需重新启动。

产物 SHA-256：

```text
b962dceec157f4960bb0b68114f019cf59f0b23771532aaf32eb338bb9fd13f4  browser.aex
263e136f0f59a3e4f3b180e1c625cbb77ff9bef3c5c5bc4986bd940c3204235d  disk.img
0f80979f37e8a17e590b45e7579f26231ae137607f99047c2f9926784190ec73  logit.iso
```

## 距离 Google 验收仍缺少什么

可选 JS 平台/网络模块需要真实的每文档状态与异步所有权（其中有静态数组地址逃逸，不能直接照抄 DOM 的 snapshot）。之后接外部脚本、资源/CSP/sandbox 策略、WindowProxy/postMessage 的来源与生命周期检查，以及 iframe 绘制、命中测试、鼠标键盘/焦点。当前 `passive_frame` 仍禁用脚本与子输入；原有同源轻量 `js_frame` 也没有因此自动变成完整文档。

这些能力接好后，才有条件再次测试 Google 的实际验证页面；验证码必须由用户手动操作，服务器返回真实搜索结果才算完成。

## 第二阶段：独立 WebAPI 与平台状态

本次实现的是实际文档运行时的网络与平台消费者，不是网页 iframe 的接入完成：

- `js_page_context_enable_webapi(context, site_url)`：为尚未打开、非当前的核心上下文启用独立 WebAPI。`site_url` 必须由 embedder 依据整个祖先链计算；NULL/空值表示不透明 site-for-cookies。跨站祖先不能简单替换成顶层 URL，否则子页面请求祖先站点时可能错误携带 Strict Cookie。
- `js_page_context_enable_platform(context)`：在 WebAPI 之后启用文档自己的平台闭包、解析生命周期、MutationObserver、Promise 错误通知。旧单例 iframe helper 不在新平台 slot 安装或关闭。
- `js_webapi.c` 分离 27 个文档字段，保留共享且地址稳定的 fetch 请求池、heap realm registry、Cookie jar、origin/session 分区存储、预检缓存和唯一编号。关闭子页面只取消它自己的请求。
- `js_platform.c` 分离 20 个字段。原生 MutationObserver subscription 由 owner 持有稳定地址；非当前文档的原生修改只向所属运行时排队，恢复原上下文之后才返回，不在通知钩子里执行 JS。熵源仍为共享系统服务。
- `js_page.c` 现在分离 42 个字段，分别控制 core、WebAPI、platform 的安装/调度/关闭，不启用仍为单例的其它模块。Storage class ID 保持进程内稳定，在各 runtime 单独注册。
- history 改写相对 fetch 的 base，但不改变文档 origin 或祖先 Cookie 策略；不透明文档不能用 pushState 提升为网络 origin。Cookie 读取、写入和请求继续使用既有策略引擎。

这里仍不切换 CSS/layout。Shipping `matchMedia` 的权威来自选中的 CSS context，`screen.width/height` 来自设备屏幕，不是 WebAPI 的 viewport 字段；不能用这些字段的存储隔离声称 iframe 渲染已经隔离。外部脚本加载、资源/CSP/sandbox 策略、WindowProxy/postMessage、绘制及输入路由仍待接入。其它可选模块、嵌套活动 frame 也未自动启用。

### 第二阶段测试结果

```sh
make BUILD=build-active-frame/work test-page-webapi-context-asan
make BUILD=build-active-frame/work test-page-context-asan test-dom-context
make BUILD=build-active-frame/work test-cookie-cors test-native-mo test-rejection-checkpoint
make BUILD=build-active-frame/work test-page-timers test-dom-iface test-frame-bootstrap-wiring
make BUILD=build-active-frame/work test-parser-script-order test-parser-script-events test-legacy-home
make BUILD=build-active-frame/work test-xhr-constants test-mk-wired
```

- 新联合测试 67/67，ASan/UBSan 无诊断；两个真实 runtime 共存，fetch headers/body 分开交付，父子更新各自 DOM，CORS 拒绝与允许、导航/存储/来源隔离、非当前 DOM 的原生 observer、Promise 错误、关闭和重开均有消费者断言。
- 三个负对照是正门禁前置：共享导航字段导致 2 项失败；错误使用子页面自身作为 Cookie site 导致 3 项失败；原生 observer 订阅丢失 owner 导致 1 项失败。测试必须确实失败在指定断言，不能用编译失败替代。
- Clang AST 字段检查及删除一个 owner 字段的自负对照通过；WebAPI 27、platform 20、page 42、DOM 32。平台检查包括 native MO/rejection include。
- 原 page contexts 34/34（含 sanitizer 重跑），DOM contexts 86/86、native MO 23/23、rejection checkpoint 12/12、DOM iface 58/58、page timers 32/32、XHR constants 15/15、legacy home 95/95；Cookie/CORS、parser order/events、frame bootstrap 对应目标通过。
- 共享树的 `test-mk-wired` 一度被另两份新片段阻断。本次未改这些片段或 Makefile；最终重跑通过：369 fragments、368 reachable、1 declared。该数字是共享工作树观察值。
- **旧 `test-webapi` 总门禁未通过**：同一批 234 项测试分别链接 HEAD 的 `js_webapi.c` 和本次文件，均为同样 6 项失败（异常 URL 一项，后续 POST wire 五项）。Headers raw 负对照另外仍固定旧的 227/6 计数，当前是 234/12。未放宽门禁、修改该测试或宣称全绿。基线与当前日志分别为 `build-active-frame/webapi-baseline-run.log`、`webapi-current-run.log`。

联合测试日志 `build-active-frame/webapi-platform-final.log`；旧目标日志 `webapi-platform-regressions-rest.log`、`webapi-platform-targeted-regressions.log`；接线最终日志 `mk-wired-final.log`。前者保留正常版和 sanitizer 的两次 67/0。没有性能提升、全浏览器无泄漏或真实 Google 搜索成功的验收结论。

### 第二阶段客机：真实顶层网络与输入回归

最终重新构建 `build-active-frame/work/browser.aex`，重新打包独立 disk.img（保留 5 个用户状态 inode），仍使用已有 ISO。共享树其它源码也可能在构建间更新，因此最终包再次独立运行，而不是沿用第一次截图。

```sh
python3 -m http.server 9876 --bind 127.0.0.1 --directory tests/fixtures/document-platform
SITE_LOAD=90 SITE_PAINT=30 python3 tests/qmp/qmp_site.py \
  --iso build-active-frame/work/logit.iso --disk build-active-frame/work/disk.img \
  --name document-platform-final --url http://10.0.2.2:9876/index.html \
  --out build-active-frame/guest-platform-final.json --boxes \
  --input-id entry --input-text platform-ok --keep
```

`tests/fixtures/document-platform/index.html` 用普通 fetch 和 XHR 分别读取 `body.txt`，检查响应字节后由 timer 修改原生文本节点；MutationObserver 校验 target/oldValue，才显示成功。最终串口记录 `PLATFORM-GUEST-PASS`、`PLATFORM-INPUT:platform-ok`，两个 HTTP 响应各完成 20 字节，服务端也记录两次资源请求。最终报告 PAINTED，0.7 秒加载、12020 改变像素，无 JS 异常；已检查 `build-active-frame/document-platform-final.input.png` 的成功文字和完整输入值。

宿主不能直接访问 QEMU 的 10.0.2.2 地址，报告的 host inventory 不可用；不以“无子资源缺口”字段宣称完整资源清单已核对。这里的两次资源成功由 guest fetch 日志、服务端日志及页面字节断言交叉证明。

最终 SHA-256（disk 为客机退出后的文件）：

```text
a1f79538f17ac3e8d57d38eaee284c04e0f85ec569dd17ee53b09d03384b55bd  browser.aex
c7589f74df93dab82c3c8931719d85304c912b4d0cd5b0f659ed9b33b4419df9  disk.img
0f80979f37e8a17e590b45e7579f26231ae137607f99047c2f9926784190ec73  logit.iso
```

测试启动的 HTTP 服务已停止，QMP 客机已退出；未提交 Git。该客机证明顶层回归，不证明新 context API 已与网页 iframe 连接。下一阶段仍需真正连接资源加载、子窗口通信、布局与输入，然后再进行用户操作的 Google 验证流程。

## 第三阶段：网络 iframe 的实际内嵌消费者

本节更新前两节的阶段性状态，保留旧结论供核对。`passive_frame.c` 现在为允许执行的网络子文档建立独立 page/DOM/WebAPI/platform context，并在 iframe 原位置绘制子页面；不是通过新标签页替代内嵌。仍保留原文件名以及不支持功能的拒绝路径。

- 每个子文档有自己的经典脚本队列、请求、DOM、样式布局、图片、定时器、MutationObserver 和 Promise 调度。原生输入按实际绘制裁剪/变换命中子视口，处理点击、滚动与键盘事件；不支持的变换拒绝路由。
- `embedded_window.inc` 在接收方 runtime 创建稳定 WindowProxy，跨 runtime 只传序列化数据。投递检查来源、目标 origin、宿主节点连接状态和文档 epoch；移除后同一回调重新插入也不能复活旧消息。原生 MessageEvent 不授予用户激活。
- 新消息会唤醒主循环。真实客机曾出现“父页收到点击并回复、子页永远等待”的失败；缺失队列 pending 信号是已验证的原因，修复后通过相同客机消费者。
- 子文档 CSP 按策略交集限制脚本、内联处理器、字符串 eval/Function 构造器、fetch/XHR 与每次重定向；动态 meta 策略由原生 DOM 订阅收紧。sandbox、未实现的 Trusted Types 要求、module/integrity 等不能假装支持。没有取消 CORS、修改身份或操作验证令牌。
- 嵌入响应与经典脚本上限提升至 4 MiB，总子文档资源预算 8 MiB。此前 transport 的 512 KiB 上限同样需要修改，不能只改消费者常量。
- 完整 selector/DOMTokenList/CharacterData 使用各 runtime 的闭包和当前 DOM owner，没有可变全局文档状态；补入已启用 platform 的子文档，仍不启用其它单例模块。

### 第三阶段证据与失败边界

`build-active-frame/inline-native-message-gates2.log` 对应 selector 接入前的最终门禁：WebAPI/context 82/82（普通版与 ASan/UBSan）、page timers 32/32、DOM owner 字段 33、frame bootstrap 通过；12 个 passive 模式及 active/active-large 通过，7 个 sanitizer 模式通过。接线当时为 375 fragments、374 reachable、1 declared（共享树观察值）。策略单测另为 99/99。输入、消息投递、旧脚本上限、消息唤醒的四个负对照必须失败在指定消费者断言；CSP connect/eval 也有独立负对照。

真实客机使用 `tests/qmp/active_frame_guest.py`、独立 disk 与 snapshot；两个本机 HTTP 服务使用不同端口来源。`guest-inline-final/results.json` 记录实际 1,049,202 字节外部脚本、20 字节 fetch、原生点击，以及父子双向可信消息，最终绘制 `FRAME-GUEST-PASS`。页面来源不是 Google，此结果不等于验证码验收。点击驱动修复为既有 `click_at_confirmed`：先核对客机真实指针位置，避免以脚本缓存坐标误判产品输入。

此客机 AEX SHA-256 为 `11eaadc5af939c5f9e3626ef0380b78d44dd4e4464397f2f058b9dd2e2e2a0bf`，证据目录 `build-active-frame/guest-inline-final/`。同次普通 Google 搜索返回 429 验证页；子页面 856,657 字节外部脚本成功执行，后续初始化仍报 undefined callee。截图仍是空白嵌入区域与打开页面提示，**不可交付为可操作验证码，更没有搜索结果验收**。没有点击或回答真实验证码。该次客机已为重建而退出。

未消除的限制：深度一、最多四个活动网络 frame；隐藏/无几何的 frame 延后启动；经典脚本顺序不等于完整 parser/async/defer 语义；动态新样式表、子文档导航、同源直接 DOM、嵌套 frame、原生表单编辑及 CSSOM/media/canvas 等仍未全面接入。不可用脚本的提示已改为“嵌入页面部分脚本不可用”，避免把部分执行说成完全没运行。

另一次 `test-message-port-platform` 为 226 项、4 项失败（attribute oldValue 及 indexedDB/crypto.subtle/attachShadow 的缺失断言）；本次未做对应 HEAD 基线归因，不能断言均为旧问题。`test-message-port` 21/21 通过。第二阶段列出的旧 WebAPI 6 项失败也未通过。以上均未通过放宽断言掩盖。

### 第三阶段后续：selector 与消息后布局

selector 接入后的 `inline-selectors-rebuilt.log` 为 85/85，普通版和 ASan/UBSan 均通过；撤回 selector 接入的负对照为 85 项、2 项失败。原生 frame 14 个普通模式及 7 个 sanitizer 模式继续通过。`inline-tail-gates.log` 的 frame bootstrap 与 iframe policy 99/99 通过，接线为 377/376/1（共享树观察值）。中途改提示头文件恰好与多源编译重叠，产生新旧文字混合的二进制；已用 `-W c/apps/browser/passive_frame.h` 完整重建验证，未放宽文字断言。

`guest-inline-selectors` 捕获第二个独立真实调度失败：父子回调都成功，但子页最后修改发生在布局之后；消息队列已经排空，程序进入等待，画面仍是 `FRAME-CLICKED`。之前成功截图受到额外输入唤醒影响，不能证明完全静止时也会更新。修复为每个外层调度回合开始先投递消息快照，再扫描父 DOM/策略和布局子文档；父页消息回调修改由 Browser 再次 settle 后绘制。没有添加轮询空转或依赖鼠标移动。

`inline-message-layout-gates.log` 是该修复后的正常/ASan/UBSan frame 回归，全部通过。客机驱动进一步要求父页 `PARENT-MESSAGE-PASS` 和子页 `FRAME-GUEST-PASS` 两段实际绘制文字，而非仅检查 console。启动还必须等待系统自动启动 Finder 后再标记 Browser 点击窗口，避免把异步 Finder 启动误归因成点错应用。

本次最终构建与独立打包日志为 `inline-message-layout-gates.log`、`inline-message-layout-pack.log`；保留 5 个用户状态 inode。最终制品 SHA-256：

```text
a677bc639b80cf2945b90903b69acd407f2df47e7b7c0d4aa14449ee7931dae6  browser.aex
6d28614effe86f757441b56bda15138e8df78ac5b221338f20e0c2d0d94e963c  disk.img
0f80979f37e8a17e590b45e7579f26231ae137607f99047c2f9926784190ec73  logit.iso
```

最终客机 `build-active-frame/guest-inline-layout-final/results.json` 的本机跨来源 fixture 为 PASS：外部脚本 1,049,324 字节、fetch 20 字节，父子实际画面均更新成功，已检查 `embedded-pass.png`。同一 AEX 普通导航至真实 Google 后仍返回 429；844,362 字节外部子脚本执行成功，初始化明确失败于 **`ReferenceError: 'Worker' is not defined`**。`public-observation.png` 仍没有可操作验证码，`public_search_accepted` 明确为 false。

检查现有 `js_worker.c`：Worker 表、任务链与调度状态仍为共享全局，`js_worker_close_all` 关闭全部 Worker；子文档尚未有对应 owner/context 与 CSP Worker 加载策略接入，不能直接打开安装开关或提供空构造器冒充实现。本次没有修改 Worker 模块。这是下一项明确的兼容性缺口，尚不证明它是最后一项。

最终独立 QEMU 为 PID 79422，QMP `/tmp/active-frame-31gl6hcc/qmp.sock`，停留真实页面供人工查看；测试本机 HTTP 服务已关闭，fixture 标签不是可重新加载的常驻服务。没有点击、回答验证码或声称搜索页通过。未修改默认运行磁盘，未提交 Git。

## 第四阶段更新（2026-09-16）

上一节的 Worker 缺失状态已由独立 owner 和受限 Blob classic Worker 实现推进；旧 PID 79422 已在核对启动参数后退出，以便重建同一独立测试磁盘。当前实现、宿主负对照、真实客机消费者和仍未通过的 Google 验收见 [嵌入 Worker 验证记录](BROWSER_EMBEDDED_WORKERS_2026-09-16.md)。这不是完整 Worker 标准实现，也不代表 Google 验证页已经可操作。
