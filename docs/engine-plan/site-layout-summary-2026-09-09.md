# 通用布局修复与 guest 可视验收，2026-09-09

最新更正（21:13）：使用现有 final-relink disk 独立重跑 Apple，已亲自检查 `final-sites/apple-chrome/apple.png`，导航白条全部消失，水平导航与 hero 文案保持。下面保留 20:48 集成轮曾出现白条及当时待复验的原记录；该待办现在已有 guest 结果，详见末节。

当前真实 guest 已证明 Bilibili 导航网格、卡片比例、统计条位置与标题可见性改善；缩略图仍是占位，不能称网站完整可用。Apple 导航从纵向堆叠恢复横排，但 integrated-sites 截图暴露新增白色按钮条。控件代理已提交通用样式修正，尚待 root 最新 disk / PNG 复验。本报告保留这个回归，不把 `PAINTED` 或无 JS 异常当成视觉通过。

## 修复与可被观察的负控

以下是 layout 代理的通用生产改动；网站只提供样本，没有 hostname、URL、框架分支，没有 UA 伪装，也没有播放视频。计数为 host gate 的断言数，包含几何查询存在性断言；不等于 WPT 数或完整网站通过数。

| 通用缺陷 | 生产边界 | 正控 | 已观察负控 |
|---|---|---:|---|
| definite 百分比高度 | 普通流按最近块容器的确定内容高度解析；遇 auto 停止；fixed 使用 viewport；替换图片同样消费 | 41 | 最近回归日志 17 项失败；早期版本为 18，后续 used-height 路径改变其中一项，负控仍捕捉核心缺陷 |
| `display:contents` | LibCSS 真正解析/级联，盒树扁平化，保留 DOM、伪元素和继承 | 32 | 14 项失败 |
| contents 的 CSSOM 几何 | 不借子树 ink union；矩形为零、片段列表为空；滚动与动态 display 切换 | 9 | 5 项失败；旧值为 160×40 |
| 零透明度祖先绘制 | 真实 paint loop 精确剔除 opacity=0 组后代；不实现半透明组，不以透明度剥夺后代命中 | 12 | 4 项失败；0→1→0 动态恢复被覆盖 |
| Grid rem/em | 显式及隐式轨道分别使用根字体和局部字体 | 10 | 3 项失败 |
| Grid item 百分比宽度 | intrinsic 阶段保持 indefinite；轨道确定后按完整 grid area 解析 calc、盒模型、min/max | 18 | 6 项失败；320px 网格的子项旧 322px、新 102px |
| auto 高度定位容器 | 先测普通流，回滚试排产物，再以本轮 used padding height 排绝对定位内容；普通流 % 不借用这个值 | 39 | 12 项失败；picture/img 320→180 高，统计条 y180→142 |

对应 Make targets：`test-percentage-height`、`test-display-contents`（依赖 CSSOM companion）、`test-opacity-group`、`test-grid-font-units`、`test-grid-percentage-item`、`test-absolute-auto-height`。每个负控都是正控的 prerequisite。两阶段试排压制嵌套试排，避免指数级递归；它保留正常最终绘制顺序及 box/item 子树范围，避免延迟绘制队列破坏这些消费者。

最近整组回归 `auto-height-regressions.log` 与 `auto-height-additional.log` 的 make 均 exit 0：grid 318、CSSOM 149、percentage 41、contents 32+9、opacity 12、form-control-container PASS、intrinsic 65、flex 176、max-height 18、generated content 54、grid font units 10、grid percentage item 18。`test-mk-wired` 为 216 fragments / 215 reachable / 1 declared。日志里的负控 FAIL 是预期且由 prerequisite 校验；例如 generated content 正控是 54/0。所有产物都位于既有 `build/`，本代理私有新产物在 `build/site-general/layout/`。

## Bilibili：三轮实际截图

证据在 `build/site-general/{baseline,after,integrated-sites}/bilibili/` 各自的 `bilibili.png`、`result.json` 与 serial。截图均实际打开检查，集成轮 result 完成于 20:48:34；实时内容会更换，不能逐字比较不同卡片标题。

| 观察 | baseline / after | integrated-sites |
|---|---|---|
| 导航 | 左网格每列近整个容器宽；大量分类横溢 | 九列紧凑显示，右侧分类也在窗口内 |
| 可核对的文字坐标 | `番剧/国创/综艺` x=476/1152/1828，步进 676 | x=176/251/326，步进 75 |
| 卡片 | 灰色图片区域向下覆盖标题区域，统计条位于卡片顶部 | 两个首屏卡片约 240×135，统计条贴底 |
| 统计文字坐标 | 首行统计 text y=330 | y=427；对应截图中条带下移至图片底部 |
| 标题 / 作者 | result 记录有标题文本，但截图被灰区盖住，不能从存在 text run 推断可读 | 截图中标题与作者/日期实际可读 |
| 缩略图 / 横幅 | 占位 | 仍占位，图片解码/展示未通过 |
| 页面错误 | 15 条 page-reported errors | 仍为 15 条，跨源 frame 拒绝和 firstChild null 尚在 |

Bilibili 的 baseline/after 约 55/57 text runs，集成轮 51 runs。下降不等于退步：截图中需要的标题变得可见，而之前记录的部分统计属于错误高框/叠盖布局。`requests=13`、`fetch_failed=[]` 或没有 `[img]` 日志均不足以证明图片已请求或解码；runtime 代理正在添加默认关闭、按需触发的 `about:images` 诊断。这里仅验收占位几何，不推断空图原因。

## Apple：修复与新增回归分别保留

