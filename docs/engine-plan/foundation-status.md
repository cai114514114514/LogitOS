# 第一批引擎基础组件：交付与验证

日期：2026-09-09。对应用户“先铺组件雏形，再逐项调试”和“重新派出，必须实现”。

## 已接入的实现

- **页面任务**：`page_runtime.c/h` 为当前页面保存 runtime/context/document 身份、代次、关闭状态与有界任务队列；`js_page.c` 的 setTimeout/setInterval/rAF 已实际使用。共享容量 4096，任务新增/重排遵守下一轮边界，关闭先失效代次再释放 JS 引用。尚未迁移 Fetch/Worker 队列，也不等同多个活动文档。
- **模块与脚本**：`js_importmap.inc` 实现 imports/scopes、最长匹配、null 阻断、合并和已解析记录；`js_module.c` 的预取、静态链接、动态 import 共用解析；`browser.c` 按已有脚本执行顺序注册 inline import maps。只读 JSON 自有字段；行内脚本用显式 referrer 映射保留真实 document URL。当前 HTTP(S) transport URL 子集，integrity map 明确拒绝，streaming HTML parser 调度尚未重构。
- **存储**：`storage_backend.c/h` 被真实 Web Storage 绑定消费；local 按 origin，session 按 origin+tab，关闭 tab 后才释放对应 session。字符串带长度，失败更新原子化，配额异常使用真实 DOMException。当前是内存后端；没有跨进程/磁盘持久性，也没有新实现 storage 事件。
- **共享 URL 算法**：从 `browser_rt.c` 提取原有 `bfetch_url.inc`，真实运输层与 loader 的内存网络装置共同使用。旧装置会留下 `/./lib.js`，导致本来可解析的模块在测试中伪造 404；修复的是装置漂移，未将此算作新网络协议支持。
- **原子行内盒与行高**：inline-block、inline-flex、inline-grid 共享外层排版协议，各自内部仍走既有块/flex/grid 算法；grid 内在尺寸调用同一个轨道求解器，试排后撤销临时绘制记录。百分比/em 行高在继承前计算，显式零与 normal 分开。
- **综合组件页面**：[样例入口](../../tests/fixtures/engine-components/index.html)和[运行说明](../../tests/fixtures/engine-components/README.md)。模块真实更新页面，按钮驱动 DOM/任务/存储/Fetch，Canvas 绘制后回读像素。dialog、RTL、元素滚动等保留为可见待完善样例，未标为通过。

运行时原代理两次被工具风险筛查中止；用户要求重派后，新的 `page_runtime_integration` 代理完成了实际迁移。此前“仅保留设计、运行时待实现”的状态已被本批实现取代。

## 机制与集成检查

| 检查 | 本轮结果 | 证明边界 |
|---|---|---|
| `test-page-runtime` | 45 checks / 0 failures | 真实 js_page 定时任务的顺序、接收者、微任务、取消、容量、关闭/重开。 |
| 页面代次负控 | 明确打印 `FAIL old page token rejected after reopen` 和 `FAIL old producer cannot enqueue into next page` | 忽略代次确实使功能断言失败；是正门禁前置依赖。 |
| 队列容量负控 | 明确打印 `FAIL queue capacity enforced` | 取消容量上限确实使功能断言失败；是正门禁前置依赖。 |
| 既有 timers 正向 | 26 checks / 0 failures | 与之前 timer 接收者/回调行为兼容；这一行不是原 ASan 负控的结果。 |
| `test-importmap` | PASS | real browser_load、HTML parser、QuickJS、module loader 与 DOM；网络/窗口为 host fixture。映射后的模块必须实际挂载 DOM。 |
| importmap 负控 | `FAIL: mapped graph executes through browser loader`，挂载断言亦红 | 禁用 map 解析后同一网页不再执行正确模块；正门禁依赖它。 |
| `test-storage-backend` | 37 checks / 0 failures | 分区、跨运行时生命周期、NUL、配额和分配失败原子性。 |
| Storage 平台消费者 | 3 checks / 0 failures | 属性 Proxy 访问真实后端，配额使用 DOMException。 |
| Storage 两个负控 | 37/3 与 3/1 | 移除 tab 分区以及换回错误异常类，都观察到对应断言红。 |
| `test-loader` | PASS | 真实加载器已有回归；其故意缺少可选平台模块的 fixture 异常不是新增网页通过证明。 |
| `test-cssom-abi` | PASS，item 256/node 256/image 16 | writer/readers 布局一致；错误编译参数负控产生 item 248。 |
| `test-atomic-inline` | 45 checks / 0 failures，负控 14 failures | 同行、换行、内层布局、自动/嵌套 grid、blockification、反复布局；负控是正门禁依赖。 |
| `test-line-height` | 36 checks / 0 failures，负控 16 failures | 百分比/em 继承、unitless ratio、显式零；CSSOM 保留现有整数 px 序列化边界。 |
| 布局既有回归 | inline-flex 36、intrinsic 65、layout 51、margin 120、flex 176、grid 四组 142/84/40/52、CSSOM 149 全部通过 | 具体日志在 `/tmp/logitos-foundation-layout-0909/`。 |
| `test-mk-wired` | 170 fragments，169 可达，1 声明例外 | 新片段全部接入；例外为原有自递归 wrapper。 |
| 综合样例静态检查 | HTML 资源链接、两份 JSON、三个 JS 文件语法有效 | 仅资产/语法检查，不是 LogitOS 内的行为通过。 |

