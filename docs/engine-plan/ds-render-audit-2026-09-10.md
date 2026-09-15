# DS 登录后渲染审计 — 2026-09-10

2026-09-11 修复更新：本清单中的 Flex 高度传递、嵌套层叠顺序和八类 SVG/按钮缺陷已实施修复，并完成本地正负对照与独立真实 QEMU 检查。登录后的真实 DS 页面现已能看到主区欢迎文字、输入框、侧栏标题和 SVG 图标。10:41:16 UTC，实际 QEMU 页面已显示第一条真实回答 “Hi there! How can I help you today?”，对应 HTTP 200 event-stream。完整边界见 [请求诊断报告](ds-request-diagnosis-2026-09-11.md)；首次回答成功不代表全部渲染状态已经验收。

这是一份已观察问题清单，不是所有页面状态均已遍历的声明。以下审计现象保留首次排查时的证据；首次审计未修改算法，用户随后授权修复。Cookie、浏览器上下文和 iframe 的实现工作另有验证记录。

## 已落实的修复与证据

| 修复 | 验证结果 | 记录 |
|---|---|---|
| Flex 拉伸高度在子树布局前生效；按钮使用 CSS padding/border | Host 高度 75 项、按钮 65 项通过；旧实现分别有 18、15 项失败。Guest 侧栏恢复为 562px，真实滚轮可达底部。 | [Flex 与按钮修复](ds-flex-used-height-fix-2026-09-10.md) |
| 按嵌套层叠上下文绘制和命中 | Host 46 项及 sanitizer 通过；旧平面排序 24 项失败。Guest 文字从 0 恢复到 441 像素，前后层点击对象正确。 | [层叠修复](stacking-context-fix-2026-09-10.md) |
| SVG 场景解码：currentColor、transform、defs/use、clipPath、描边和实例继承 | 新版 Guest 8/8，旧版 1/8；Host 91 项及资源预算 5 项通过。 | [SVG 解码和像素证据](svg-render-audit-2026-09-10.md) |
| 从实时 DOM/CSS 绘制 SVG，并处理动态创建、属性修改、viewBox 大小写及按钮内容框 | 新版 Guest 8/8、旧版 1/8；同页面真实点击后自动重绘，16 个最终像素采样全部匹配。 | [SVG DOM 真实 guest](svg-dom-live-guest-2026-09-11.md)、[属性大小写](foreign-attribute-case-fix-2026-09-11.md) |
| Cookie 快照兼容时钟回退 | 原快照在较早时钟下恢复从失败/0 条变为成功/32 条；47 项及 sanitizer 通过。 | [时钟回退修复](cookie-clock-rollback-2026-09-11.md) |

渲染冻结版本位于 `build-ds-render-fixed/snapshot/manifest.json`。登录后实际页面截图为 `build-ds-render-fixed/real-request/chat-before-send.png`；该私有诊断目录包含用户界面内容，未把历史聊天或账号文字复制到报告。

后续 Cookie 时钟恢复、XHR 常量/事件修复及响应完成诊断已进入
`build-ds-render-fixed/snapshot-xhr/`。该浏览器 SHA-256 为
`fa4cd5c8646cb55bfd4467cec5ac118759ef8ac1f88f458a33443d0ce9eef6a7`，
保留同一冻结内核，使用新建的无账号测试磁盘；没有覆盖正在运行的真实请求私有盘或用户默认磁盘。
源文件摘要、三项产物摘要均在该目录记录。真实聊天结果与 XHR 修复的归因边界见
[请求诊断](ds-request-diagnosis-2026-09-11.md)。

该组合版本随后在保留 `/browser` 的私有磁盘上实际重启并重新打开 DS 首页；
历史列表及账户区自动恢复，未重新输入登录资料，Cookie 加载/持久化报错未再出现。
此次只重开首页，没有第二次发送聊天消息。真实 XHR 分块 guest 新版 8/8、旧版 0/8，
正常 HTTP-200 传输控制保持通过，见 [XHR guest 验收](xhr-chunked-guest-2026-09-11.md)。

## 登录后的实际页面

