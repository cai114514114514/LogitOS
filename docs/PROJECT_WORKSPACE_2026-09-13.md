# Project 工作空间：首轮实现与应用联调

日期：2026-09-13。对应 [总体设计](FS_AI_NATIVE_DESIGN.md)。浏览器和第三方源码不在本轮修改范围。

## 已交付行为

- 每个用户目录直接是 Project，子目录是子 Project。空目录可立即查询身份，不需要先建任务或调用模型。
- Finder 使用文件列表与工作侧栏。可新建 Project、选择资料、生成报告、打开 TextEdit，以及暂停/取消任务。工作列表按四项分页，最多沿用现有八个任务槽。
- ProjectID 使用目录 FileID。v5 FileID 由卷身份与不复用的对象序号组成，独立于路径和可复用的 inode 槽。文件内容具有独立代数。
- Project 改名或移动到另一父目录后，Finder、任务服务与 TextEdit 通过同一身份找回报告。重启保留关联与待审候选。
- TextEdit 发起新任务时使用文档所在的 Project；新建的根目录未保存文档仍使用 `/docs`。编辑、审阅、保存继续使用原来的有界文档模型。
- 同名文件删除后重新创建不能接管旧任务；外部改写保留 FileID、推进内容代数，打开时不会用旧任务检查点替换磁盘新内容。文件单独越过原 Project 的写入范围时，任务进入待核对。
- AEX SDK 增加 FileID 查询/解析与绑定快照；`agentctl ref PATH`、`project PATH`、`lookup PATH` 可用于其他自有应用联调。普通 v4 卷仍兼容路径模式。

## 启动与复跑

在仓库根目录执行，使用独立构建目录：

```sh
make BUILD=build-project run-project
```

默认读取已有 `build/disk.img`，首次生成 `build-project/project.img`，随后连续使用该 Project 副本。`.env` 由宿主模型网关读取，代理不拿到服务密钥。已有 QEMU 占用源盘或目标盘时，磁盘保护会拒绝操作。

可用 `PROJECT_SOURCE_DISK`、`PROJECT_DISK` 指定源盘和副本；两者必须不同。首次转换仅接受 v4 源盘，已有 v5 副本通过 `PROJECT_DISK` 继续使用。升级应用时安装器先校验、保留所有原有对象身份及非目标文件字节，再校验输出、同步并原子替换副本。旧 v4 任务在转换后按已确认产物的校验结果建立初始身份绑定；不确定的结果暂停核对。

```sh
make BUILD=build-project PROJECT_OUT=build-project/check-01 test-project
make BUILD=build-project PROJECT_OUT=build-project/check-02 test-project-real
```

第一个入口包含身份及安装器负向控制、宿主崩溃测试、六种启动配置、旧任务迁移和原生应用联调；模型是确定性测试服务。第二个入口追加 `.env` 配置的 DeepSeek 实测，并自行管理一个有调用上限的宿主网关。重复运行选择新的 `PROJECT_OUT`，避免覆盖上次的磁盘证据。

实际启动器的连续两次启动测试：

```sh
make BUILD=build-project PROJECT_OUT=build-project/check-03 test-project-session
```

## 实测记录

本机独立构建与证据位于 `/tmp/logitos-project-20260913/`。临时目录清理后可用上述仓库入口重建；结构化摘要位于 `reports/2026-09-13-project-workspace.json`。

| 验收 | 结果与证据边界 |
|---|---|
| 文件身份 | 实际 LogitFS 源码，30 项检查；改名、重启、内容代数、删除重建、卷标识损坏、重复 ID、序号回滚及耗尽拒绝 |
| 断电 | v5 在每个设备写入切点与三种丢失模式下运行，1,861 项检查；挂载、fsck、旁观文件内容与分配一致性通过 |
| 绑定快照 | 全宽 ID/代数往返、失败任务重试不重置快照代数、单槽撕裂恢复、双槽损坏拒绝 |
| BIOS/UEFI × 512 MiB、2 GiB、8 GiB | 首轮六项配置通过；配套复核曾在 BIOS 8 GiB 卡住，同一构建单独复跑通过。这是仍需定位的稳定性缺口，不能把复跑通过当成修复。具体轮次见下文和结构化报告 |
| 原生 Finder/TextEdit + DeepSeek | 67 项断言通过：Finder 新建与进入 Project、从侧栏发起报告、TextEdit 未保存编辑、待审候选、改名/移动/重启、应用和保存、来源与磁盘内容核对 |
| v4 任务迁移 | 实际 v4 任务完成后转换 v5，再更新应用；确认同一任务、文档、改名与磁盘校验仍成立 |
| 普通启动入口 | 实际 `project_session.py` 启动两次 QEMU，Project 身份与内容代数保持；另覆盖原生双击进入 Project |
| 回归 | `test-fs-host`、`test-agent-review`、`test-agent-session`、`test-mk-wired` 通过；负向测试输出中的预期 FAIL 不作为正向通过替代 |

