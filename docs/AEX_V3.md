# AEX v3：应用身份、代理与持久任务

AEX v3 继续内嵌静态 ELF，在原有界面和命令行入口前增加应用激活入口。同一个应用可以打开界面，也可以通过受限通道作为任务代理运行。任务数据独立保存在磁盘上；关闭窗口不会取消任务。

## 使用

在 Finder 选择 UTF-8 `.txt` / `.md` 文件，点击 **Ask Logit**；在 Tasks 输入目标、选择输出目录，再点击 **Create report**。Finder 代理读取授权来源，TextEdit 代理生成带来源字节范围引用的 Markdown 报告。报告保存为新文件，原材料不改写。

点击 **Open report** 后可以在 TextEdit 中继续输入。未保存的编辑会同步到任务的检查点；在 Tasks 中输入追加要求并点击 **Revise selected**，后续执行使用最新文档版本。Ctrl+S 发布一个新的磁盘版本。如果候选版本与人工编辑冲突，Tasks 保留两个版本并显示首个差异，用户选择 **Keep current version** 或 **Use candidate version**。TextEdit 的 Ctrl+R 可载入用户选定的任务版本。

TextEdit 的 **Ask Logit** / Ctrl+L 发布当前文档；其他自有桌面应用的 Ctrl+L 发布本应用主动提供的语义状态。系统入口为 Command+Space。当前实现不读取屏幕截图或模拟其他应用的点击。

所有自有程序都支持：

```sh
/bin/cp --aex-capabilities
/bin/cp --ask '解释这项复制计划' '把 source.txt 复制到 target.txt，尚未执行'
/bin/cp --aex-memory
```

`--ask` 的输入是明确提供给应用的文本。普通程序调用仍进入原来的 CRT 和主函数。除 Finder/TextEdit 的文档闭环外，其他应用代理提供本领域的分析和草稿；它们不会因此获得执行 shell、修改设置、联网、访问设备或任意文件的能力，也不会把建议描述成已经执行的动作。

```sh
/bin/agentctl status
/bin/agentctl document 1
/bin/agentctl verify 1
/bin/agentctl pause 1
/bin/agentctl resume 1
/bin/agentctl cancel 1
/bin/agentctl budget 1
/bin/agentctl revise 1 '保留已有内容，增加下一步计划'
```

`verify` 逐字节比较当前任务文档与公开磁盘产物。存在尚未发布的编辑时，它会明确报告差异。

`status` 同时报告服务存活时间、空闲超时次数、实际 `poll` 返回次数与错误数，以及内核累计的服务 CPU 时间。`idle_wakes` 只计没有进行模型请求时的等待超时，不等于全部 CPU 唤醒。代理 RSS 是工作过程的采样峰值；恢复时间采用当前内核的 10 ms 时钟，显示 0 可能表示低于一个时钟刻度。

## DeepSeek 配置

默认配置在 `fsroot/etc/agent.conf`，模型为 `deepseek-flash`。任务服务支持聊天补全接口的完整文本响应和 `complete_work` 函数响应，拒绝截断输出、未知动作及无效来源范围。

QEMU 验收和 `make run-agent` 使用宿主机网关。私有 `.env` 可使用 `deepseek_api` 或 `DEEPSEEK_API_KEY` 字段。提供商密钥只由宿主进程读取；虚拟机只拿到该次网关的临时令牌。请求正文和认证头不写入网关日志，`run-agent` 默认也不启用网络抓包。

已有普通构建磁盘时，保存文档并关闭使用该磁盘的 QEMU，然后在仓库根目录运行：

```sh
make run-agent
```

这个入口构建相互匹配的 broker、TextEdit、任务中心和 agentctl，在已有 `build/disk.img` 中更新 `/bin/agentd`、`/textedit.aex`、`/assistant.aex`、`/bin/agentctl`，并配置 `/etc/agent.conf`、`/etc/agent.key`，再启动模型网关与 QEMU。全盘其他文件、空目录、权限与时间戳均保留，包括 `/untitled.txt` 等不在常规重打包保留范围内的文档。正在使用的磁盘会被拒绝更新，不会自动关闭其他虚拟机。初次使用尚无磁盘时，先执行 `make build/disk.img`。

独立构建可以指定现有磁盘，例如 `make BUILD=build-agent-fix AGENT_SESSION_DISK=build/disk.img run-agent`。凭据文件路径由 `AGENT_ENV` 指定；每次宿主会话默认最多 32 次请求，可以用 `AGENT_SESSION_LIMIT` 调整，任务自身预算仍独立生效。私有会话记录位于对应构建目录的 `agent-sessions/session-*`。关闭该 QEMU 时网关同时结束；再次 `run-agent` 会安装新的临时令牌，已有任务可点击 Resume 继续。

