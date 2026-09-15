# AEX v3 验收记录 — 2026-09-11

本轮接入 75 个自有程序（13 个桌面应用、62 个 CLI/服务程序），交付 AEX v3、认证启动、受限代理、SDK、持久任务、独立应用记忆，以及 Finder → TextEdit 的资料到报告闭环。浏览器与第三方源码未在本任务中修改。原程序的常规入口保留。

此前统一验收全部通过，使用 `.env` 中的真实 DeepSeek 凭据，模型 `deepseek-flash`。但该验收始终注入临时密钥，漏掉了普通无密钥启动；截图暴露的故障及本次修复验证见文末补记。**仍有一次旧运行中的模型调用停滞未定位，不能据此宣称稳定性问题全部结案。** 原失败证据和后续复跑分别保留，见下文。

## 最终构建与证据

| 项目 | 位置或结果 |
| --- | --- |
| 独立构建目录 | `/tmp/logitos-aex-agent-poll-20260911/build` |
| 最新统一验收 | `/tmp/logitos-aex-agent-poll-20260911/acceptance-4/verified.json` |
| 普通内核 SHA-256 | `2dc930b9716ba3adddc1e00a5f4dbc2ace15d9ad3301fab4e544ba79f0b0ebb8` |
| 宽内存/PIE/PT_INTERP/IME 回归 | `/tmp/logitos-unix-poll-final-regression-20260911/verified.json` |
| 凭据与构建复核 | `acceptance-4/credential-and-artifact-check.json` |
| 应用能力和路径 | [75 个应用的能力表](AEX_APP_CAPABILITIES.md) |
| 使用和接入方式 | [AEX v3 协议与 SDK](AEX_V3.md) |

本机 `/tmp` 对应 `/private/tmp`，两种路径指向同一批产物。工作树包含其他任务的修改，本轮未创建提交，也未整理或回退这些修改。

## 真实模型闭环

每个配置均通过真实 QMP 鼠标和键盘完成 Finder 选择、任务创建、TextEdit 打开和未保存编辑、后续要求、保存、关闭窗口及同盘重启。测试读取真实磁盘产物，核对 `4200` 元、`120` 本书、来源引用、人工输入的句子和重启前后的文档摘要；来源文件逐字节保持不变。

| 启动 | RAM | GUI / 保存 / 重启 | 真实请求数 |
| --- | --- | --- | --- |
| BIOS | 512 MiB | 通过 | 4 |
| BIOS | 2 GiB | 通过 | 4 |
| BIOS | 8 GiB | 通过 | 6 |
| UEFI | 512 MiB | 通过 | 6 |
| UEFI | 2 GiB | 通过 | 6 |
| UEFI | 8 GiB | 通过 | 6 |

共 32 次提供商请求，全部 HTTP 200。单次宿主请求耗时 0.937–3.274 秒，记录在 `acceptance-4/gateway/metrics.json`。这不代表每份初次候选都被接受：四个配置的首次修改候选未保留人工新增内容，版本保护拒绝提交；测试在 Tasks 中明确选择保留当前版本，再提交保留原句的后续要求。该流程没有静默覆盖人工编辑。

每个 `real-*` 目录保留命令、退出码、真实输出、磁盘、串口、截图和 `report-extracted.md`。例如 UEFI/8 GiB 的最终公开报告为 2,287 字节，SHA-256 为 `14da9412df4ab6f0a670d85fa231205a5409eac165c0d7bfa90dc5ac545716a9`。它包含来源范围、人工输入的句子和新增的 Next steps 部分；`report.png` 是 QEMU 的 PPM 截图仅转换文件格式所得。

引用检查证明对象和片段存在，不证明模型的每个推断都正确。例如建议和对计划日期的推算仍需用户审阅；测试没有把 HTTP 200 当作事实正确性的证明，也没有强求候选中的建议数量一定符合首次指令。

## 生命周期、隔离与负向控制

统一入口先执行负向控制，再执行对应正向检查，最后才调用真实模型。

