# DOM、脚本运行时与 HTML 交互清单


**第二批更正（2026-09-09，保留下面首次审查的旧结论）：原生 mutation、Live Range、shadow event path、MO/rejection 调度及 native modal 已接入；具体支持边界与 host/guest 证据见[统一接线报告](expansion-status-2026-09-09.md)。下面 snapshot/无持久化/无顶层消费者等原判断只描述第一批时点。**

日期：2026-09-09。范围：`c/apps/browser/js_*.c` 及其 `.inc`、DOM/HTML parser、forms/focus/tabs，必要时追到 browser.c 消费者。工作区基线 HEAD `41d7a74e8a26`，**含多代理尚未提交增量**。本清单是源码审查，不是 WPT 重跑、跨站通过率或完整 API 一致性认证；不引用 CLAUDE.md 的旧通过率作为当前能力。

用户已要求暂停逐点调试，先连贯搭建基础模块。本清单保留现有实现，不建议通过全局空对象、永远成功的 Promise 或无操作方法“补齐浏览器”。第一批组件必须有真实消费者；尚未实现的网页接口仍保持缺失或明确拒绝。

## 1. 结论与边界

已有一套能解析 DOM、执行脚本、修改页面和处理输入的引擎，并非从零开始。关键缺口集中在：

1. DOM、页面 JS、fetch、布局仍主要是单页面全局状态；独立 DOMParser 与 iframe 辅助 context 没有统一所有权模型。
2. 定时器、Promise、网络回调、原生事件、插入脚本各有入口，但缺少统一的文档代际、任务来源和销毁契约。
3. DOM mutation 的通知分散于 C 写入路径和 JS prototype wrappers；Range、observer、自定义元素、原生编辑没有共享的一次性事务记录。
4. 构造器/IDL 方法比实际交互链完整：PointerEvent 存在不代表原生 pointer capture；dialog 状态存在不代表顶层绘制和模态输入隔离；FileList 存在不代表用户能选文件。
5. 导航、脚本装载、历史、滚动恢复存在实用路径，但尚不是统一的文档加载状态机。

“雏形完成”的含义应为：内部对象、状态转换、销毁路径和一条生产消费链已接通，并有能够看红的回归；不是让页面的每个 feature detection 都返回 true。

## 2. 当前模块地图

状态用语：**已有**表示能找到生产实现及调用入口；**部分**表示核心路径存在但有明确结构性边界；**缺失/拒绝**表示此次源码范围未发现实现，或代码明确拒绝。这里的“已有”不等于整个标准已完成。