用户已确认密码登录成功，并把 DS 留在前台。诊断使用该虚拟机已有的 `about:boxes` 和 `about:images`，没有打开旧聊天、提交消息、读取 Cookie 值或重复登录。完成后关闭了本次串口日志记录，保留原终端连接。

`build-ds-render-audit/live/ds-serial.log` 是本轮正确的 DS 诊断；更早的 `live/serial.log` 捕获的是另一个页面，不能作为 DS 证据。截图为 `live/ds-ready.png` 和 `live/diagnostic.png`。完整显示列表共 252 项，末尾明确记录 `252 shown of 252`。

| 实际症状 | 证据与归因 | 状态 |
|---|---|---|
| 聊天区域白屏、欢迎文字和输入框不可见 | 它们已有文本和控件 item。主区域外层 865×562，内部列容器仅 865×14，绝对定位容器 865×0。通用同形复现中 item 的 `clip_h=0`，真实 painter 不发出文字绘制；仅改变 overflow 或补定高即可看到文字。 | 已修复高度传递；真实登录页主区可见。 |
| 侧栏只有日期，聊天标题消失 | 标题文本先出现在最终显示列表中，侧栏祖先背景随后才画，日期又在其后。公开 CSS 的外层 z-index=8、内层 z-index=1 与最小复现一致；父背景覆盖子文字。 | 已修复层叠顺序；真实侧栏标题可见。 |
| 侧栏底部账户区不在可见范围 | 562px 视口内，侧栏被布置为 1118px，账户区落在 y=1054 附近；滚动容器高 920px。 | 同形 guest 修复为侧栏 562px、滚动区 458px，真实滚轮验证通过。 |
| 品牌、按钮等图标为空或只剩灰色圆形外观 | DOM 图像统计中 `img=0`，布局也没有图像项；SVG 不计入 DOM img 统计。通用动态 SVG 和着色缺陷已有独立复现，公开 CSS 依赖 SVG 和 currentColor。 | 八类已复现缺口已修；真实页面已有 SVG 图标。仍未逐一验收所有按钮的阴影、对比度及交互状态。 |

## 层叠顺序：有文字绘制，随后被盖住

`layout.c` 的 `zsort()` 只保存并全局排序一个 z-index，未保留祖先层叠上下文。因此一个 `z-index:8` 父容器中的 `z-index:1` 子元素，会被错误排到父背景之前。

同一 shipping parser → CSS → layout → painter 测量：

| 本地页面 | 文字绘制序号 | 父背景绘制序号 | 结果 |
|---|---:|---:|---|
| 父 z=8，子 z=1 | 1 | 2 | 背景盖住文字 |
| 仅去掉子的 z-index | 2 | 1 | 文字可见 |

Host 证据：`build-ds-render-audit/stack_probe.{c,mk,log}`。
真实 guest 截图：`build-ds-render-audit/stack-guest/page.png`，左边空白、右边有 `VISIBLE TEXT`。
脚本：`build-ds-render-audit/stack_guest.py`。它只访问本地服务器，使用独立 `-snapshot` 虚拟机，输入 ISO/disk 前后哈希一致，未操作用户正在登录的虚拟机。

显示列表中的“text item 存在”及“painter 发出过文字”都不能单独证明最终屏幕上能看到文字。这组对照保留了后续覆盖步骤。

## Flex 高度：外层补好了，内部仍按零高裁剪

通用结构是固定高度的 Flex row，里面是 `flex:1; flex-direction:column; position:relative; overflow:hidden` 容器，再里面是 `position:absolute; inset:0` 内容。当前实现先布局内部子树，再补 Flex 分配的高度，绝对定位包含块及裁剪已经按旧高度建立。

原始复现为 562→14→0px；去掉 overflow 后几何错误仍在，但文字可画；给列容器明确 562px 高度后几何和裁剪均正确。这些对照用来定位缺陷，不是站点补丁建议。

完整数值、真实 guest 三列截图和代码位置见 [Flex 高度审计](ds-cross-stretch-audit-2026-09-10.md)。

## SVG 与图标

共 11 个 native 案例：静态红色矩形对照通过，10 个不符合预期的案例归入八类问题：

