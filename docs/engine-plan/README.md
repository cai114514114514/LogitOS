# 浏览器引擎整体雏形与组件总表

2026-09-09。执行方式按用户要求改为：**先把组件版图和成组基础实现铺开，再逐层调试网页。** 不再以某页某个像素为本轮工作单位。

这份表覆盖引擎、平台服务和常见网页组合组件，作为后续持续扩展的总目录。Web 平台仍在演进，不能把任何一张有限清单称为“地球上全部组件”。这里尤其区分“已有可执行子集”“已确认缺口”“尚未核对”；没有把已有构造器当成完整实现，也没有把所有候选都说成已证实缺失。

源码证据与详细接口分别在 [布局与绘制](layout-inventory.md)、[DOM 与运行时](dom-runtime-inventory.md)、[加载、存储与多媒体](platform-inventory.md)。下表是跨模块索引；具体状态以这些带函数入口和边界的审查记录为准。

第二批扩展与当前验证见 [2026-09-09 统一接线报告](expansion-status-2026-09-09.md)。下文第一批的“仅内存”“Range snapshot”等历史边界保留，已有更正在各 inventory 原记录旁标注。

## 第一批实际代码

| 基础组件 | 产品入口 | 本批目标及边界 |
|---|---|---|
| 模块映射与图加载 | `js_importmap.inc`、`js_module.c/h`、`browser.c` | imports/scopes、包前缀、阻断项、受控合并、已解析记录、静态/动态 import 共用解析、导航清理；实际模块可以修改 DOM。HTTP(S) 子集；integrity map 明确拒绝。 |
| 文档任务所有权 | `page_runtime.c/h`、`js_page.c` | 文档代次、关闭状态、任务来源、有界队列、取消与真实 timer/rAF 消费；当前仍只有一个活动页面，不声称 iframe 多文档已经完成。 |
| 分区存储服务 | `storage_backend.c/h`、`js_webapi.c/h`、`browser.c` | local 按 origin、session 按 origin+tab；保留 NUL 字符、配额、分配失败原子性、关闭标签页释放；仍为进程内存，尚无磁盘持久化。 |
| 原子行内容器 | `css_engine.c`、`css.h`、`layout.c` | 将 inline-block / inline-flex / inline-grid 的外层行内语义与内部布局分开；复用已有块/flex/grid 布局。完整基线、书写模式等仍分批补。 |
| 综合组件样例 | `tests/fixtures/engine-components/` | 一个普通 HTML 页面组合布局、表单、事件、模块、存储和绘制，用同一页面观察组件连接；样例存在不算通过，接口存在不算行为通过。 |

最终构建、门禁和仍未完成的事项记录在 [本批交付与验证](foundation-status.md)。这一批是可继续扩展的第一层实现，未将下面整张总表标为完成。

## 组件版图

表内“补”指确认仍需实现或完善；“核对”是扩展候选，尚不能确定源码覆盖度。已有实现的具体缺口不抹掉，未来实现后应在原记录旁增加更正。