| 范围 | 已有能力与实际入口 | 部分实现、缺失与影响 |
|---|---|---|
| 页面 JS 生命周期 | `js_page.c:1056` 创建 QuickJS runtime/context；1138 起按依赖顺序安装 DOM、Web API、events、forms、worker 等；1272 关闭 | `js_page.c:576` 与 `js_dom.c:87/93/1098` 仍是全局 page/context/document。不能直接对第二文档重复安装并声称隔离成立。 |
| 定时器与工作推进 | `js_page.c:591` timer/rAF 列表、seq 快照；644/704 的 pending/run_due 同时推进媒体、网络、worker 等；有 watchdog 与 microtask pump | 已有调度器，不能重复再造一条不可见队列。下一步应抽出 owner/epoch 与任务生命周期，先接 timer；每个任务来源的顺序和预算仍需独立约束。 |
| ES module | `js_module.c:145` URL normalizer，编译/依赖发现/预取、`import.meta.url`；411 起处理模块求值 Promise 与 top-level await | bare specifier 在 150 附近明确拒绝；未发现 import-map 注册/匹配表。不是“没有 module 支持”。 |
| HTML parser 与脚本 | `html_tokenizer.c`、`html_tree.c` 是实际 tokenizer/tree builder，DOM mutation API 可用；`browser.c:1460` 起执行收集脚本 | parser 不暂停执行脚本：`html_tree.c:1404` 明示；browser 将 classic 按文档序跑完后再跑 module。async/defer/parser-blocking 不能由这两个 pass 完整表达。 |
| document.write | `js_platform.c:2742` 已有 write/writeln，树级插入并通过已有插入脚本入口执行 | 不是 tokenizer insertion point；先 parse 到 EOF 再执行脚本，不能实现真正的重入解析。旧“write 不存在”注释已经被代码旁的更正撤回。 |
| DOM 节点与接口 | `dom.c/h` 提供 doc、节点、属性、树操作、ID 索引、shadow 数据；`js_dom.c` 与 `js_dom_iface.inc` 安装 Node/Element/Document/HTML 原型及 mutation | wrapper 与部分辅助表依赖 pointer/serial；很多辅助状态读取全局当前文档。独立文档的跨文档移动、销毁与回调取消需要共同 owner。 |
| ID、选择器、集合 | `dom_get_element_by_id_in` 已按当前树序处理重复 ID、隔离 shadow 范围；`js_select.c` 覆盖复杂选择器、live collections、matches/closest | 查询与样式匹配仍是不同实现，需共享测试而不能推断所有 grammar 相同。querySelector 的老三种选择器实现已被 js_select 无条件覆盖，不能据旧 C fallback 判定整个浏览器只支持三种。 |
| CharacterData 与遍历 | `.data/nodeValue/textContent`，append/substring、insert/delete/replace、splitText；`js_platform.c:1922` TreeWalker | `js_characterdata.c:21` 明示部分旧 C API 按 UTF-8 字节、新 JS API 按 UTF-16 code unit，需统一转换；TreeWalker 当前有 nextNode/parentNode/firstChild/nextSibling，未发现完整 NodeIterator/反向遍历表面。 |
| HTML data documents | `js_domparser.c` 是独立 doc/arena wrapper；有 HTML 解析、查询、写入、remove 和保留 detached wrapper 的生命周期 | 与原生 DOM 是两套 wrapper/class；跨体系 import/adopt 在 `js_platform.c:2621` 一带仍拒绝。XML 在 `js_domparser.c:1073` 返回明确 parsererror 文档；`createDocument` 明确拒绝，不能算 XML parser。 |
| Shadow DOM 与 custom elements | `js_dom.c:4030` attachShadow；`js_platform.c` registry、upgrade、连接回调、ElementInternals/state 等 | `dispatch_event` 仍沿 raw parent 走；`composedPath` 在 `js_dom.c:3219` 仍按当前树重算，旧“shadow-less”理由已与现有 shadow 树不符。需补固定传播路径、composed 边界、retarget、closed-tree 可见性；ElementInternals 尚不参加真实表单提交，见 platform:3765。 |
| 原生 DOM events | `js_dom.c:3502` 三阶段 dispatch；listener 顺序、capture/once/passive、防默认、回调异常记录；`js_events.c` 补构造器、EventTarget、handleEvent、signal | `PointerEvent` 构造器已有（events:300），但本次范围未发现原生 pointer capture 状态机/捕获 API；原生浏览器仍主要派发 mouse/key/wheel。不应把合成 PointerEvent 当真实指针交互完成。 |
| MutationObserver | `js_platform.c:1631` observer registry、childList/attributes/characterData records，Promise microtask 批量交付 | 主要通过 JS 方法/属性包装捕获变化，不是所有 C mutation 的共同通知点；observe option 校验/旧值过滤等仍需补齐。原生编辑、parser、独立数据文档不能据其存在推断全部可观察。 |
| IO/RO | `js_platform.c:1399/1566` 实际几何检查与 scroll/resize 重查；IO rootMargin/threshold 已参与计算 | IO 自定义元素 root 仍只存储而未实现根滚动裁剪（1391）；需要依赖统一 layout/scroll snapshot，不能返回看似生效的选项却使用 viewport。 |
| Focus 与原生编辑 | `focus.c` 保存 node+serial、tab 顺序、focus/blur；`forms.c` 真实 value/checked/selection、键盘编辑、contenteditable；beforeinput 可取消 | 内部文本 selection 使用 UTF-8 字节，由 js_forms 转换；跨节点编辑、生命周期、组合输入与脚本 Range 仍需统一事务。没有据此认定完整 IME/composition 链已实现。 |
| 表单与 selectedness | `forms.c` 为 native state；`js_forms.c` 接 submit/reset、selection；`js_semantics.c` options、表格集合、form owner、validity | 本轮已修 select JS/native 双状态与 reset/越界 index。仍需统一普通鼠标、键盘、`.click()`、requestSubmit 的 activation/default action 路径，保证约束验证与 submitter 数据一致；不能仅按成员存在判断。 |
| Validity 与编辑查询 | `js_semantics.c:1578` 起有 ValidityState/checkValidity/reportValidity/custom validity；`js_forms.c:1091` 起有 execCommand 查询与少量 flag 命令 | number/range 是明确计算分支，日期族不能推断齐全；reportValidity 不等于完整原生提示 UI。execCommand 多数编辑动作仍返回 false；designMode 仍缺失。旧“execCommand 完全不存在”的注释已被后段修正。 |
| Selection / Range | `js_forms.c:832` basic Range boundary、collapse、cloneRange/toString，与 native Selection 通道 | Range 仍是 snapshot；insertNode/delete/extract/cloneContents/surround、compareBoundaryPoints、rects、Selection.modify 未实现。富文本编辑器及基于文本范围定位的浮层会卡住。 |
| Dialog / popover / invokers | `js_semantics.c:612/710` 真实状态 Map、show/hide/close、toggle 事件和 command/commandfor；选择器可读状态 | 不等于浏览器 top-layer：本次 focus/browser/paint 范围未找到共享 modal stack 与输入阻断；需要将状态接到顶层绘制、inert、焦点恢复、Escape/light dismiss。 |
| 文件与拖放 | Blob/File/FormData 表面已有；`js_forms.c:554` 为未选择状态提供真正空 FileList | 没有能填充 input.files 的真实选择路径；DataTransfer 文件源/原生拖放未闭环。先提供内部文件选择服务接口和真实用户取消/完成状态，不能伪造文件列表。 |
| Navigation / history / tabs | js_webapi location/history 的同文档与 full navigation 请求；browser 消费导航队列；tabs 保存源码/CSS/资源、URL/title/scroll，新增 session restore intent | 只有一个 live page runtime，切 tab 关闭 JS 再重放，不是保留堆的 BFCache；本次未发现完整 Navigation API。历史 entry、文档 epoch、重定向、取消、scroll restoration 需要统一状态机。 |
| iframe | `js_platform.c` same-origin/about:blank/srcdoc data document；`js_frame.c:353` 为已采用文档创建独立 QuickJS context，支持特定插入 inline script | frame context 仍无真 document/fetch/timers，外部 frame script 明确拒绝；无子文档 layout/paint、无 nested/cross-origin frame。不能据 contentDocument/JSContext 存在认定 iframe 已实现。 |
| Worker | `js_worker.c` 每 worker 独立 JSRuntime，消息、importScripts、timer、终止和错误事件，接 page 调度入口 | 同线程交错执行，不是并行；module worker、transfer list/MessagePort transfer、SAB、nested/SharedWorker 明确拒绝。后续应复用上下文 owner/任务取消，不拷贝页面 singleton。 |
| 存储与 service worker | local/sessionStorage 的 per-origin C store；IDB/CacheStorage 有内存操作；SW registration 检查与明确失败 | localStorage 当前是进程期（webapi:327），IDB 是 JSContext 期（idb:29），二者旧注释所谓相同寿命不成立；无跨重启持久性。SW register 拒绝、controller=null，FetchEvent/respondWith/Clients 未实现；不能把 registration 外壳算 SW。 |

