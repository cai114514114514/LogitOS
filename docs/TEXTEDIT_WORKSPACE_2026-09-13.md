# TextEdit：正文和文档工作侧栏

用户选择的「正文＋工作侧栏」已经接到原生 TextEdit。左侧编辑或审阅文档，右侧显示该文档的目标、授权资料、Finder/TextEdit 进度、状态和后续要求。浏览器和第三方源码没有参与本次修改。

## 使用

在 Finder 打开报告，或直接在 TextEdit 打开 UTF-8 文本/Markdown。宽窗口默认显示「这份文档的工作」。在底部输入要求，回车提交。首次发起任务包含尚未保存的编辑；以后继续同一任务。后台工作不随窗口关闭而取消。

候选稿完成后可在「当前正文／拟修改」之间切换，再选择「应用修改」或「保留当前版本」。候选稿确认前不替换正文、不生成新的公开报告文件。应用后通过原有操作回执和发布意图机制保存新的报告版本。保存按钮及 Ctrl+S 可以另外保存人工编辑的版本。

「已授权资料」可打开任务中保存的来源版本；正文中的完整引用显示为可点击的「来源 N」。单次读取最多 16 KiB，可以继续下一段。其他对象、错误对象版本和不合法范围被服务拒绝。

编辑支持 UTF-8 光标、鼠标定位、Shift 选区、Ctrl+A/C/X/V、Ctrl+Home/End、一层撤销/重做（Ctrl+Z 再次切换）、滚动，以及 1 MiB 容量上限。工具栏字体和字号控制显示；B/I 插入 Markdown 标记。输入采用短暂合并后的检查点，保存、提交要求和关闭窗口会先同步。

## 状态和兼容性

- SDK 增加 `AG_WORK_VIEW`、`AG_WORK_DECIDE`、`AG_WORK_SOURCE`，没有改变已有消息编号、`ag_task` 大小或快照状态版本。旧 CLI 和已有 AEX 继续使用原协议。
- 待审阅策略和候选稿放在任务目录下独立的 `review/slot0`、`review/slot1`，复用已有带校验、代数及 fsync 检查的快照实现。候选稿先持久化，随后任务进入等待审阅；这两步间中断可恢复。
- 决策同时绑定当前正文版本和已经显示的候选稿代数。过期决定及取消后的决定不能提交。`operation` 继续用作 SDK 请求关联值，服务不会擅自改写它。
- 从已有文档开始的工作保存原文档路径绑定，重开同一文档可恢复同一任务。代理依据保存的授权对象版本工作，原始来源磁盘文件保持原样。
- TextEdit 可以控制自己的文档工作；任务中心也能使用新审阅协议。普通命令行创建的任务保留原来的自动发布行为，打开到 TextEdit 后才启用后续审阅。
- 多窗口的过期人工编辑单独保存为 `local-draft`，不会覆盖代理候选稿；可用 `agentctl draft ID` 取出。当前界面没有三方自动合并工具。

候选稿的颜色标记使用共同前缀/后缀之间的变化范围，内部可能包含未变的行；不是逐字修订追踪。当前版本是文本/Markdown 编辑与格式预览，尚不是完整富文本排版器，也没有分页打印、任意对齐、字体样式持久化或多级撤销。窗口小于侧栏需要的空间时，保留正文并提示放大窗口。

## 普通启动与可重复构建

已有普通磁盘时运行 `make run-agent`。入口构建并安装匹配的 TextEdit、broker、任务中心和 agentctl，连接宿主 `.env` 所配置的 DeepSeek 网关。它保留已有磁盘中的其他文件、空目录和元数据，拒绝修改正在使用的磁盘，不重打包用户数据。普通 `make run` 不自动启动宿主模型网关。

SDK/libc 的静态库现在先在临时目录中按确定顺序重建，再替换原库。原来增量更新静态库会保留历史成员顺序，使代码完全一致的普通构建和独立构建出现不同 ELF 布局和 AEX 身份哈希。本次四份普通 AEX 与独立构建已逐字节核对一致。

## 重跑

