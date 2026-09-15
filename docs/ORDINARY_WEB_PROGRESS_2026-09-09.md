# 普通网页可用性：实施与验收记录

目标保持为普通真实网页能够加载、阅读和交互。Example Domain 单页、主机
unit gate、`PAINTED` 标签均不能独自证明目标完成。本轮不改系统 ABI。

## 验收范围

至少覆盖简单静态页、带外部样式及脚本的文档页、中文资讯页。真实 guest 中
检查正文与图片、完整样式依赖、滚动、链接、输入/提交、返回与重新加载；
搜索页还必须得到实际结果。网站拒绝或网络失败保留真实原因，不伪装身份，
不按 hostname 或框架名称改变实现。

## 可重复的源码与镜像边界

- 初始代码：`41d7a74e8a26d59bd13f368506a52da930062b3c`。
- 基线 worktree：`/tmp/logitos-ordinary-baseline-0909`，fresh ISO/disk 构建
  exit 0；证据 `/tmp/logitos-ordinary-baseline-0909-evidence/`。
- 第一批快照：`/tmp/logitos-ordinary-pass1-0909`，包含 margin、DOM ID、
  timer 修复；精确覆盖文件/hash 见同名 `-evidence/source-manifest.json`。
  `make -j4 BUILD=build-ordinary-pass1 build-ordinary-pass1/logit.iso
  build-ordinary-pass1/disk.img` exit 0。并行中的后续修复不进入这份快照。
- Guest 用 1 GiB、TCG 4 vCPU、1280×800 virtio-gpu、snapshot disk。
  性能结论不使用主机 wall clock。截图、串口和测试结果均是各次运行的证据，
  不能据此推断之后仍在变化的主工作区已通过。

## 已确认的变化

| 通用问题 | 修复及当前证据 | 尚待验证 |
|---|---|---|
| margin 的 `%` 丢单位、负值与 auto 混用 | 独立 auto 标记、保留百分比精度，在 containing block 上解析；120 项通过，两个旧行为负控分别失败 23/9 项；既有 flex 176、grid 318、layout-box 51 通过 | 更多实页尺寸/滚动组合 |
| body 未分配 auto 横向余量 | 第一批 guest 的 Example 正文从屏幕 x=138 移到 x=363，页内居中；新旧截图均已人工查看，正文仍完整可见 | 点击及返回流程 |
| DOM ID 查询不遵循当前树序、跨入 shadow tree | 29 项通过，旧行为负控失败 7 项；保留 detached node 的 wrapper/listener，真正 destroy 后才失效 | 完整页面中的组件行为 |
| one-shot timer 释放后又读取 raf 字段 | 修前 ASan 明确 heap-use-after-free，退出 86；缓存 metadata 后运行时 gate 26/26，通过恢复旧读的负控 | guest 交互与长期稳定性 |
| strict timer callback 的 this 错误 | 普通 timer 显式传 page global；rAF 保留 Web IDL 默认 undefined；严格/绑定/箭头回调、异常、取消及 microtask 顺序通过 | 更广的网页行为 |
| H2 首批正文早于响应头交给 fetch | 真实 H2 适配器 fixture 的 HEADERS+8192 B DATA 修前 26 项中 5 失败，修后 26/26，负控失败；h2mux 98/98 | 新镜像实站复测，旧 QQ 日志没有请求 URL，不能把候选根因当成实站已修 |

## 真实网页发现及进行中的修复

- **Python 文档**：初始 guest 确认 `https://docs.python.org/3/`，18 requests，
  页面导航纵排占据首屏，语言/版本显示为 Greek/dev。公开 `classic.css`
  通过 `@import` 引入的 `basic.css` 才有隐藏标题、横排导航规则；旧加载器
  没有载入这一依赖。正在实现通用 import 加载，及 single-select 的 JS/native
  状态统一。主机拿到的 CSS 是诊断样本，未声称与 guest 响应逐字节相同。
  第一批实际交互已通过点击输入 `json`、Enter 提交、PageDown、点击结果到
  `/3/library/json.html`、Alt+Left 回搜索页。搜索找到 71 页，但摘要触发
  DOMParser 独立 wrapper 的 `remove()` 缺失，故还不能判完整可用。完整输入
  动作、截图及串口在 `pass1-0909-evidence/python-flow/`，不是脚本注入的成功。
- **QQ**：初始 guest 新闻起点 x=-156，文字被裁切；串口有两次 fetch 失败
  （connection failed、response exceeds limit）和 6 条页面 error。
  尚未达到可用状态。新日志增加 URL、失败边界和已收字节等，避免盲调大小上限。
  第二批 guest：`response exceeds limit` 本次为 0；唯一 fetch 失败定位到
  `http://127.0.0.1:11601/check` 的本机服务探测。新闻 x=0，仍有导航 x=-18
  裁切、页头低对比及右栏溢出；5 条页面 error，最终 `ERRORS`。单次动态页
  比较不等于全部问题已修，原始结果在 `/tmp/logitos-ordinary-pass2-net-0909-evidence/`。
- **测量装置修正**：旧 `qmp_site.py` 给 QQ `PAINTED/no JS exceptions`，却漏掉
  `[webapi] fetch:` 与 `[error]`。新解析器分别保留 fetch 错误与页面报告错误，
  这些诊断会使新结果进入 `ERRORS`；没有改写旧 JSON。`test-sites-errors`
  修前两项失败，修后全部通过，抹去诊断的负控仍明确失败。

## 第三批镜像及后续集成（2026-09-09）