1. 动态创建 SVG 没有可绘制的源，最终无图像项。
2. SVG 属性修改后仍解码旧源码，颜色保持原值。
3. `currentColor` 未使用元素或继承的颜色，而是黑色。
4. CSS `fill` 没有作用到 SVG 子节点。
5. SVG `transform` 未应用。
6. `<defs>/<use>` 未实例化图形。
7. `clipPath` 未裁剪图形。
8. 原生 button 内容仍扣除固定内边距，CSS 零内边距的小图标被压缩。

其中第 3 类有元素色、继承色两个案例，第 8 类有两个按钮尺寸案例。多数 DS 控件实际是 div；按钮原生内边距问题不能直接当作所有 DS 图标缺失的原因。

详见 [SVG 审计和真实像素证据](svg-render-audit-2026-09-10.md)。初次未加引号的 SVG 测试使静态对照也失败，已单独保留并排除；有效结论来自修正测试输入后的正对照。

## 用户协议、二维码及连接提示

- 用户协议重叠截图是有效的用户报告，但当前冻结修复镜像中未再复现。原始 HTML/CSS 的 133 个 h1/h2/p 相邻块在 1280 和 1920 测试中无重叠；未注入探针的真实 HTTPS 页面和先打开 DS 再打开协议页也正常。证据在 `build-terms-layout-evidence/`。不能据此声称已找到或修复当时重叠的根因。
- 用户已经确认“独立打开嵌入页”可用，本轮没有再次测试这个入口。真正内嵌的子文档、CSS/图像及上下文隔离已实施，独立跨端口源的真实 guest 测试画出子文字及 3072 个绿色、1024 个粉色图像像素；仅禁用嵌入绘制的负对照仍获取相同资源，但这三项均消失。最终证据在 `build-iframe-embed-fix/guest-final/{guest-current,guest-old}/`。它不等同于完整的跨域脚本 iframe，具体见 [内嵌实现和边界](passive-iframe-embed-2026-09-10.md)。
- 独立 guest 正常打开 DS 公开登录页一次，父框内已显示微信子页静态模板文字，二维码尚未出现，子文档记录 `images=0`。截图在 `build-iframe-embed-fix/guest/positive/public.png`。同时出现的“已允许/已拒绝”等文字不是服务端实际认证结果；本轮没有运行子页脚本，也没有原始子 HTML 证据来确定二维码的具体生成路径。该截图另显示登录按钮和切换登录入口文字被背景/阴影遮挡，尚未单独归因。
- `poor connection` 曾出现于验证码流程。`document.referrer` 类型错误已修复，用户后来确认密码登录成功；尚无证据能断言短信已实际送达，或所有验证码网络失败均已解决。验证码关闭不等于短信发送成功，本轮没有自动重放发送请求。

## 重启登录丢失的独立修复

发现 `make run` 依赖重建磁盘，把旧文件系统连同浏览器状态一起替换。现已保留整个 `/browser`，拒绝覆盖运行中的磁盘，并在私有副本恢复/校验旧文件系统后原子替换。真正“启动写入 → mkfs 重建 → 再启动读取”已通过，持久 Cookie 和 localStorage 保留，会话 Cookie 正常失效。

详见 [磁盘重建修复与验收边界](browser-profile-rebuild-2026-09-10.md)。这修复未来的构建丢状态；已被旧重建清掉的数据不能凭空恢复。

## 首次审计时的检查边界

本轮 Cookie 磁盘修复、iframe 策略和上下文隔离的独立检查均有明确正负对照。最后一次全树 `test-mk-wired` 检查时，其他并行工作新增的 `tests/agent.mk`、`tests/pcnet.mk`、`tests/virtio_scsi.mk` 尚未接入，导致该全树检查失败；本轮新增的测试片段已接入。日志是 `build-iframe-policy-fix/mk-wired-final.log`，没有删除或代改其他任务的片段。

2026-09-11 更新后的全树 `test-mk-wired` 已通过：278 fragments、277 reachable、1 declared。
日志为 `build-ds-render-fixed/mk-wired-final.log`；上段保留的是首次审计当时的结果。