## 3. 第一批应连贯落地的基础组件

以下为模块边界建议，不表示这些新文件已经存在。无需修改内核 ABI。对同一生产文件只安排一个 owner；其他代理通过公共契约与测试接入。

| 模块 | 第一批可交付实现 | 第一批明确不声称完成 | 依赖、验收与建议归属 |
|---|---|---|---|
| D1 文档与 realm owner | 内部 owner 保存 document/runtime/context 身份、epoch、OPEN/CLOSING/CLOSED 状态；统一任务归属与 teardown。先将当前 page 的真实生命周期迁入；多文档字段能表达，但只开启当前 page | 不立即开放多 frame DOM，不重置全局 DOM 安装到子 context，不新增网页全局来宣称支持 | 独立 owner 模块 + js_page 生命周期；open→schedule→close→open 后旧任务不得触达新文档；真实 timer 为首个消费者 |
| D2 有界任务调度与检查点 | 在现有 pending/next_due/run_due 契约内实现 task source、owner epoch、队列上限、一次性取出/取消/释放、回调重入边界；timer/rAF 先接入，网络/worker 逐源迁移 | 不并行执行 JS，不另设无人 pump 的第二条队列，不把全部 task source 无差别 FIFO 化 | 依赖 D1；同一 callback 中 clear/requeue/close 的有界测试，负控分别破坏 epoch 和取消；保留 Promise/microtask checkpoint 行为 |
| D3 DOM mutation 与节点生命期服务 | 一个原生 mutation sink：insert/remove/replace/attr/chardata 的 old/new 信息、before/after 边界、文档 epoch；统一 detached 与 destroy，供 observer/CE/Range 消费。先接实际 setAttribute+insert/remove，再接 native editing | 不用每新增 API 就再包装一次 prototype；不将 detached node 当 destroyed node；不靠裸指针是否非 NULL 判断可用 | dom.c/js_dom owner；以原生编辑和 JS mutation 产生一致 records 为门槛。Range 先接边界跟随与删除调整，再开放编辑方法 |
| D4 事件与输入状态机 | 固定 event path entry（原目标、当前目标、shadow-adjusted target、相关目标可见性），明确 epoch；原生 mouse/key/wheel 与合成 dispatch 共享 default-action/activation 检查。pointer capture 另有实际 pointer id 状态 | 不伪造 trusted event，不让同步递归 dispatch 冒充后续任务；不把 PointerEvent 构造器当硬件 pointer 支持 | 依赖 D1/D3；capture/bubble、detach/reparent、shadow composed/retarget、取消默认动作；事件来源适配由 browser owner 接入 |
| D5 HTML 控件与交互控制器 | value/selectedness/check state 以 forms 为单一来源；统一 activation、validity、submitter、reset；顶层栈表达 dialog/popover，向 focus/paint 提供真实状态；至少完整接通一种 modal 及普通表单 | 文件选择器未接 UI 前不允许成功返回 File；未接 top-layer 前不宣称模态输入隔离；不再建立 JS/native 两份选中态 | forms/focus/js_semantics owner；DOM 与键盘/鼠标/.click 三路结果一致。top-layer 和绘制代理约定只读栈消费接口 |
| D6 导航与脚本加载状态机 | navigation entry 记录请求/最终 URL、类型、history mode、epoch、pending restore、取消原因；分离 parser/classic/defer/async/module-ready 队列；import map 先做有效性、URL规范化、scopes/longest-prefix匹配与模块真实消费者 | 不把 import-map 数据块当脚本；不一口气声称 streaming/reentrant parser；不把成功 fetch 当脚本成功执行 | browser/js_module owner；导航取消/重定向/恢复与旧页面任务隔离；用不同脚本依赖顺序的本地页面验收。parser pause/resume 是下一阶段明确入口 |
| D7 realm-local Web API 服务 | fetch/location/storage/observer 等安装函数从“读当前全局”迁向显式 owner；先建 registry 和每个 realm 的状态容器，实际迁移一类异步 Web API | 不直接开放 iframe fetch/ServiceWorker；没有两个独立状态消费者前不声称多 realm 隔离；无文件后端前不声称持久存储 | 与 D1 契约对齐，由 platform/webapi owner 实现；两个内部测试 owner 的取消和状态不能串号；API输出必须来自真实服务 |