| 检查 | 最新结果 |
| --- | --- |
| 实际 AEX 构建与登记覆盖 | 75/75 |
| 实际程序激活和故障恢复 | 104 项通过，含 75 个程序的实际激活 |
| 真实 guest 隔离检查 | 六个配置分别运行 20 项；身份、越界对象、继承描述符和禁止的动作均拒绝 |
| 公开发布前崩溃 | 10 项通过 |
| 公开文件已写入、回执未确认时崩溃 | 10 项通过 |
| 普通 CLI 功能回归 | 10 项通过，含 65,808 字节二进制的三级 cat 管道 |
| 任务状态机、版本和去重 | 76 项通过 |
| fsync / close 失败 | 7 项通过，错误不确认提交 |
| 模型配置 | 177 项通过，ASAN/UBSAN 同样通过 |
| 独立应用记忆 | 117 项通过，ASAN/UBSAN 同样通过 |
| Unix socket 原有行为 | 132 项通过 |
| 认证 Unix 通道 | 175 项通过 |
| 描述符引用与转交 | 22 项通过 |
| Unix poll | 90 项通过，ASAN/UBSAN 通过 |

故障模型明确标为 simulated，仅用于暂停、取消、断连、延迟响应、代理中断、预算耗尽、无效候选、重启和发布恢复。公开写入的两处注入在独立构建目录编译故障版 broker；普通构建不含注入。恢复按原任务与文档版本核对已有产物，两处均没有新增模型请求或重复生成公开文件。

负向控制实际移除检查点、操作去重、对象版本、权限、记忆隔离及容量限制，恢复错误的描述符转交/身份检查，或省略 fsync/close 错误检查；对应断言全部失败。Unix poll 的四个控制分别恢复 NVAL、移除注册、遗漏数据报唤醒和遗漏对端完整 shutdown 的 HUP；均由具体断言捕获。故意失败的控制日志不是正向测试失败。

## 空闲性能

修复了 AF_UNIX 缺少 `file_poll` 后端而返回 NVAL 的问题；任务服务可在真实等待队列上休眠。接收、发送、监听、数据报和单向/双向关闭均按实际状态报告就绪。

最新 BIOS/512 MiB 空闲测试：5,130 ms 客体时间中，服务 CPU 增加 5,119,162 ns，约占一个客体 CPU 的 **0.100%**；发生 5 次空闲超时，poll 错误为 0。实际返回 27 次，扣除无空闲间隔的状态查询对照 22 次后，净等待返回为 5 次。状态回复本身约 75 KiB，会跨越 4 KiB socket 缓冲，因此不能把查询产生的所有返回都算作空闲唤醒。

以上是 broker 的 CPU 记账，不是整个桌面的 CPU 占用。工作代理的 RSS 是采样峰值，未覆盖每条指令的瞬时峰值；按配置的值保存在 `acceptance-4/resource-summary.json`。恢复时间采用当前 10 ms 时钟，0 表示本次测量不足一个时钟刻度，不表示恢复没有成本。

## 内存与旧应用回归

最终普通内核与上表摘要一致。使用同一份最终源码另行编译 `WIDEVERIFY=1` 内核，其摘要为 `8601ecd635c7fb070aec813b19b54e29b0f7efcf4174ef4f31cf2142f8df4db2`。

- BIOS/UEFI × 512 MiB、2 GiB、8 GiB：6 组宽内存/静态 PIE 通过，6 组 PT_INTERP 通过。
- 18 项实际 TextEdit/IME 保存通过：每个配置均保存 ASCII `nihao `、`你好` 及 66 字节长中文串，停止虚拟机后提取磁盘原文比较。
- BIOS/8 GiB 记录匿名物理页 `0x10160e000`；virtio-blk 传输 524,288 字节，CPU 指针 `0xffff80010168e025`，设备地址 `0x10168e025`，映射、固定页和直传/回退资源回到基线。
- 稀疏高虚拟区、首次触页、fork/COW、权限、swap/回收、裸 ELF/AEX PIE 多基址、全局/函数指针、TLS 与 PT_INTERP 均由原有测试入口验证。

