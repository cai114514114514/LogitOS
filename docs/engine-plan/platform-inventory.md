# 浏览器平台组件盘点与基础模块路线


**第二批更正（2026-09-09，保留下面首次审查的旧结论）：WebCrypto 真实运算与 localStorage 双槽持久化已接入；具体支持边界与 host/guest 证据见[统一接线报告](expansion-status-2026-09-09.md)。下面 snapshot/无持久化/无顶层消费者等原判断只描述第一批时点。**

日期：2026-09-09。范围：加载、网络 WebAPI、存储、Worker、多媒体/解码与平台安全边界。初始盘点阶段只读代码并撰写本文；随后获授权实施 Web Storage 基础模块，结果与边界追加在第10节，保留原盘点结论供对照。没有新跑 guest、外网请求或兼容性评分。TLS 单页诊断暂停，原报告保留在 `/tmp/logitos-pass4-tls-diagnosis-0909/result.md`。

**结论：需要补齐平台服务的共同生命周期和多 realm 接口，再扩展 Worker、存储和 Service Worker。** 当前已经有真实加载器、流式 Fetch、HTTP/2、独立 Worker runtime、MSE 与多种解码器；重新造一套构造器外壳会增加重复状态，不能形成完整引擎。

状态口径：**已实现**表示所列有限能力有实际执行路径；**部分**表示有真实子集但重要语义/生命周期缺失；**缺失**表示在本轮检查的生产入口没有实现，或明确拒绝。任何一项都不等于全面符合标准。下列测试是已有验收入口，本轮没有重跑，不能引用旧通过数作为当前证明。

## 1. 生产连接图与所有权

```text
browser.c: load_once / main loop / native events
  ├─ bfetch + resource cache → HTTP/1.1 / hpool → socket syscall
  ├─ js_module → QuickJS compile + static graph prefetch → bfetch
  └─ js_page: one active page JSRuntime / JSContext
       ├─ js_webapi: Fetch / XHR / SSE → bxfer → HTTP/1.1 or HTTP/2
       ├─ js_worker: separate JSRuntime per dedicated worker, same host thread
       ├─ js_frame: same-origin JSContext on parent runtime
       ├─ js_idb / js_cache: JSContext-local in-memory stores
       └─ js_media → js_media_src → demux / audio+video decode / avclock / OS output
```

`Makefile` 展开续行后，`BROWSER_JS_SRC := browser.c $(sort $(wildcard c/apps/browser/js_*.c))`，平台绑定不是靠手工枚举文件接入；但**链接不等于安装**，实际安装顺序在 [js_page.c](../../c/apps/browser/js_page.c) 的 `js_page_open`，推进在 `js_page_run_due`，撤销在 `js_page_close`。`BROWSER_PIPE` 包含 HTTP/1、HTTP/2、HPACK、cookies、hpool 等；`http_cache.c` 由 `browser_rt.c` 文本包含，不能另外链接一份。

核心所有权问题：`js_dom`/`js_page` 的活动文档状态，以及 `js_webapi` 的 `g_loc`、`g_fetch`、`g_mk_response` 等仍是文件级单例。Worker 已经存在，却不能再次安装页面版 WebAPI；同源 iframe 采用另一套轻量 document 包装规避覆盖父文档。这是多数后续组件的共同依赖，不是某个站点的特殊问题。

## 2. 加载与执行平台