配套构建的真实模型闭环使用 `deepseek-flash`，四次请求均 HTTP 200，宿主网关记录为第 5–8 次请求，分别为 2.239、1.892、4.150、3.514 秒。这些是请求记录，不作为来宾性能基准。任务服务记录的代理峰值 RSS 样本为 1,191,936 字节、累计代理 CPU 为 83,139,784 ns。恢复计时该样本上报 0 ms，不能据此宣称零耗时；本轮未做独立空闲唤醒或吞吐基准。

实测画面（QEMU，不是概念图）：

- `/tmp/logitos-project-20260913/paired-native/finder-report.png`
- `/tmp/logitos-project-20260913/paired-native/reboot/applied-in-renamed-project.png`

## 构建配套复核

复核期间曾用较早的内核镜像搭配较新的图形应用，出现一次 Finder 启动白窗。
这条失败记录保存在 `delivery-native/`，不能被前面的通过记录覆盖。本轮另外
对内核、共享图形库及应用进行配套重建，记录 901 份源码在构建前后没有变化，
清单位于 `consistent-source.json`。配套运行记录包括 `paired-native/`、
`paired-matrix/`、`paired-migration/` 和 `menu-session/`。
Finder 的最后两处菜单/双击修正在 `menu-session/` 使用实际启动入口验证；
真实模型闭环的 Finder 是该次菜单修正前的配套版本。

配套矩阵的 BIOS 512 MiB、2 GiB 通过，BIOS 8 GiB 首次复核失败：任务始终运行中，
重复状态捕获中的服务毫秒计时不推进，宿主确定性模型请求数为 0；系统仍能执行命令。
原始记录在 `paired-matrix/bios-8G/`。未修改内核或应用的诊断复跑
`diagnostic-bios-8G/` 通过全部 55 项断言。测试现在会在失败时保留 CPU 和中断
控制器状态，不导出包含模型凭据的内存。没有把这次失败归因于 Project 身份代码，
也没有宣称白窗或时钟停滞的具体根因已经解决。

随后 `paired-uefi/` 的 512 MiB、2 GiB、8 GiB 三项配置均通过全部 55 项断言；
`clock-probe/` 另做六次 BIOS 8 GiB 独立启动，六次均观察到 PIT 计数继续推进，
未再复现停滞。各配置都有通过记录，整体稳定性仍保留上述未解决项。

## 当前边界

v5 本轮只交付身份与内容代数，普通 mkfs 仍默认生成 v4。Project 启动入口负责生成独立 v5 副本，未对用户原盘就地升级或搬动系统目录。

仍沿用 59 字节组件名、应用 128 字节路径、有界目录视图以及 1 MiB TextEdit 文档限制。没有把长名称、回收站、持久链接、多卷事务、完整版本图或通用文件操作服务算作已交付。任务的历史产物仍采用现有独立版本文件，尚未统一成整个文件系统的版本图。

FileID 解析返回的是经当前权限检查的路径，不是授予权限的句柄。解析到后续路径写入之间尚无统一的原子 FileID 提交协议；本轮验证的是改名完成后的重新关联与恢复，不宣称任意并发改名期间的事务提交安全。

本轮没有重新执行宽内存、PIE/PT_INTERP、DMA 全矩阵或中文输入法的独立输入验收，不把启动矩阵与字体显示当作这些能力的证明。

下一阶段按总体设计继续建设以父目录身份及预期版本提交的文件操作服务、操作日志与回收恢复，再迁移现有系统/应用/用户目录。这个顺序让已经接通的 Finder、TextEdit 和任务服务成为每一步的真实使用者。