| 组 | 组件清单 | 当前基础与下一批缺口 |
|---|---|---|
| E01 文档装载 | URL、重定向、主文档、MIME、编码、base URL、失败页、下载 | 有真实加载；补统一导航上下文、取消状态、响应策略和非网页响应处理。 |
| E02 资源调度 | CSS、图片、字体、脚本、preload、prefetch、modulepreload、优先级、去重、解码队列 | 已有资源表/预取；补不同消费者共享请求所有权、取消、公平性；核对提示类资源是否真消费。 |
| E03 HTTP 与连接 | HTTP/1.1、HTTP/2、流复用、连接池、压缩、缓存验证、Vary、Range、HTTP/3 | H1/H2 有基础；补共享加载策略和缓存持久化；HTTP/3 尚无浏览器路径。 |
| E04 HTML 分词/建树 | DOCTYPE、实体、注释、raw text、模板、表格纠错、外来内容、脚本暂停点 | 已有真正 HTML parser；补 streaming parser 与脚本交错执行、document.write 插入点，而非重写标签扫描器。 |
| E05 文档模型 | Node、Element、Document、DocumentFragment、Text、Comment、Attr、集合、命名空间 | 已有丰富 DOM；补统一文档所有权、原生 mutation 通知、跨文档 adopt/import 的一致生命周期。 |
| E06 DOM 查询/序列化 | ID、class、tag、querySelector(All)、matches、closest、inner/outerHTML、DOMParser、XMLSerializer | 有实现子集；补 selector/parser 统一语义、独立文档一致性、XML 解析。 |
| E07 组件封装 | template、slot、ShadowRoot、Custom Elements、upgrade、ElementInternals、表单关联 | 已有真子集；补 shadow 事件路径/retarget、封闭树可见性、表单关联原生消费者。 |
| E08 ECMAScript 执行 | realm、Promise、microtask、异常、模块缓存、循环依赖、TLA、import.meta | QuickJS 与模块加载已有；补显式 realm 服务和任务所有权，不复制页面全局单例。 |
| E09 脚本装载 | classic、defer、async、module、动态插入、nomodule、import maps、动态 import | import maps 本批接入；补完整解析/执行调度、可挂起模块图、Worker 共用加载。 |
| E10 CSS 语法与级联 | stylesheet、声明、选择器、specificity、important、继承、initial/unset/revert、层 | LibCSS+扩展实现；补扩展声明完整优先级、cascade layers/revert-layer 和统一计算值合同。 |
| E11 CSS 条件与变量 | media、supports、自定义属性、fallback、循环、calc/min/max/clamp、container query、scope | media/变量/supports 有子集；容器查询、作用域及新数学函数逐项核对并补消费者。 |
| E12 样式树与失效 | CSSOM、computed style、inline style、rule 插入/删除、选择器依赖、子树重算 | 已有作用域失效；补原生 mutation 统一信号、变量依赖、布局前后几何快照。 |
| E13 普通流与盒模型 | block、inline、inline-block、匿名盒、margin/padding/border、box-sizing、min/max | 已有布局；本批原子行内盒，后续补完整 margin collapse、格式化上下文与百分比计算。 |
| E14 Flex | 主/交叉轴、grow、scaled shrink、basis、wrap、order、gap、align、baseline | 真布局存在；扩展边界与原子外层统一，基线/内在尺寸继续完善。 |
| E15 Grid | 显式/隐式轨道、fr、minmax、auto-fill/fit、area、placement、span、alignment、subgrid | 真布局存在；inline-grid 本批接线；subgrid 和完整 intrinsic track sizing 后续补。 |
| E16 Table/List | table/row/group/cell、caption、colspan/rowspan、border-collapse、list marker、counter | 表格/列表有子集；补 CSS table display、一致表格算法、计数器与复杂 marker。 |
| E17 定位与浮动 | float、clear、absolute、fixed、relative、sticky、containing block、z-index | 有基础；补定位/滚动/变换包含块统一，完整 sticky 与堆叠上下文需按行为验收。 |
| E18 流式排版 | 多列、分页、fragmentation、break-*、widows/orphans、shape-outside、exclusions | 作为独立布局组件扩展，当前不能声称分页/多列完整。 |
| E19 文字与字体 | 字形选择、shaping、kerning、连字、fallback、@font-face、WOFF/WOFF2、可变字体 | 已有字体与文本路径；补字体加载到布局失效闭环、复杂字形/变量轴/替代字体一致性。 |
| E20 国际文字排版 | bidi、RTL、vertical writing、CJK 换行、word-break、overflow-wrap、hyphenation、ruby | 不用拉丁文字页面证明这些完成；按整套文本排版算法核对/补齐。 |
| E21 文本装饰 | line-height、letter/word spacing、white-space、text-align、ellipsis、decoration、shadow | 有子集，line-height 归一化已落盘；补多行截断、装饰几何、复杂溢出。 |
| E22 绘制与合成 | display list、裁剪、圆角、opacity、transform、背景、渐变、阴影、filter、blend、mask | 有真绘制；补完整层级/局部坐标合同、复杂 clip、遮罩滤镜、合成缓存。 |
| E23 图像 | JPEG、PNG、GIF、APNG、WebP、BMP、ICO、SVG、AVIF、方向、色彩、动画 | 多格式真实解码；AVIF 未见生产注册；其余需确认具体 profile、动画和颜色消费者。 |
| E24 SVG | path、shape、text、viewBox、paint server、gradient、clip/mask、filter、use、DOM | 有 SVG 解码/绘制子集；作为图像可画不等于内联 SVG DOM、滤镜、动画全部可用。 |
| E25 Canvas/GPU | Canvas 2D、Path2D、ImageData、文字、图像、hit region、导出、OffscreenCanvas、WebGL/WebGPU | Canvas 2D 真消费者已有；图像编码导出、离屏与 GPU 上下文分别核对/扩展，不能返回假成功 context。 |
| E26 滚动 | 根滚动、元素滚动、横向滚动、scrollIntoView、scrollbar、scroll anchoring、snap、overscroll | 根横纵滚动已接真实坐标；补元素滚动树、统一命中/裁剪、锚定与 snap 状态机。 |
| E27 动画 | rAF、CSS transition、keyframes、WAAPI、timeline、scroll timeline、取消/完成 | 已有动画子集；补实际合成/布局时钟的一致推进、暂停恢复及滚动时间轴。 |
| E28 事件 | capture/bubble、once/passive、preventDefault、composedPath、retarget、EventTarget | 基础存在；补不可变传播路径、原生默认动作与 shadow 边界。 |
| E29 原生输入 | mouse、pointer、touch、wheel、keyboard、focus、Tab、pointer capture、composition/IME | mouse/key/focus 已有；构造器不算设备输入支持，补完整指针/捕获/组合输入状态机。 |
| E30 基本表单 | button、text/password/search、textarea、checkbox、radio、select/option/optgroup、label | 有真状态/编辑；补键盘、鼠标、JS activation 同源状态和更完整原生绘制。 |
| E31 扩展表单 | number/range、date/time/month/week、color、file、datalist、meter、progress、output | 分类型核对交互与提交；文件选择、日期选择器不能用普通文本外观冒充完成。 |
| E32 表单流程 | form owner、submitter、FormData、GET/POST、validation、reset、autocomplete、autofill | 有子集；统一提交/验证/重置默认动作，自动填充需用户数据与真实 UI 服务。 |
| E33 富文本 | Selection、live Range、边界调整、复制/粘贴、contenteditable、beforeinput、undo/redo | 现 Range 多为快照；补原生 mutation 边界更新后再补编辑操作、几何与编辑历史。 |
| E34 浮层与模态 | details/summary、dialog、popover、top layer、backdrop、inert、Escape、light dismiss | 已有状态 API；补共享顶层绘制/命中栈、模态输入限制、焦点恢复。 |
| E35 拖放与文件 | DragEvent、DataTransfer、拖入文件、文件选择、下载、Blob URL、FileReader | Blob/File 基础已有；补真实用户文件通路和生命周期，不能编造选择结果。 |
| E36 网络 WebAPI | fetch、Request/Response/Headers、XHR、ReadableStream、SSE、WebSocket、Beacon | 已有真网络子集；补跨 realm 实例化、完整 Streams、请求策略一致性；Beacon 当前明确失败。 |
| E37 二进制与编码 | ArrayBuffer、TypedArray、TextEncoder/Decoder、Blob、File、FormData、URLSearchParams | 基础存在；补 BYOB/transfer/大体积流式序列化及边界一致性，避免重复编码器。 |
| E38 存储 | localStorage、sessionStorage、IndexedDB、CacheStorage、cookie、quota、eviction、持久化 | 本批先统一 Web Storage 分区后端；IDB/Cache 仍各自内存服务，后续统一事务/持久性。 |
| E39 并发与消息 | dedicated/module/nested/shared Worker、MessagePort、postMessage、BroadcastChannel、transfer | dedicated worker 子集存在；补模块/嵌套、真实跨文档服务与 transfer 所有权。 |
| E40 离线应用 | Service Worker、注册更新、install/activate、FetchEvent、respondWith、Clients、离线缓存 | 注册表面存在但执行拒绝；需资源/JS Fetch 两条路径共同接通后再启用。 |
| E41 嵌入与导航 | iframe/srcdoc、WindowProxy、跨源访问、frame layout、history、BFCache、Navigation API | frame 轻量 context 存在；完整子文档 DOM/布局、历史与多活文档仍缺。 |
| E42 音视频播放 | audio/video、load/play/pause、seek、buffer、MSE、音画时钟、字幕、TextTrack、全屏/PiP | 有真实解码输出；补增量 demux、轨道选择、完整视频控件、更多容器/编码组合。 |
| E43 媒体生成/通信 | WebAudio、AudioWorklet、WebCodecs、MediaRecorder、getUserMedia、WebRTC、EME | 生产入口缺失；分别需要音频图、实时任务、采集/权限、编解码和资源释放机制。 |
| E44 网页策略 | origin、cookie/CORS、CSP、SRI、mixed content、sandbox、Permissions-Policy、COOP/COEP | cookie/CORS 等有子集；补统一响应策略对象与所有加载门的执行点，不能只解析字段。 |
| E45 密码学与身份 | getRandomValues、randomUUID、SubtleCrypto、Credential/WebAuthn | 随机源与 key 数据部分存在；SubtleCrypto 运算尚有明确拒绝，身份验证需真实平台服务。 |
| E46 页面/进程服务 | 文档 owner、任务源、预算、取消、runtime 销毁、renderer/decoder 隔离 | owner/队列本批接入；多 realm 和进程隔离是后续工程，不等同多个 JSContext。 |
| E47 辅助能力 | accessibility tree、ARIA、role/state、焦点语义、屏幕阅读器、缩放、高对比 | DOM 属性存在不算辅助技术可用；需要到 OS 的真实可访问性消费者，逐项核对。 |
| E48 国际化与格式 | Intl、locale、collation、number/date、plural、segment、timezone | 有 Intl 模块；按算法/区域数据核对范围，不能以英语结果推断全地区覆盖。 |
| E49 页面生命周期/观测 | visibility、pagehide/show、freeze、Performance、User/Resource Timing、Observer、IO/RO | 已有观测子集；补统一布局快照、真实加载计时和文档切换语义。 |
| E50 OS/设备集成 | Clipboard、Fullscreen、Screen、Orientation、Geolocation、Notification、Push、Gamepad、USB/HID/Bluetooth、Serial | 按能力逐项核对，涉及 OS 服务先报告依赖；需要用户操作的能力不伪造授予。 |
| E51 安装/共享/文件服务 | Web App Manifest、安装、Share、File System Access、OPFS、StorageManager、Background Sync | 扩展候选；需真实平台后端与生命周期，未接通前保持未支持。 |
| E52 开发与验收 | console/stack、DevTools、DOM/style/布局检查、网络面板、WPT、reftest、组件样例 | 已有多种工具；保留 host/guest/真实站点证据区分，新增组件必须有真实调用入口。 |