| 组件 | 状态与当前能力 | 源入口、依赖与具体边界 |
|---|---|---|
| 主文档/静态资源请求 | 已实现 | [browser_rt.c](../../c/apps/browser/browser_rt.c) `bfetch_start/pump/take/release`、[bfetch.h](../../c/apps/browser/bfetch.h)；非阻塞 socket、H1 parser、origin pool、redirect、range、HTTP cache。`browser.c` 负责文档与资源阶段，导航关闭旧请求/池。 |
| HTTP cache / tab资源 | 部分 | [http_cache.c](../../c/apps/browser/http_cache.c)、[tabs.c](../../c/apps/browser/tabs.c)；有 Cache-Control/validator/Vary 策略和 tab内容保留。进程内缓存，非磁盘 HTTP cache；需要与未来 partition key、权限策略一致。 |
| HTTP/2 Fetch适配 | 已实现 | `browser_rt.c` `bxfer_open/start/pump` → [http2.c](../../c/net/http/http2.c)/[hpack.c](../../c/net/http/hpack.c)，ALPN、同 origin复用和多 stream。静态 bfetch GET路径仍以 HTTP/1.1 为主。已有 `test-h2mux`、`test-fetch-header-order`。 |
| ES module / chunk | 部分 | [js_module.c](../../c/apps/browser/js_module.c) `mod_normalize/mod_loader/mod_compile_and_prefetch/js_module_eval`；相对/绝对URL、`import.meta.url`、QuickJS dynamic import和TLA job路径；通过 `JS_EVAL_FLAG_COMPILE_NO_RESOLVE` 发现静态依赖并预取。发现一层后仍 `bfetch_prefetch_wait`，不是可挂起的完整异步 graph scheduler。 |
| Import maps | 盘点时缺失；本轮部分实现 | 原检查时 `js_module.c:151` 对 bare specifier明确抛 TypeError，`type=importmap` 不建立映射。**本轮更正：主线已新增 [js_importmap.inc](../../c/apps/browser/js_importmap.inc)，接入imports/scopes、prefix与late-map限制子集；验证结果由主线汇总，不沿用“完全缺失”。** Worker共用graph/完整异步loader仍是后续依赖。 |
| 页面任务与微任务 | 部分 | [js_page.c](../../c/apps/browser/js_page.c) `pending/run_due/pump`：timer、rAF、Fetch/WebSocket、worker任务、QuickJS jobs、CSS动画；有脚本预算和 interrupt。当前各模块自带队列/单例，缺少共享 task source、realm owner、取消 epoch与统一调度契约。 |
| History / URL /滚动状态 | 部分 | [js_webapi.c](../../c/apps/browser/js_webapi.c) history/location， [js_url.c](../../c/apps/browser/js_url.c)/[js_urlbind.c](../../c/apps/browser/js_urlbind.c)，[js_cssom.c](../../c/apps/browser/js_cssom.c)通过embedder回调同步真实滚动。基本SPA同文档导航存在，不能等同完整 session-history/BFCache/navigation API。 |
| WASM | 部分 | [js_wasm.c](../../c/apps/browser/js_wasm.c) → `c/lib/wasm` MVP解释器；真实compile/instantiate/import/trap/memory成长绑定。并非现代WASM SIMD/threads/完整提案集；不应因有WebAssembly对象便给出全能力答案。 |

请求上限也属于契约：`browser_rt.c:64` bfetch 16项，idle60s/total90s，first-byte12s；`js_webapi.c:719` 原生Fetch 8项，JS层还有排队，high-water512KiB；H2 stream body cap在 `browser_rt.c:1648` 为32MiB。它们有真实内存/背压意义，但目前没有统一页面总预算管理。

## 3. 网络与基础 WebAPI