第三批快照 `/tmp/logitos-ordinary-pass3-0909` 含 import、初版 select、
DOMParser、H2 和前述修复，fresh ISO/disk exit 0。实际文档页加载了
14,685 字节的 `basic.css`；语言/版本显示 English / 3.14.7，Navigation
多余标题消失。它仍有导航换行和搜索控件宽度错误，不能据此认定布局完成。

第一次连续导航失败证据保留在同名 evidence 的 `python-flow/`：旧脚本
slice 结束后未解除 watchdog，下一页的 Web API 初始化被已过期计时器中断，
导致 `fetch` 消失；同次 `language_data.js` 还遇到 TLS ServerHello 前重置。
该日志不能用于判断 DOMParser 修复无效。

独立首次打开搜索页的 `search-first-flow/` 已通过实际截图核实：搜索完成、
找到 71 页，JSON 和 pickle 摘要有可见正文；点击第二项进入
`/3/library/json.html`，正文可读。该镜像尚不包含下列最新修复，且仍出现
一次 analytics 服务 TLS 失败。它证明的是搜索摘要和结果导航这一条路径。

后续主工作区修复，尚待统一新镜像验收：

- watchdog 在 slice end 和 close 解除旧 deadline；真实安装器及可控时钟
  gate 修前 3 失败、修后 0 失败，同时实际长循环仍能被中断。
- CSS 扩展选择器复用 LibCSS 完整匹配；33/33、负控 20 失败，ASan/UBSan
  普通样本无诊断。本地 QQ 样本错误渐变和伪元素 transform 已消失，尚非 guest。
- 链接折叠空白加入无绘制的命中区域，遮挡不再穿透到底层链接；23 项通过。
- float 内在宽度正确累加 inline run 并采用控件尺寸；65/65、负控 19 失败。
- inline-flex 保留外部 inline 语义；首轮 36/36，后续回归仍在进行。
- 水平滚动正在集成真实绘制、命中、CSSOM、可见滚动条及 session；绘制 gate
  14/14，恢复旧绘制的负控失败 5 项。未把绘制测试当作完整输入流程通过。

## 第四批统一镜像：已完成的 guest 边界

`/tmp/logitos-ordinary-pass4-0909` 的 74 路径覆盖快照及源哈希已核对，
fresh ISO/disk 构建 exit 0。证据在同名 `-evidence/`，产品代码与主工作区
一致；后加入的 runtime 测试和日志解析增强不改变该镜像产品代码。

`fixture-flow/` 使用普通本地 HTTP 页面、真实键盘和鼠标，已逐张核实：

- 初始右侧内容不可见；页面自己的 `scrollTo()` 把它移入视口，画面及
  `scrollX/pageXOffset` 同为 674。两个有界滚动事件均 depth=1。
- Shift+滚轮回到 0，普通 Right 到 40，拖动可见滚动条回到 674；没有误导航。
- 横滚后点击输入开头，X 出现在字符串开头；下拉框可选 Second。
- 切换标签回到 674；关闭应用、重新启动并按现有提示 Enter 加载后仍恢复 674。
  未单独验证纵向重启恢复，不能把本项扩大为两轴全覆盖。
- 点击 `json next` 的实际空格（screen 964,458）进入 target.html，画面明确显示
  HORIZONTAL LINK NAVIGATION REACHED。clientX=810、pageX=1484 与偏移一致。
- 上一页完成于 guest 352470ms，下次导航为 472740ms，间隔 120270ms 超过
  45000ms watchdog 窗口；新页仍输出 FETCH_READY=function，无旧 deadline 中断。

对应新主机 gate：绘制 14/14，旧绘制/旧 extent 负控分别失败 5/4 项；
CSSOM 同步 23 项及旧 CSSOM 149 项通过；session 25/25，负控 7 失败。
真实 browser.c 事件循环 gate 51 polls 完成，旧同步派发负控准确显示 depth=2；
正例 depth=1、DOMRect/绘制一致，真实鼠标队列点击后目标正文进入 painter。
`test-mk-wired` 为 165 fragments，164 可达、1 合理独立声明；ABI 检查通过。

Python 和 QQ 在第四批镜像中的实页复测仍在进行；上述是隔离的输入/状态
验收，尚不能替代两个真实页面的结果。

**后续更正与工作方式调整（保留原句）：** Python 第四批首页与一次重试后的
搜索结果已显示，但随后浏览器在 DOM 事件路径中发生 GPF，未完成结果链接导航；
一次搜索主文档请求在 TLS ServerHello 前失败，重载后成功，原因尚未确认。
QQ 第四批复测未完成。原先“仍在进行”现改为保留证据并暂停逐页调试，不能
把这段过程记为普通网页端到端通过。

用户随后要求先广泛铺出引擎组件，再慢慢调试。新工作总目录为
[引擎整体雏形与组件总表](engine-plan/README.md)，包含 52 组能力与常见页面
组合组件。首批已开始实际接入 import maps、页面任务所有权、分区 Web Storage、
原子行内盒；构建与验证边界单独记录在
[foundation-status](engine-plan/foundation-status.md)。这些新增代码不自动修复
上述 GPF 或 TLS 失败，也不将候选组件清单包装成已实现数量。

## 现有失败，未伪装为绿色

- `test-js-dom-asan` 完整运行通过，但 QuickJS 的有符号左移 UBSan 警告仍在。
- `test-stream` 的 62 项中 3 项 AbortError 失败及 `test-cookie-cors` 的 54 项
  中 1 项 opaque Response 失败，在隔离原始快照上逐项相同。此次 H2 修复没有
  引入它们，但它们仍是未解决的检查结果。

目标仍进行中。完成前必须以最终构建的 guest 页面和真实交互逐项验收，不能
把本文的局部修复、旧快照或主机测试合并成一个未经运行的“全部通过”。