先准备独立构建目录及其 BIOS/UEFI 启动镜像、普通基础应用与 `agent-runtime-test.aex`，不要复用正在运行的磁盘。本次沿用此前已验收的内核启动镜像，单独编译应用和服务。

```sh
make BUILD=/tmp/logitos-textedit-check/build test-agent test-mk-wired
make BUILD=/tmp/logitos-textedit-check/build \
  TEXTEDIT_WORK_OUT=/tmp/logitos-textedit-check/evidence test-textedit-work
```

第二个入口先运行实际绕过审阅的 broker 负向控制，再验证独立文档绑定和原生审阅流程。输出目录必须是新的。直接执行脚本时，也应先执行负向控制。当前机器可直接复用已准备好的 `/tmp/logitos-textedit-work-20260913/build` 作为 BUILD，并指定新的 TEXTEDIT_WORK_OUT。

已运行宿主网关时，真实模型验收使用：

```sh
python3 tests/boot/run-textedit-work.py \
  --build /tmp/logitos-textedit-check/build \
  --gateway /path/to/private-gateway-state \
  --out /tmp/logitos-textedit-check/real
```

固件与内存参数为 `--mode bios|uefi --ram 512M|2G|8G`。确定性模型只作为故障和生命周期测试。`tests/boot/render-textedit-work.py` 可从已完成真实任务的磁盘副本生成原生审阅截图，会再请求两个真实模型结果，不应用候选稿；`--existing-proposal` 只渲染已保存的候选稿，不再请求推理。

## 本次证据

证据根目录：`/tmp/logitos-textedit-work-20260913/`。

| 验证 | 实际结果与证据 |
|---|---|
| 主机回归 | `test-agent-final.log`：原有代理状态/记忆/存储/IPC 测试通过；新增审阅与编辑检查 19 项；过期版本负向控制命中实际断言 |
| 审阅负向控制 | `final/negative/result.json`：真实 broker 绕过审阅后，被“确认前正文不得改变”断言捕获 |
| 原生完整闭环 | `final/positive/result.json`：21 项，包含真实点击资料、提交要求、重启、应用、保存、保留版本、过期决定和来源越界拒绝 |
| 文档直接启动 | `final/document/result.json`：7 项，覆盖未保存正文、原文路径绑定、重启后在同一窗口入口继续及保存到新报告 |
| DeepSeek 完整闭环 | `acceptance/deepseek/result.json`：BIOS / 512 MiB，20 项，6 次真实模型调用，逐字节检查报告和磁盘产物 |
| UEFI | `acceptance/uefi8g/result.json`：UEFI / 8 GiB，20 项原生流程检查；模型为确定性故障夹具 |
| 最终可重复构建 | `binary-equivalence.json`：四个普通 AEX 与独立构建逐字节一致；`reproducible-positive/result.json` 复验新库顺序的完整原生流程 |
| 最终真实渲染 | `preview-final/result.json`：BIOS / 2 GiB，最终 AEX 使用真实模型生成待审阅稿；确认人工句子仍在，正文保持不变 |
| 中文输入 | `ime-complete.log`：ASCII 控制、你好、完整候选/标点/长句，保存后逐字节核对。使用现有扩展 Qwen 词库；小型验收磁盘通过 `IME_TEST_DOCK_COUNT=3` 声明实际 Dock 数量 |
| 构建接线 | `wired-final.log`：301 个测试片段接线检查通过 |

最终文字配色和浅绿底色经过 `native-polished/native-review.png` 的原生截图检查；这是完整功能回归之后的纯绘制调整。

普通 `build/disk.img` 已更新四个匹配程序，其他 409 个路径的内容及元数据逐项一致，见 `ordinary-install.json`。更新前镜像保存在证据目录的 `ordinary-before.img`。真实模型完整验收及两次最终渲染共请求 10 次，全部 HTTP 200；宿主凭据未写入源码或日志。

本次没有重新声称完成 BIOS/UEFI × 三档 RAM 的全部宽内存、PIE/PT_INTERP、DMA 和所有桌面应用回归；这些内核能力使用之前的验收镜像，本轮证据针对新增 TextEdit 流程、协议和输入法兼容性。模型响应不被模拟结果替代。