| 组件 | 状态与当前能力 | 源入口、依赖与具体边界 |
|---|---|---|
| Fetch / Request / Response / Headers | 部分 | [js_webapi.c](../../c/apps/browser/js_webapi.c) `wf_*`、JS prelude、`js_webapi_pump`；headers先resolve、流式body、取消、redirect、credentials/CORS/preflight、body消费与异常。当前页面 realm绑定；Request保存 `integrity` 字段（:3763/:3833），本轮没有发现校验消费者。 |
| Streams / SSE / XHR | 部分 | `js_webapi.c` 内真实 ReadableStream body、EventSource帧解析/重连、异步XHR；high-water向网络回传暂停。不能据此声称全WHATWG Streams（BYOB/transfer等）已完成；同步XHR明确不在此路径。 |
| Blob / File / FormData /编码 | 部分 | [js_blob_prelude.inc](../../c/apps/browser/js_blob_prelude.inc)、[js_encoding_prelude.inc](../../c/apps/browser/js_encoding_prelude.inc)、`js_webapi.c` Request/body序列化。真实数据容器/对象URL与编码入口已有；文件选择、任意本机文件能力不因此自动存在。 |
| WebSocket | 部分 | [js_websocket.c](../../c/apps/browser/js_websocket.c) → [ws.c](../../c/net/http/ws.c)；专用H1 socket、真实Upgrade验证、消息、错误与close推进。`WebSocketStream`缺失，permessage-deflate不协商，不能把不支持的扩展当成功。 |
| MessageChannel / postMessage / BroadcastChannel | 部分 | [js_platform.c](../../c/apps/browser/js_platform.c)约564–846；有任务投递、clone及origin检查。BroadcastChannel注册表局限当前页面context，不是跨tab/worker共享总线；worker transfer list明确拒绝，不能用复制冒充转移。 |
| 随机数 / WebCrypto | 部分 | `js_platform.c` getRandomValues/randomUUID连接内核随机源；[js_subtle.c](../../c/apps/browser/js_subtle.c)有算法/usage校验和raw/JWK key材料导入导出。**digest/encrypt/decrypt/sign/verify/derive等运算仍拒绝 NotSupportedError**（:457起），不是已完成WebCrypto。底层 `c/crypto`可复用，但需实际绑定、link与失败测试。 |
| 平台设备/交互服务 | 缺失或有限 | clipboard文本入口存在于 `js_platform.c`；sendBeacon明确返回false表示未发送。此次生产源搜索没有找到 AudioContext、MediaRecorder、getUserMedia、RTCPeerConnection、WebCodecs、文件picker实现。没有明确消费者时保持缺失，不能创建会“成功”的空对象。 |

下层网络具备 [sock.c](../../c/net/core/sock.c)非阻塞生命周期、DNS/IPv4/IPv6路由、Happy Eyeballs及 [tls.c](../../c/net/tls/tls.c) TLS1.2/1.3、证书/主机名验证与 [tls_psk.c](../../c/net/tls/tls_psk.c) ticket缓存。它们是可用基础，不是完整浏览器网络策略层。未见浏览器QUIC/HTTP3路径。暂时TLS失败分类/重试诊断保留为后续维护任务，不作为当前逐页调试工作继续展开。

## 4. 存储与 Worker

| 组件 | 状态与当前能力 | 源入口、依赖与具体边界 |
|---|---|---|
| localStorage | 部分 | `js_webapi.c:321`起，C层按origin存储，活到browser进程结束、可跨页面导航；8 origins，每store256条/256KiB。没有磁盘持久化。 |
| sessionStorage | 部分；tab分区本轮补齐 | 初始同一 `g_stores` 仅按session标记分开，注释明确“与localStorage相同，但应为per-tab”。**本轮更正：storage_backend按origin+kind+tab id分区，主线已接tab销毁清理；细节与验证边界见第10节。** |
| IndexedDB | 部分 | [js_idb.c](../../c/apps/browser/js_idb.c)：内存数据库、事务/request/index/cursor子集、统一settle任务与abort。JSContext-local，导航重建会丢；Blob/File值拒绝，worker/frame未安装。已有 `test-idb`及negative prerequisite。 |
| CacheStorage | 部分 | [js_cache.c](../../c/apps/browser/js_cache.c)：Cache匹配、put/add/addAll、Vary与响应body消费，64MiB配额，Promise终止路径。页面context内存存储；worker无caches，非持久离线缓存。已有 `test-cache`配额/拒绝控制。 |
| Dedicated Worker | 部分 | [js_worker.c](../../c/apps/browser/js_worker.c)：每worker独立QuickJS runtime，classic脚本、importScripts、消息clone、timer、promise jobs、terminate/watchdog/error；同线程交错执行，最多8个，不提供并行计算。module/nested workers和transferables明确拒绝；共享页面Fetch/IDB/Cache服务尚未安装。 |
| SharedWorker | 缺失 | `js_worker.c:49`明确不定义。依赖跨文档服务寿命、共享通信、origin key与reference counting。 |
| Service Worker client表面 | 部分，执行缺失 | [js_swreg.c](../../c/apps/browser/js_swreg.c)执行URL/secure origin/scope检查后register始终拒绝；controller为null；ready拒绝是显式规范偏差。FetchEvent/respondWith/Clients、安装激活及fetch截获缺失。不能把“有navigator.serviceWorker”算作离线应用支持。 |
| 持久存储后端/配额协调 | 缺失 | 未见上述三类store共享的磁盘backend、事务提交/恢复、跨realm锁、统一quota/eviction。**不要照抄旧注释“VFS无pwrite所以不能做”作为当前结论**：当前 libc `io.c:80`已有SYS_FSYNC入口，底层可靠性需单独核对；本次确认的是浏览器没有接持久backend，不是重新审定整个文件系统。 |