证据在 `build/site-general/{baseline,after,integrated-sites}/apple/`。baseline 导航菜单纵向穿过 hero 文字；after 已恢复单行水平导航。集成轮保留水平布局、hero 文案和 calendar 控件，但导航各菜单后出现约 20×44 的白色按钮条，after 无此条。

集成轮 `result.json` 的 `PAINTED` 标签与清空的 JS exception 数组说明运行路径前进，不能掩盖白条。字体端点 404 仍在，hero 仍是纯色背景而非完整媒体内容。DOMMatrix/runtime 改动由 runtime 代理负责，本报告不把它归为 layout 代理实现。

root 与 caret 已定位白条为另一路通用控件消费缺陷：computed 透明背景和零 border 被当成“未设置”，重新补了 native 默认 chrome。caret 报告 `test-control-chrome` 20 正控通过、旧逻辑 12 项失败，并恢复 UA 样式参与级联；这个报告是代理交接证据，尚不能替代最终 Apple 新 PNG。生产代码由该代理冻结后交 root 重建。

## 证据边界与下一次复验

- `display:contents` 的旧 guest consumer 曾打印 `item offsets=80,160 wrapper=160x40`：盒树布局已经正确，但 JS 几何接口错误。这个原有错误保留在 `display-contents-2026-09-09.md` 旁，不能用早期 32 个 layout-only 断言宣称 JS 已验证。
- Zero-opacity dirtyrect 独立只读探针覆盖高度 0 的父容器、位于 y200 的 100×50 子元素。稳定透明第二帧 dirty=0；0→1 和 1→0 均产生 `0,200,100,50` 脏区。它排除了该缩减的祖先透明度导致子元素不进入 dirtyrect 的猜测，未证明 z.ai 空白的全部原因。
- 仍未扩充 flex/grid 后续 stretch 所产生的 auto-height definiteness、transform 建立 fixed containing block、完整静态定位算法或半透明组复合。此次没有借站点样式掩盖这些边界。
- 本轮新增 guest fixtures 是 `grid-percentage-item.html`、`absolute-auto-height.html`，位于 `tests/fixtures/engine-expansion/`。正控 host 几何与真实 Bilibili 布局改善是两层证据。已逐行读取 integrated-guest/serial.txt：664 行 DISPLAY-CONTENTS PASS（wrapper=0×0）、745 行 GRID-FONT-UNITS PASS（40/64/196）、824 行 GRID-PERCENTAGE-ITEM PASS（102/110/220）、898 行 ABSOLUTE-AUTO-HEIGHT PASS（180/180/180/142/188）。这些为本轮实际 guest consumer 验证；最新控件修正和图片诊断仍由 root 下一轮统一重建运行。
- 本代理保持生产冻结，仅更新此报告。收到 root 最终新 PNG 后继续对照 Apple 白条、Bilibili 实际图片和现有几何，保留旧结果旁的更正。


## 最新 Apple control-chrome 实机复验，21:13

本代理未 build、未修改任何生产或网络文件。使用现有 `build/logit.iso`
和 `build/disk.img` 的私有副本进行一个独立 QEMU boot，实际参数为
1 GiB、1280×800、snapshot。完成后删去两份临时镜像副本，保留证据于
`build/site-general/final-sites/apple-chrome/`：

- `apple.png`：亲自打开与 integrated-sites/after 对比；导航白色竖条全部消失，菜单仍单行水平排列，hero 标题、说明和 calendar 控件保留。
- `result.json` / `apple.serial.txt` / `driver.log`：driver exit 0；PAINTED；JS、timer、module、console error 数组均为空，无 app fault / panic；字体 `/wss/fonts?...` 404 仍在。
- `artifact-manifest.json`：disk SHA256 `c2c3bd40f34da84a48086a737d3259ab8d850c7d2c8513d2a75b60e9bf405d1f`，ISO SHA256 `157511cacfa5ddf4e9aeb46934c2f1ad13a7caed292d37440a94b79fa183f48d`。

这个结果只关闭“修正后的 Apple 导航是否还有白条”的视觉回归。主区域仍是纯色背景，完整媒体内容、全页滚动和交互没有在此轮验证，不能据 PAINTED 宣称 Apple 全站兼容。该次 disk 对应 root 指定的外部 TEMP-B 网络诊断状态，源 `browser_rt.c` 的 TEMP-B 行已确认存在；本轮 requests=36、dials=31、reused=0 不与之前 keep-alive 构建作布局性能比较。Bilibili 最新图像诊断由 root/runtime 独立处理，不从 Apple 结果推断。

## 后续 Bing 定位缺陷：host 已闭环，等待统一 guest

原生首页点击暴露另一通用缺陷：`top:20%` 在 computed-style 桥接被丢弃，fixed 的双边 inset + `margin:auto` 未求解，搜索框与高 z-index 导航重叠。现已保留百分比单位并按真实 containing block 求解，补齐定位盒自动外边距；绘制、原生命中、CSSOM 与 selection 共享 fixed 视口投影。18 项实际 painter/hit/CSSOM host 检查及 70 项布局检查通过；百分比、margin、projection 负控分别观察到 18、9、6 项失败。详情与规范、日志、边界见 [positioned-insets-2026-09-09.md](positioned-insets-2026-09-09.md)。

此前“本代理保持生产冻结”的记录对应 Apple 截图轮；之后 root 明确授权此通用定位修复。当前新增 fixture 为 `positioned-insets.html`，最终 disk 与 Bing 自然坐标输入由 root 统一验证；尚不把 host 成功写成新 guest 已通过。inset calc、transform fixed containing block 和完整 static-position 算法仍未声称支持。