主线日志：`/tmp/logitos-foundation-gates-0909.log`、`/tmp/logitos-foundation-integration-0909.log`；独立运行时日志在 `build-page-runtime-local/`，存储冻结记录在 `/tmp/logitos-storage-backend-0909/`。负控失败日志与正例成功日志应分别读取，不能把一次 grep 到的 FAIL 当成整个 gate 失败。

## 集成状态

独立检出 `/tmp/logitos-engine-foundation-0909`，`BUILD=build-engine-foundation`。`make -j6 all build-engine-foundation/disk.img` 最终退出 0，实际重新链接/打包浏览器并生成磁盘镜像。`browser.aex` SHA256：`efea2fdff655961808a509ce5246e3600a12a689a4335916f931d733291007af`。完整 ISO/disk/ELF 哈希、源码 manifest 与日志位于 `/tmp/logitos-engine-foundation-0909-evidence/`。已核对本批产品源与构建快照一致。

## 综合样例 guest 验证

同一个最终镜像，QEMU x86_64 TCG、1 GiB、4 CPU、virtio-gpu、snapshot-only 磁盘；仅通过鼠标/键盘操作普通页面，未注入测试脚本。

- 首页首次 6 个请求，import map 成功注册，静态依赖 `modules/ui.js` 实际装载并把“模块尚未执行”改为“模块已执行”。[实机首页](evidence/workbench.png)。
- 原子容器与前后文字出现在同一段排版中；完整基线/vertical-align 尚未验收，不把这张截图当逐像素一致。
- Canvas 显示两色块，真实 getImageData 回读 `20,106,114,255`，SVG 图像也已解码显示。
- 实际点击按钮后，页面记录依次显示 `timer fired`、`microtask fired`、`rAF fired`。[任务与实际操作记录](evidence/task-chain.png)。
- 点击 Fetch 后 JSON 响应正文出现在页面；点击动态导入后 `modules/extra.js` 实际装载并显示导出的文字。
- 第一个 tab 写入/读取 `local=my-value; session=my-value`；第二个同源 tab 读取 `local=my-value; session=null`。[标签页存储隔离](evidence/storage-isolation.png)。
- 本次导航后的日志未出现页面 JS/module exception、prelude failure 或应用 fault。只表示这条样例链，没有覆盖任意网页。

原始截图、`actions.jsonl`、`serial.log`、`manual-review.json` 在 `components-live/`。前两个驱动尝试分别因 stdin EOF 结束、复用旧目录导致启动定位失败，保留为装置失败记录，未计入上述验收。

**尚未完成的验收**：本批 guest 负控/逐像素前后对照、所有表单/弹层/RTL/元素滚动、跨进程持久性，以及旧 Python 事件 GPF/TLS 的真实站点复测。持续目标仍未完成；持久存储、多 realm、完整 iframe/ServiceWorker 等仍按总表推进。