可并行组织：D1/D2 一位 owner；D3/D4 一位 owner；D5 一位 owner；D6 一位 owner；D7 与 D1 先约定 owner id/epoch，避免两份 realm registry。受代理数量限制可分轮，不能让多人同时改 js_dom.c、js_page.c 或 browser.c。

第一轮最小完整链：**页面 owner → 有界任务 → 真实 callback → DOM mutation → 统一失效/观察记录 → 帧提交；导航关闭 owner 后所有旧任务失效。** 这是所有组件可以先写齐的共同骨架。新增能力的公开安装点只在对应链已经存在时启用。

## 4. 后续接续，而非第一批塞入假接口

- D1/D3/D7 具备两个独立真实 DOM 后，才推进同源 iframe 的完整 Window/Document；随后对接布局代理的子文档 viewport/paint，再讨论跨源 WindowProxy 与嵌套。
- D3 的边界跟随先落实后，再补完整 live Range、富文本编辑与 Range 几何；不能继续用快照 Range 暴露会改变内容的方法。
- D4 与焦点/顶层栈贯通后，再完善原生 pointer capture、composition、拖放/文件选择、弹窗焦点恢复。
- D6 的 parser 调度入口稳定后，再把 document.write 从树级模拟迁到 tokenizer insertion point，并完善 async/defer/模块依赖加载顺序。
- D7 完成真正 realm-local fetch 与任务后，再做 module worker/transfer；service worker 的执行与两条资源装载路径的拦截必须一起接通，不能只让 register() resolve。
- 存储先定义真实 durability/partition key，再做后端；不要因旧注释引用历史 VFS 缺口，就把已发生变化的 OS 能力当作当前不能持久化的证明。