可先实现真实内存backend和精确生命周期，并让“持久”能力显式不可用；不能把重启会丢的数据包装成已落盘。持久化开启的前提是写失败传播、原子替换/提交点、fsync行为及恢复fixture有证据。

## 5. 多媒体与解码

| 组件 | 状态与当前能力 | 源入口、依赖与具体边界 |
|---|---|---|
| HTMLMediaElement /普通src | 部分 | [js_media.c](../../c/apps/browser/js_media.c) `applySrc/ensureSrc`（:1210后）、load/play/pause/currentTime/events → `mel_*`；普通src通过Fetch取得数据，markup src也会消费。不是仅有MSE外壳。 |
| MSE | 部分 | [js_media_src.c](../../c/apps/browser/js_media_src.c) + js_media.c：SourceBuffer追加、range/remove/offset、demux/decoder/clock、局部画面与音频输出；4 SourceBuffers/8 elements/48MiB append cap。当前累积buffer并按完整box前缀重开demux，lazy减轻但仍有累计重复解析成本；不是通用增量demux。 |
| 字幕 | 部分 | [subs.c](../../c/lib/media/subs.c)解析WebVTT/SRT；js_media.c `mel_subs_active`、`js_m_trackload`、ensureTracks已fetch并绘制默认track。完整TextTrack/CueList编辑、语言选择、多轨控制未据此证明。 |
| 容器 / AV时钟 | 已实现有限集合 | [demux.c](../../c/lib/media/demux.c)、[mp4.c](../../c/lib/media/mp4.c)、[mkv.c](../../c/lib/media/mkv.c)、[avclock.c](../../c/lib/media/avclock.c)：内容嗅探、MP4/fMP4/Matroska、sample时间戳/seek与丢帧策略。当前whole-file接口是流式媒体扩展的直接依赖。 |
| 音频解码 | 部分 | `c/lib/audio/{wav,mp3,flac,vorbis,aac,opus}.c`，真实PCM生产；Opus CELT子集，SILK/hybrid明确拒绝。浏览器 `js_media_src`直接消费AAC/MP3，库内有某编码器不代表媒体页面所有容器组合都可播。 |
| 视频解码 | 部分 | `c/lib/video/h264*.c`、`h265*.c`、`mpeg12*.c`、`mjpeg.c`真实解码；浏览器媒体核心主要直接接H264/H265。具体profile/帧型/分辨率需各自gate，不从文件存在推断全覆盖。未见C侧VP9/AV1生产解码器；Rust VP8 inter支持还须核对feature/link/消费者才能计入网页视频能力。 |
| 图像解码 | 已实现有限集合 | [img.c](../../c/lib/image/img.c)统一注册/EXIF；C JPEG/GIF/SVG，Rust PNG/APNG、BMP/ICO、WebP等入口在 [lib.rs](../../rust/src/lib.rs)。未发现AVIF生产注册路径，不能因某shim/注释提及格式便标支持。动画、色彩/方向与缓存由消费者共同决定。 |
| WebAudio /采集/RTC/WebCodecs/DRM | 缺失 | 本轮browser生产源搜索没有这些执行入口。音频播放syscall、decoder和MSE不是这些API的实现；需要各自数据流、权限、实时调度与资源释放合同。 |

