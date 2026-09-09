# 对 c/net/ssh（LogitOS 自研 SSH-2 服务端）做攻击测试

**对象**：`c/net/ssh/`（7 个模块，服务端协议库）+ `c/apps/coreutils/sshd.c`（守护进程）。两个位置当前 git status 干净、无竞争 agent，是本任务修改区。全程 `BUILD=build-sshattack`，boot 测试用独立端口，避免与其他 agent 冲突。

已从代码确认的具体攻击假设（待测试证实/证伪）：
- `input_relay` 双 CHANNEL_EOF → 第二次走 `sys_close(-1)`（sshd.c:833 无守卫，kernel fd 表语义未知）
- `run_channel_setup` 请求循环对第二个 CHANNEL_OPEN 静默 `continue`（sshd.c:756），与 ssh_conn.h:10-13 "每连接一个 session"的声明不符——客户端会挂等
- 接收方向无窗口核算：客户端可无限超窗发 CHANNEL_DATA（input_relay 只管写入子进程 stdin，OUR_INIT_WINDOW 2 MiB 只对发送方向生效）
- 全链路无空闲超时 + 8 个槽位 + 阻塞读 → 8 条慢速连接可永久占满 sshd（pre-auth DoS）
- 其余：版本行/KEXINIT/认证/通道各解析层的畸形输入、低阶点完整清单、认证跨会话签名重放、MaxAuthTries 边界、rekey 请求路径

## Phase 0 — 基线（suspect the apparatus first）
1. `BUILD=build-sshattack make test-ssh-wire test-ssh-packet test-ssh-kex`、`make test-ssh-os` 先全绿，确认现有地基可用。
2. `python3 tools/audit_tests.py` 记录当前 5 个 SSH gate 的 UNWIRED (NEW) 状态（改前快照）。

## Phase 1 — host 级对抗测试（快速迭代，无 QEMU）
1. **敌意 KEX 向量**：扩展 `tests/unit/ssh_kex_gen.py`（或加 `--adversarial` 段）生成：全部 12 个规范低阶点 + 全零 + 非规范编码（高位置位等）+ qclen 31/33/0。断言：低阶点被现有 all-zero 检查拒绝、非规范点行为被钉住（拒绝或 RFC 7748 掩码后一致，二选一写实）、无密钥派生、无崩溃。ASAN 构建。
2. **解析器 ASAN fuzzer**：新建 `tests/unit/ssh_attack_fuzz.c`（仿 x509_fuzz.c/http1_fuzz.c 家法）：确定性 PRNG + SEED/SCALE，打击 `ssh_r_string/mpint/namelist`、`ssh_kexinit_negotiate`、`ssh_authreq_parse/ssh_auth_parse_publickey/ssh_auth_parse_password`、`ssh_parse_channel_open/request/data/window_adjust`、`ssh_pkt_recv`（明文与固定测试密钥的加密相位）。LSan 探针仿 demux.mk（darwin/arm64 无 LSan 则响亮 SKIP）。
3. **authorized_keys 解析滥用**：host 单测喂垃圾（缺字段、截断 base64、错误类型、超长行、CRLF、重复键）给 `ssh_authkeys_match`。