普通 `make run` 不自动读取宿主机 `.env`。缺少模型配置或密钥时，任务服务仍应启动，文档上下文、任务和记忆可用；推理任务显示等待配置，调用预算不增加。安装配置后可恢复同一任务。

**2026-09-11 修正：**此前“启动前设置密钥权限”的实现遇到缺失文件时误以为 `chmod` 会报告 `ENOENT`，实际 LogitFS/libc 路径可能返回权限错误，导致无密钥的普通镜像反复重启服务。旧验收总注入临时令牌，漏测了这一情况。启动不再因缺少密钥退出；每次实际加载凭据前必须成功设置 `0600`，包括后来安装或配置到其他路径的文件，保护失败不读取凭据。

直接连接远端服务时，`/etc/agent.conf` 支持 `host`、`port`、`tls`、`path`、`model`、`thinking`、`max_tokens`、`key_file`。凭据文件应由任务服务所有者持有，权限 `0600`。明文 HTTP 仅允许 `127.0.0.1` 和 QEMU 宿主地址 `10.0.2.2`；不跟随重定向。字段范围、整串数字和 HTTP 字符均严格检查。

## 协议与接入

公共 ABI 位于 `include/abi/aex_agent.h`；SDK 位于 `c/lib/agent/`；应用登记表是 `c/apps/agent/catalog.json`。表中的稳定 AppID、程序路径和领域描述同时用于构建和运行，浏览器不在表中。

清单包含 `abi`、`capability_version`、`state_version`、`objects`、`actions`、`contexts`、`document_max` 和保留字段。清单声明不等于授权。内核将 AppID 与实际装载的整份 AEX 的 SHA-256、PID、uid/gid 和执行代数绑定；程序不能靠消息中的 AppID 冒充另一个应用。

| 接口 | 用途 |
| --- | --- |
| `SYS_AGENT_SPAWN` / `ag_spawn_worker` | 受限启动，显式转移唯一的新 socketpair 端点到 fd 3 |
| `SYS_AGENT_SELF` / `SYS_AGENT_PEER` | 查询内核绑定的自身和通道对端身份 |
| `ag_publish_selection/document/state` | 发布有类型、版本和明确内容的上下文 |
| `AG_SOURCE` | 按对象 ID、版本和片段读取授权快照 |
| `AG_MODEL` | 经任务服务申请推理并扣减预算 |
| `AG_CHECKPOINT` / `AG_RESULT` | 保存阶段结果、提交候选文档 |
| `AG_PROGRESS` / `AG_METRICS` | 报告进度及诊断采样 |
| `ag_memory_read/write` | 查看或显式更新本应用持久记忆 |

帧头固定 56 字节，包含魔数、协议版本、类型、长度、状态、角色、任务 ID、操作 ID、对象 ID、对象版本。负载最大 8 MiB，支持短读写。普通应用连接 `/run/logit-agent`，先验证服务的内核身份和已安装程序摘要。代理只通过 fd 3 与父服务交互，并核对父进程身份。

受限代理默认没有标准输入输出、任意文件、网络、设备、窗口、fork 或 exec 权限。用户指针走既有 usercopy；真正的文件操作由任务服务的短生命周期辅助进程在授权用户 uid/gid 下执行。辅助进程关闭其他描述符并清除附加组，不运行应用代码。

新增自有应用时，登记 `catalog.json` 中的源文件、稳定 ID、路径和领域说明，保留原构建规则并加入既有目标。链接包装器插入 `_agent_start`，保留原 `_start`，生成绑定 ELF 摘要的清单旁文件；`mkaex.py` 验证旁文件后生成 v3。未登记程序继续生成 v2，旧 AEX、裸 ELF、静态 PIE 和原有非代理 PT_INTERP 路径保持独立。v3 代理镜像目前不接受 PT_INTERP。

## 持久化与恢复

每个用户的任务位于 `/state/agents/u<uid>/t<id>`。两个快照槽将任务状态、操作回执和文档作为同一提交保存，含代数及头部/状态/文档校验；必须成功完成写入、fsync 和 close 才确认提交。不使用覆盖式 rename。

来源是任务私有的不可变副本，记录路径、对象 ID、版本、长度和校验。生成报告的引用验证真实对象与字节范围；有效引用不等同于对模型事实判断的自动证明。人工修改前的基准文档也记录长度与校验，丢失人工新增内容的候选进入比较流程。