已有验收入口包括 `test-mse`/`test-mse-os`、`test-subs`、`test-demux`、音频codec gates、各视频codec gates及图像fixtures。本轮不重新发布CLAUDE的旧红绿状态，也不以host“decoded”代替guest实际shown/PCM输出。

## 6. 安全与资源边界

| 边界 | 状态 | 当前证据与需要的组件 |
|---|---|---|
| origin / cookies / CORS | 部分 | Fetch `wf_needs_preflight/cors_check_response/cors_check_preflight`、[cookies.c](../../c/net/http/cookies.c)及document.cookie共用jar，HttpOnly/Secure/SameSite相关检查存在。bfetch资源与JS Fetch仍有不同策略入口，未来新增loader必须接同一request policy，不能默认继承已安全。 |
| iframe realm / sandbox | 部分 | [js_frame.c](../../c/apps/browser/js_frame.c)同源新context；`js_platform.c:3350`起对cross-origin或sandbox frame显式拒绝。尚无完整跨源frame代理与sandbox能力；拒绝是现实现边界，不是缺一行allow属性。 |
| secure-context / permissions | 部分 | `js_platform.c:860`仍固定 `isSecureContext=true`；swreg自用实际URL单独检查。缺统一secure-origin与permission决策对象，多个API各自判断易漂移。 |
| CSP / SRI /跨源隔离 | 缺失生产执行入口 | 本轮browser源搜索只找到Request.integrity字段，没有CSP/Permissions-Policy/COOP/COEP执行器或SRI校验消费者。需要响应policy解析、document政策对象及每条加载/执行入口的统一检查；不能把TLS等同网页沙箱。 |
| JS/网络/媒体资源预算 | 部分 | `js_page_open`有2MiB JS stack及interrupt预算，worker有task watchdog，Fetch/Cache/MSE分别设cap。未见页面runtime调用JS_SetMemoryLimit，也没有共享“realm+总进程”预算仲裁。需要错误可传播的分配、任务公平性和统一撤销。 |
| 进程隔离 | 缺少浏览器站点/decoder隔离 | 各页/worker/同源frame主要在同一browser进程，worker的独立JSRuntime不等于独立进程。媒体/C解码器也是进程内调用；目前应精确记录边界，不能宣称Chrome式renderer sandbox。 |

## 7. 可直接开始的连贯基础模块

以下是实施包，不是一次性暴露所有Web API。文件名为建议，尚未创建产品实现。