## Phase 2 — guest 内恶意客户端电池（打真实二进制）
1. **`tests/boot/ssh_attack_client.py`**：raw-socket SSH-2 客户端，Python `cryptography` 做 x25519/ed25519（依赖已被 ssh_kex_gen.py 证明存在），与服务器零共享代码（独立 oracle 家法）。能完成真实握手+认证，也能在任意阶段偏离。
2. **`tests/boot/run-ssh-attack-test.sh`**：复用 run-ssh-test.sh 的全部脚手架（mk_ssh_disk.py 磁盘、串口注册、hostfwd、SSHD_READY 轮询、mktemp 下现铸密钥）。每发一次攻击：(a) 带超时发射 (b) 断言特定结果（拒绝种类/DISCONNECT/关闭） (c) **控制**：干净登录必须仍成功——每个攻击都带存活控制。
3. 电池约 20 项：pre-KEX 畸形（超长版本行、空 name-list、无公共算法、qclen 错、低阶点、乱序消息、明文 USERAUTH_SUCCESS 伪造、帧滥用）→ post-KEX（错键 MAC、rekey 诚实拒绝、6 次失败断连、跨会话签名重放、畸形签名、第二 CHANNEL_OPEN、WINDOW_ADJUST 回绕、伪通道号 CHANNEL_DATA、双 EOF、超长 exec 命令、超窗数据洪泛定量表征）→ 可用性（8 条慢速连接锁死 + 第 9 条 CONN_REFUSED）。
4. **`tests/boot/ssh_tamper_proxy.py`**（仿 tls12_tamper_proxy.py，on-path 表征，只表征不改协商）：翻 KEXREPLY 字节→客户端签名验证必须死；NEWKEYS 前注入 IGNORE→**表征连接存活**（这是拒绝 strict-kex 决策的已接受代价，注释引 ssh.h:60-65）；删客户端 NEWKEYS→修复前永久阻塞、修复后有界丢弃；翻首包密文字节→MAC 错死。

## Phase 3 — 修复（每项：先看攻击测试红，再修，再绿——家法第 5 条）
1. 双 EOF 守卫（`if (cc->child_in_w >= 0)`）；先查 kernel `sys_close(-1)` 是否本身不安全，若不安全最小修 kernel fd 路径。
2. 第二 CHANNEL_OPEN → 显式 CHANNEL_OPEN_FAILURE 回复（兑现 ssh_conn.h 的既有声明）。
3. CHANNEL_DATA 通道号校验（非本连接通道的数据忽略）。
4. 接收方向窗口核算：超 OUR_INIT_WINDOW → DISCONNECT protocol error。
5. **pre-auth 空闲超时**（设计级，用户已确认）：若传输层无 recv 超时选项，用 watchdog 线程扫槽位限期 + `sys_shutdown` 解阻塞（镜像 output_pump 的既有手法）。post-auth 空闲**故意不加**——长静默会话是合法 SSH 用法，写注释说明。
6. strict-kex 维持拒绝（设计决策不动，on-path 探针只表征）。
每处修复注释写：为什么、测到什么、故意不做什么。

## Phase 4 — 接线与审计
1. 新建 `tests/ssh_attack.mk`（独立文件防整文件覆盖）；Makefile 在 pre-wire 段加一行 `-include`（Makefile 在他人修改列表中，用专为此设计的 pre-wired 段，编辑前查 mtime）。
2. gate：`test-ssh-attack-host`（fuzz 负控 sabotage 构建为前置条件，"ERROR: AddressSanitizer" 锚定 grep——demux 假绿教训）、`test-ssh-attack-kexvec`、`test-ssh-attack-os`（前置依赖 `test-ssh-os`）。
3. 全部接线（用户已确认）：fragment 内加 `ci-host: test-ssh-wire test-ssh-packet test-ssh-kex test-ssh-attack-host ...` 与 `ci-boot: test-ssh-os test-ssh-attack-os`（仿 canvas.mk:336 的 fragment 内 ci 行模式，避免动共享 Makefile 其他区域）。
4. 修正 ssh.mk 头部已过时的 "NOT YET REACHABLE" 声明（-include 实际已落地，按"旧声明旁留更正"家法改写）。
5. `make test-mk-wired`、`test-audit`（UNWIRED (NEW) 消失，baseline 不涨）、`test-negctl-drift`、liveness 检查全绿。

## Phase 5 — 验证与报告
1. 全电池绿（BUILD=build-sshattack）；`make` 一次确认 kernel 构建不受影响；`rm` 后重链 sshd.elf 证明链接线（家法）。
2. sshd 不进产品镜像 APPS（产品面决策，超出本次范围，报告注明）。
3. CLAUDE.md Networking 段补攻击 gate 与修复事实；查 ssh.h:15-17 引用的 ssh_algorithm_notes.txt 是否真存在，不存在则更正引用。
4. 报告：每项发现带红→绿证据、guest 内测量值、故意不做清单（strict-kex、post-auth 空闲、产品镜像不放 sshd）。

**明确不做**：strict-kex 实现、fit 任何站点、模仿其他浏览器/客户端信号、绕过或删 gate。