回归目录保存 919 项声明源码与镜像/应用 staging 的一致性记录，结束时无漂移。这些是 QEMU 测试，不是实体硬件验证，也不等于重跑了此前 DMA 轮次的每一种设备后端。

## 仍未结案的偶发停滞

`/tmp/logitos-aex-agent-poll-20260911/acceptance-3/publication-2` 曾在等待第二处发布崩溃标记时达到 120 秒期限。桌面与网络周期计数继续运行，模拟服务未收到完整模型请求。

直接读取该磁盘的两个有效快照：第 3 代为 `calls=0`，第 4 代为 `calls=1, phase=RUNNING, active=1`。因此代理启动、资料读取和 metrics 消息已经完成；停滞发生在模型计数持久化之后。配置文件读取、网络建立和服务的等待状态仍不能仅靠旧串口日志区分。没有捕获到当时的 RAM，不能把原因断言为 Unix 丢失唤醒或网络故障。

随后相同镜像进行了 30 次独立复跑，以及 1 次固定原端口 65064 的复跑，均完成发布和重启恢复；最新统一验收也通过。**这些通过不能证明原问题已经修复。** 本轮没有凭猜测修改调度或缩短超时。故障入口现已在异常时先冻结客体并保存寄存器和 512 MiB RAM，再清理进程，避免下次丢失现场；只在使用模拟凭据的故障测试中启用此保存。

健康的“模型故意延迟响应”样本已用于验证离线解析：broker 在 10 ms poll，代理在 180 s poll，TCP 已确认完整请求且等待响应，状态与注入一致。解析器及说明位于 `/tmp/logitos-agent-frozen-diagnostic-20260911`，健康样本不能替代失效现场。

## 凭据、边界与重跑

扫描最终源码、构建、验收和内存回归产物的 10,394 个文件、8,076,152,969 字节，排除私有 `.env` 本身，真实提供商密钥的精确字节匹配为 0；78 项 AEX/启动镜像摘要复核均无变化。网关日志不含认证头和请求正文；宿主密钥未装入虚拟机。这里只说明已扫描文件及精确字节匹配的边界。

Finder/TextEdit 提供完整文档闭环；其余应用接入身份、上下文、代理、记忆和领域分析/草稿，未自动化它们所有原生操作。TextEdit 上限 1 MiB，仍继承追加/退格式编辑；冲突界面显示首个不同片段并选择版本，不提供完整三方合并。其他明确上限见 [协议文档](AEX_V3.md)。

```sh
make BUILD=/tmp/logitos-aex-check/build \
  AGENT_ACCEPT_OUT=/tmp/logitos-aex-check/results \
  AGENT_ENV="$PWD/.env" test-agent-acceptance
```

输出目录必须为空。该命令会调用真实模型；只跑主机测试用 `make BUILD=/tmp/logitos-aex-check/build test-agent`。测试放在既有 `tests/`，许可证沿用 `LICENSING.md` 和 `LICENSES/` 的目录边界。

## 补记：普通镜像无密钥启动失败与会话接线

用户截图中的 `Task service unavailable (-1)` 和 TextEdit 的 `Cannot publish this document` 是同一故障：普通镜像没有 `/etc/agent.key`，旧启动分支只允许 `chmod` 的 `ENOENT`，但 LogitFS setattr/libc 路径把缺失文件报告为权限错误。`agentd` 在创建服务 socket 前退出，桌面每 5 秒重启。直接核对运行镜像证明应用都是最新 AEX v3，不能归因于旧应用或 DeepSeek HTTP 拒绝。另一个接线缺口是普通 `make run` 没有启动读取宿主 `.env` 的网关。