| 顺序 | 模块与可交付行为 | 前置依赖 / 完成判据 |
|---|---|---|
| P1 | `platform_context.[ch]`：显式context owner（realm ID、document/tab ID、origin、base URL、epoch、关闭状态），注册/取消异步资源；从WebAPI location/request持有者开始迁移。 | 先保持页面行为，用两context同时创建请求/关闭其一的fixture，另一方仍收到自身结果；旧epoch回调不得触达新页。必须真接 `js_page_open/close`，不能只有结构体。 |
| P2 | `request_policy.[ch]` + loader上下文：为bfetch与JS Fetch共同携带destination、initiator origin、credentials、mode、redirect policy、abort owner；保留两种transport，以同一个策略入口作决策。 | P1；先cover真实已有CORS/cookie语义及redirect跨origin，随后secure context、mixed content、SRI/CSP按可执行规则逐项接入。所有资源门都受控才算闭环，不能只在fetch拦截。 |
| P3 | `platform_tasks.[ch]` + realm绑定的Fetch实例：task owner、task source、ready/deadline、有限poll、取消与microtask checkpoint；移除 `g_fetch/g_mk_response` 对单page绑定。 | P1/P2；同一真实网络fixture服务window+worker，事件进入正确context，关闭父页后全部settle/取消，无跨realm JSValue。让Worker实际可fetch后才安装API。 |
| P4 | `storage_service.[ch]`：统一origin/tab partition key、quota、backend ops、版本与transaction提交；先真实内存backend，localStorage进程寿命、sessionStorage按tab、Cache/IDB共享service。 | P1/P3；跨realm读写一致、跨origin隔离、独立tab session、quota/abort/close传播都可观察。持久backend作为后续独立能力，验证失败返回/commit/recovery后开启。 |
| P5 | `module_graph`扩展：import-map表和scopes、统一normalize、per-context module cache、可取消graph状态；复用QuickJS compile-no-resolve seam。 | P1/P2/P3；diamond/cycle/dynamic import/TLA/redirect后base和map解析fixture；Worker module必须用同一graph机制，不复制页面loader。先实现import maps可独立形成真实收益。 |
| P6 | `media_pipeline`增量层：feed/read/seek/reset式demux入口、共享decoder capability registry、output owner和memory quota；把MSE累计重解析换成增量消费。 | 保留现MP4/MKV与decoder契约；同一正常文件任意合法分片后sample/PTS/PCM/frame与whole-file对照一致，取消释放、无音频设备也明确失败。格式注册同一处供canPlayType/isTypeSupported/实际open。 |
| P7 | 真正Service Worker生命周期 | P1–P4且module需要P5：install/activate/waitUntil、registration scope、clients、FetchEvent/respondWith、window与静态资源统一截获、更新/注销/取消。整个链可运行后再把register从拒绝改成功；第一版可明确session-only，不能称持久离线支持。 |

可与P1并行的独立实用包：`js_subtle`接真实SHA摘要/HMAC/AES-GCM等经过核对的crypto库、ImportMap parser/normalizer纯模块、媒体capability表。WebCrypto每加入一个算法就完成真实运算与错误/向量对照，不用normalize-only“通过”计数驱动扩张。先不做依赖操作系统新权限/设备合同的RTC、采集或DRM外壳。

工期不确定性主要来自单例迁移时的JSValue归属、现有多个任务队列的顺序、存储提交保证与增量demux接口。P1/P2会触及多个现有模块，不能当“加两个新文件”估算；应逐包以真实消费者接入和negative prerequisite作为结束条件。此盘点未给无法据证支撑的天数或“完整浏览器完成百分比”。

## 8. 与旧文档的明确更正

- `CLAUDE.md`旧句“Video has no player / <video src>不可达”：当前 `js_media.c:1234`起已处理markup src，`ensureSrc/applySrc`连接Fetch和媒体引擎。**更正为：普通src路径存在，支持格式/流式能力及实际播放需按消费者验收。**
- 旧句“subs.c parsed and unreachable”：当前 `js_media.c:140`消费active cues，:1250起加载track。**更正为：解析、默认track加载和绘制存在，完整TextTrack API仍不全。**
- `js_webapi.h`旧句“no FormData/Blob / no WebSocket / no connection reuse”：当前Blob/FormData prelude、js_websocket及bxfer池化已存在。**更正为：这些组件已接入，能力边界见上表。**
- `js_idb.c`旧注释把localStorage寿命类比为one JSContext：当前 `js_webapi.c:321`的C store明确跨导航，IDB本身仍context内存对象。**两者不能再合并写成“同样durability”。**
- 旧注释“没有VFS positional write所以无法持久化”：本次不沿用为当前事实；浏览器未接持久backend是确认事实，kernel/FS提交能力需独立实证。