公开写入先保存操作意图，再通过不覆盖已有名称的原子创建写入并 fsync。恢复时先检查公开结果：内容与已提交意图一致则完成核对，明确不存在则完成创建，内容不一致则进入待核对状态，禁止盲目覆盖或重复执行。已提交模型结果按操作、输入校验和文档版本缓存。

取消是终止状态；暂停可以恢复。两者都先保存状态，再撤销代理通道并停止代理。服务恢复会丢弃旧 PID 和活动计数。程序升级不删除数据；不兼容或损坏的状态暂停等待处理。

领域记忆按稳定 AppID 分目录，保存显式 UTF-8 事实与偏好，不保存 KV cache，也不自动提取材料中的指令。普通应用只能访问自己的目录；任务中心可代表用户查看或修改选定应用的记忆。示例：

```sh
/bin/agentctl memory os.logit.textedit
/bin/agentctl memory os.logit.textedit 1 '报告应简短，并保留来源引用。'
```

第二条命令以版本 1 为前提，重复的相同操作不会重复生效。Tasks 的 **Read app memory** / **Remember text** 提供对应界面操作。

## 有界资源与本轮限制

| 资源 | 当前上限或行为 |
| --- | --- |
| 单文档 | 1 MiB，超限拒绝，不截断 |
| 来源 | 32 个、合计 1 MiB，目录深度 8；UTF-8 文本/Markdown |
| 同时保留的任务 | 每用户 8 个，暂未提供任务归档/删除界面 |
| 代理/模型 | 每任务最多 2 个代理；当前调度串行执行一个工作代理与一个模型请求 |
| 模型预算 | 每任务默认 32 次尝试；用户显式增加预算 |
| 记忆 | 每应用 64 KiB；64 条操作回执，上限后明确拒绝继续写入 |
| 文档操作回执 | 每任务 64 条，上限明确报错 |
| 文档界面 | 继承现有追加/退格编辑方式；没有完整的选区、撤销和三方合并编辑器 |
| 差异界面 | 显示首个不同片段并允许选择版本，不是完整逐行合并工具 |
| 未确定的公开结果 | 保留状态和文件，需要用户核对；没有自动修复被外部改写的产物 |

不支持的资料会明确拒绝创建并指出路径，不静默忽略。新增数据范围需要新的上下文授权。PDF/OCR、语音、视觉上下文、文件系统监控、离线大模型、通用动态加载器和自动产生新目标均不属于本轮。

## 重跑验收

从仓库根目录运行，选择新的输出目录：

```sh
make BUILD=/tmp/logitos-aex-check/build \
  AGENT_ACCEPT_OUT=/tmp/logitos-aex-check/results \
  AGENT_ENV="$PWD/.env" test-agent-acceptance
```

入口先跑真实代码的负向控制和主机检查，构建全部自有 AEX，再跑无密钥启动/恢复、75 个实际启动、普通 CLI 功能、空闲对照、确定性故障和公开写入前后崩溃测试；真实模型阶段包括普通会话启动入口与 BIOS/UEFI × 512 MiB/2 GiB/8 GiB 的 GUI/同盘重启验收。故障模型明确标为 simulated，不计入真实模型证据。运行保留私有磁盘、命令、串口、模型计数、截图、逐阶段结果和镜像摘要；入口退出时只停止自己创建的网关与虚拟机。

业务输出重定向到虚拟机的普通文件。命令完成后，测试短暂停止虚拟 CPU，使用既有 LogitFS 读取器提取文件，然后继续运行；每次保留原文、命令和退出码。串口日志只负责命令传输与诊断，内核日志不能再插断业务断言。GUI 操作仍由实际指针和按键事件完成。

单独的 `make test-agent` 不调用付费模型。构建结构检查为 `make test-agent-catalog-build`。上一轮内存/PIE/PT_INTERP 和输入法回归继续使用仓库对应的 `tests/` 入口。

本轮的实际结果、产物摘要和证据限制见 [验收报告](AEX_V3_ACCEPTANCE_2026-09-11.md)；全部应用的登记路径和能力见 [应用能力表](AEX_APP_CAPABILITIES.md)。

许可证遵循既定 `LICENSING.md` 边界：公共 ABI、应用、测试和工具为 MIT；`c/lib/agent` 按 `c/lib` 规则为 GPL-3.0-or-later，组合二进制遵循仓库已有分发规则。完整许可证仍位于 `LICENSES/`，不在测试或 SDK 中复制新的许可证文件。

2026-09-13 的原生「正文＋工作侧栏」、审阅协议与验收记录见 [TEXTEDIT_WORKSPACE_2026-09-13.md](TEXTEDIT_WORKSPACE_2026-09-13.md)。