现在缺失配置不会使任务服务退出，也不计入模型调用预算；每次读取实际凭据之前必须成功收紧权限，失败不读取。新增 `make run-agent` 负责宿主网关、临时令牌、已有镜像的服务更新及 QEMU 生命周期。更新使用现有磁盘锁与原生 fsck 快照，只替换 broker/config/key，保留全盘其他路径，避免常规重打包丢掉根目录下保存的文档。普通 `make run` 的任务服务修复默认启用，但不会自动读取 `.env`。

本次独立构建与证据：`/tmp/logitos-agent-startup-20260911`。服务 AEX SHA-256 为 `25c9f775604c32f00d5fa5ce56f2dd6b84bbd59cf4efc898d6ca3b5b8d7e71ea`。

| 验证 | 本次结果 / 证据 |
| --- | --- |
| 恢复旧 fatal 分支 | 真实 QEMU 反复启动且服务不可连接，命中 readiness 断言；`build/agent-startup/negative/result.json` |
| BIOS / 512 MiB 无密钥生命周期 | 16 项通过；真实 TextEdit 未保存输入和 Ask Logit、等待预算 0、暂停、取消、记忆、重启、后来安装模型配置、同任务恢复；`build/agent-startup/positive/result.json` |
| UEFI / 512 MiB 无密钥生命周期 | 同样 16 项通过；`uefi-1/result.json` |
| 无密钥重启后接入真实 DeepSeek | 14 项通过，原任务完成，2 次 HTTP 200；`real-recovery-bios/result.json`、`real-recovery-gateway/metrics.json` |
| 普通会话入口接真实 DeepSeek | BIOS、UEFI / 512 MiB 各 8 项通过、各 2 次 HTTP 200；报告事实/引用/磁盘一致性与原资料检查；`real-session-bios-1`、`real-session-uefi-1` |
| 模型配置主机测试 | 182 项，ASAN/UBSAN 同样通过；5 个负向控制，包括移除密钥权限保护 |
| 会话镜像更新 | 两个负向控制抓住文档丢失、凭据变得可读；正向检查全路径内容/元数据、打开中的磁盘、fsync 失败及目录类型冲突 |
| 既有代理主机回归 | `test-agent` 各目标完成；含状态机 76、记忆 117、Unix 132、认证通道 175、FD 22、poll 90 |
| 用户普通磁盘更新 | 已关闭原 VM 后，仅替换 `build/disk.img` 的 `/bin/agentd`；394 个路径逐项比较，其他路径内容及元数据不变；`ordinary-disk-install.json` |
| 普通完整镜像启动 | 使用更新后磁盘的私有副本和普通 `build/logit.iso`，BIOS / 1 GiB 服务 ready，真实 status RPC 成功；`ordinary-boot/result.json` |
| 提供商密钥精确扫描 | 1,300 个修复产物文件、5,629,965,183 字节，精确匹配 0；`artifact-check.json` |

真实请求合计 6 次，均 HTTP 200。确定性故障模型只用于可重复的生命周期验证，不计入真实模型证据。新的无密钥启动负向控制是对应正向门禁的前置依赖，并已接入统一验收；普通会话的真实模型测试也进入统一入口。

本次没有重跑整套 6 配置宽内存/PIE/IME 矩阵，也没有据此关闭上文的旧偶发停滞问题。`test-mk-wired` 开始时 278 个片段检查通过；工作树并行新增到 284 个片段后，末次检查报告 `tests/worker_globals.mk` 的 `test-worker-globals` 未被根 Makefile 接入。它不属于本次修改，保留原状；本次 `tests/agent.mk` 已可达，不能把全仓门禁写成通过。

单独重跑（构建已准备、输出目录需选择新路径）：

```sh
make BUILD=/tmp/logitos-agent-startup-20260911/build \
  AGENT_STARTUP_OUT=/tmp/logitos-agent-startup-rerun test-agent-startup
make BUILD=/tmp/logitos-agent-startup-20260911/build \
  AGENT_SESSION_OUT=/tmp/logitos-agent-session-rerun \
  AGENT_ENV="$PWD/.env" test-agent-session-guest
```