## 5. 验证方式与本次证据

已有可复用入口：`test-dom-id`、`test-dom-iface`、`test-selectors`、`test-domparser-remove`、`test-events`、`test-forms`、`test-semantics`、`test-select-state`、`test-page-timers`、`test-page-lifecycle`、`test-frame`、`test-worker`、`test-module`、`test-session-restore`。它们覆盖不同 link surface，不能互相替代；本次未重跑这些 gate。

每个新内部模块都需要：

1. 生产消费者与测试共享实现/来源列表；host probe 仅作为机制证据。
2. 生命周期、失败、取消、越界/容量边界有确定返回；不以 crash-only 的失败充当正确负控。
3. 负控是正向 gate 的 prerequisite，实际观察断言红；新增 fragment 由 Makefile owner 接线并跑 test-mk-wired。
4. 集成构建必须重建 ring-3 的 disk.img；最后用普通本地 HTML fixture 做 guest 操作链，再用少量真实站点检验通用能力。
5. 分开记录“编译”“单元机制”“guest fixture”“真实网页”；禁止以 API 名称数、模块文件数或旧 WPT 百分比证明网页完成。

pass4 的已知 GPF 证据保留：`/tmp/logitos-ordinary-pass4-0909-evidence/python-flow/serial.log` 3960 附近；精确 ELF `.../build-ordinary-pass4/browser.elf` 的 `0x45118bd6` 位于 dispatch_event 构造祖先路径、读取 node serial 的阶段。尚未确定坏指针来源，不能据此声称 D1/D2 必然修复该故障。依用户要求，此处只登记证据，不继续 core 调查。

## 6. 本次读取的关键文件指纹

下列是写清单时工作树字节，不是 pass4 镜像字节；并行开发后会变化。源码行号同理，以函数名和这些指纹定位审查版本。

- `js_page.c` — `5899686772cba24e193f229a98ef29eb8ea0060834923f38d03942b7f7a876f0`
- `js_dom.c` — `6d9986b3a014f07acd1b5462f0f876335f994ebc6fc885bad3bc0e3f01f03416`
- `js_domparser.c` — `a0740b706acd1a131a4c59248ee221e9c3703004babb274500e41c9d0202c8c7`
- `js_platform.c` — `7dbafde52076de8b1a9f02bc239f64baa996941d735e6d736339e642ed1e4347`
- `js_forms.c` — `49bf448d8bfbba206d0963bb27e7746db5855c58ad7e19c6a82a78985ef5ffb4`
- `js_semantics.c` — `c72137e0ff241dea2ce1a89729fd773d170845bfc82ecdbcce328d0d72bde2cb`
- `js_module.c` — `83411be4321e52a9b35d60daf04e88bed0afac58bae587744f0d2bc20f82d83e`
- `js_frame.c` — `68780f7667959fc0cbaabc668daae229833cabc6d4eb7eb944300f60b22371c7`
- `js_worker.c` — `cacbef5e9765b40d28d6cacbab9970c3e01a6a38a47bab847fe7c18a6fe3e57e`
- `js_webapi.c` — `ec6fb9a07c13f11ea627035a9e528ef7d3d3e88707a811e3d21da0421f030ede`
- `forms.c` — `ee1df9001fb6f4da4c6ec4e41b8fbb6f85682f023707e51d8c856d3ed677cd31`
- `focus.c` — `b8539039c8b31191c579f4858aecfa102aa124fbc412970b3c5f00f5eeae32c1`
- `tabs.c` — `ef34e0f54ab029d2b7cff0835cb18acf9e64843f2015ffe452530a24ef10b6d1`