## 9. 本批基础模块的验收方法

先在已有source集合派生的host gate验证真实接口与生命周期，每个新增行为都有可观测红的negative control且为positive prerequisite；两realm、跨origin、关闭/取消、配额耗尽都用有限合法fixture，不以构造器存在计通过。`make test-mk-wired`保持可达，ring-3改动必须重建disk。最后由主线统一冻结快照，在guest跑综合平台fixture：普通资源+module、Fetch驱动DOM、worker消息/请求、origin存储、短音视频与字幕，采实际请求结果、paint文本、帧显示和PCM；再做少量真实网站兼容性验收。逐页异常回到所属组件修复，不再另开特定站点分支。

## 10. 本轮已落地：Web Storage 内存服务

新增 [storage_backend.h](../../c/apps/browser/storage_backend.h)/[storage_backend.c](../../c/apps/browser/storage_backend.c)，已由 [js_webapi.c](../../c/apps/browser/js_webapi.c) 的真实 localStorage/sessionStorage 消费者调用，旧 `g_stores` 内联实现删除。接口不持JSContext/JSValue，只接受规范化origin、local/session种类和稳定tab ID，未来可以接其他realm，但**本批没有安装第二realm的WebAPI**。

- localStorage按origin共享，存活至browser进程结束；sessionStorage按origin+tab分区，页面runtime重建不清空。主线已在page open前调用 `js_webapi_set_storage_session(tabs_active()+1)`，两处关闭tab入口在 `tab_dehydrate` 释放runtime后调用 `js_webapi_drop_storage_session`，然后才允许slot复用。
- wrapper持不可变partition key，不持可复用的backend slot地址；空读不消耗store配额，clear/remove最后一个key释放空area。保留16个非空area、每area256项/256KiB的有界内存策略；不自动驱逐其他origin内容。
- key/value改为显式字节长度，保留字符串内嵌NUL；替换值、创建新area仅在全部分配成功后提交，配额/分配失败保留旧值与顺序。JS coercion异常正确传播；完整platform绑定产生真实 `QuotaExceededError` DOMException。
- 为使所有现有窄host link与产品消费相同实现，`js_webapi.c`文本包含backend一次，沿用http_cache/wasm先例。产品 `UCFLAGS`已有 `-MMD -MP`、且Makefile包含`.d`，会跟踪include依赖；新增host gate另列c/h显式依赖。

验证入口：[tests/storage_backend.mk](../../tests/storage_backend.mk) 已由主线接入Makefile，源码列表分别从 `WEBAPI_TEST_SRC`、`PLATFORM_TEST_SRC`派生。

1. 修改前真实WebAPI消费者fixture：15检查/9失败，包含跨tab串值、NUL截断、异常种类问题；记录 `/tmp/logitos-storage-backend-0909/before.log`。
2. 修改后backend/JS消费者：37检查/0失败；恢复origin-only session分区的正式负控：37检查/3失败，明确打印 `different tab has independent session storage` 等失败。
3. 实际platform安装器/Storage属性代理/DOMException：3检查/0失败；恢复旧RangeError的正式负控：3检查/1失败，明确打印 `storage quota is a real DOMException`。两个负控均为positive prerequisite，记录 `complete.log`，主线也复跑确认通过。

验证边界：这是host真实源码消费与源码tab接线核对，**没有本批guest双tab实测，也没有持久化/跨realm/多线程证明**。ring-3 disk构建与综合guest验收由主线统一冻结版本完成，不在此文虚报完成。

明确未交付：磁盘backend/提交恢复、storage事件广播、IDB/Cache统一后端、并发锁、sessionStorage复制opener语义、跨realm共享绑定。opaque origin目前仍继承原WebAPI按raw URL字符串建key的偏差，须等document/realm origin服务提供真实opaque token后统一处理；本批不以伪origin掩盖该限制。