## 常见网页组合组件

这些通常由 HTML/CSS/JS 组合出来，引擎不应按组件库或网站名称分支。综合样例将逐批使用它们检验上面的通用能力。

| 组合层 | 应覆盖的网页组件 | 引擎依赖 |
|---|---|---|
| 页面骨架 | header/footer、侧栏、双栏/三栏、dashboard、卡片、面包屑、分页、导航菜单 | flow、flex/grid、sticky、链接与历史 |
| 信息展示 | 表格、树、列表、空状态、徽章、标签、头像、图标、tooltip、统计图 | intrinsic size、表格、SVG/canvas、浮层 |
| 折叠/切换 | tabs、accordion、details、tree expansion、split pane、stepper | DOM mutation、焦点、布局失效、键盘 |
| 数据输入 | 搜索框、自动补全、combobox、日期/颜色选择器、富文本、上传、滑块 | forms、Range、IME、文件选择、默认动作 |
| 弹层 | dialog、drawer、popover、context menu、toast、dropdown、backdrop | top layer、focus/inert、命中、定位、动画 |
| 内容浏览 | 文章、代码块、目录、Markdown、数学公式、无限滚动、虚拟列表、图片墙 | 文本、字体、滚动、IO/RO、DOM 与 frame 时序 |
| 媒体内容 | 图像预览、轮播、视频播放器、音频播放器、字幕、波形、直播 | 图像/媒体管线、Canvas、时钟、流与输入 |
| 应用流程 | 登录/表单校验、购物车、实时消息、路由、离线编辑、拖放排序、多窗口嵌入 | storage、fetch、events、history、worker、iframe |

## 整组推进顺序

1. 本批：原子行内盒、模块映射、任务所有权、分区存储和综合样例，先能编译且有真实消费者。
2. 文档服务：原生 mutation、固定事件路径、live Range、统一表单激活、top-layer；这些共同支撑编辑器与弹层。
3. 页面结构：滚动树、完整 CSS 级联/包含块、文本排版、子文档布局；这些共同支撑复杂内容页。
4. 异步平台：realm-local Fetch/消息/存储、模块 Worker、完整模块调度、Service Worker；这些共同支撑离线与后台逻辑。
5. 丰富媒体和平台服务：增量媒体管线、图像编码、音频图、采集、设备与辅助技术，按真实后端逐项启用。

保留待修事实：此前 pass4 的事件传播 GPF 与一次主文档 TLS 失败尚未定位解决。它们仍在问题清单中，本轮组件扩展不自动意味着修复它们，也不把旧镜像视为本批实现的验证。
