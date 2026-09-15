# SSH 多通道、远程转发与主动换钥（2026-09-11 第二轮）

本轮接续 [凌晨的服务器接线记录](NET_SERVERS_2026-09-11.md)，工作目录为 `build-net-ssh-complete`。该记录中的 63 项结果保留为历史证据；当时尚缺的同时多通道、远程转发和主动换钥已实现。

**2026-09-13 后续接线**：个人登录密钥新增 NIST ECDSA 和 RSA SHA-2，SFTP 增加链接操作与限额声明，见 [后续实现与验收](NET_KEYS_SFTP_2026-09-13.md)。本文的 80 项仍对应 9 月 11 日镜像。

## 实际功能

- 每个 SSH 连接最多 4 个活动通道，分别拥有 64 KiB 输入队列、流量窗口和子进程描述符。一个程序暂停读取 stdin 时，其他命令仍可执行。关闭后使用新的通道编号，迟到的 CLOSE 不会关闭下一条通道。
- 标准 OpenSSH `-R` 创建客体回环监听，支持端口 0 动态分配、取消和同端口重新绑定。`forwarded-tcpip` 可以与命令、SFTP 和 `-L` 共用一个加密连接。待接收连接留在有界 TCP backlog 中，直到有可用通道。
- 服务端默认在约 1 GiB 传输量或 1 小时后主动换钥。计数包含保守的封装开销，允许提前换钥；`sshd [port] [rekey_bytes] [rekey_seconds]` 可设置十进制阈值，某项为 0 表示关闭该触发条件。首次会话 ID 保留；换钥期间已在途的数据进入对应通道队列，控制回复在 NEWKEYS 后发送。
- Native IPv4 TCP 的 `poll` 接入实际监听/收发等待队列，支持空闲等待、接收、EOF、发送缓冲可用性和关闭。Unix socket 仍使用其既有实现；UDP/raw readiness 尚未实现。

这些功能继续消费现有 X25519、Ed25519、AES-CTR、HMAC-SHA256 和密钥派生实现，协议字节仍由 `c/net/ssh` 提供。

## 用法

按前一份记录配置个人账户、公钥和已核对的主机指纹。转发需要管理员创建 `/etc/sshd.forward.enabled`，要求 root 所有、普通文件且不可被 group/other 写入；服务启动和开机启用仍沿用前一轮配置。

假设宿主机 `2222 → 客体 22`，宿主机有 HTTP 服务监听 `127.0.0.1:8080`：

```sh
ssh -p 2222 -N -R 18080:127.0.0.1:8080 alice@127.0.0.1
```

客体程序即可访问 `http://127.0.0.1:18080/`。把 `18080` 改成 `0` 可请求动态分配。远程监听限定为 `localhost` 或 `127.0.0.1`，固定端口须不低于 1024。`-L` 目的地址支持 `localhost` 和 IPv4 字面地址。

## 修复的回归

1. TCP 发出请求 EOF 后进入 FIN_WAIT，旧代码只在 ESTABLISHED 中通告读取后的新窗口。慢读者可能排空零窗口却无法恢复响应；现在 FIN_WAIT 也发送窗口更新。原始半关闭状态检查实际打印 `FAIL half-close drain advertises receive window`，修复后通过。
2. TCP EOF 读取曾直接释放 CLOSED 槽位，即使重复描述符仍持有它。现在保留 `app_owned`，最后一个描述符关闭才释放，避免另一个连接抢先复用。
3. INET `poll` 原来返回 NVAL。把它误作“可读”会进入阻塞读，阻止空闲换钥和独立远程接受连接。现在返回真实状态并注册等待队列；本地 SHUT_RD 也唤醒等待者。
4. 新的远程通道创建在本轮时间采样之后，无符号减法一度将其误判成已超时。超时检查现在重新采样时间；关闭交叠期间不会接受后立即丢弃正常连接。

## 验收与证据

```sh
make -j6 BUILD=build-net-ssh-complete all build-net-ssh-complete/disk.img
make BUILD=build-net-ssh-complete test-unix-poll test-tcp-readiness test-tcp-half-window test-ssh test-server-semantics test-sftpd test-mk-wired
make BUILD=build-net-ssh-complete test-servers-os
```

`test-servers-os` 的前置依赖包含私有单通道对照构建、TCP 半关闭窗口对照和 TCP readiness/EOF 所有权对照。单通道版本须在普通 OpenSSH 的 `multiple commands share one SSH connection while one stdin is paused` 检查上失败；客户端使用 `ProxyCommand=false` 禁止另起连接掩盖失败。流量/时间换钥分别验证，流量测试关闭定时触发，客户端阈值保持高位；关闭服务端触发条件时，仅观察到首次 NEWKEYS。

结果以 `build-net-ssh-complete/server-guest/result.json` 同时为 `passed: true`、`complete: true` 为准。门禁运行私有临时账户和磁盘，使用标准 OpenSSH、SFTP 与 HTTP/HTTPS 客户端。`server-guest/disk.img` 是测试盘，实际使用应选择 `build-net-ssh-complete/disk.img`。

最终产品盘于 **2026-09-11 15:23:53（Asia/Shanghai）**构建；上述完整门禁 **80/80 通过**，结果文件的两个完成标志均为 true。涵盖已有 PTY、SFTP、HTTP/HTTPS、半关闭、连续转发和重建重启保留检查。执行过的对照在预定检查上实际失败，随后正向通过。标准 TCP 状态机 **245/245**、新增 readiness **8/8**、半关闭窗口 **1/1**、Unix poll **90/90**、SSH 协议/密钥交换 **169/169**、服务器语义 **17/17** 与 SFTP 文件操作通过；测试片段可达性门禁通过。

`server-guest/artifacts.json` 保存镜像、服务程序、测试程序和源码 SHA-256。验收 ISO 与当前产品 ISO、SSH/HTTP 程序已逐一比较相同；ISO SHA-256 为 `950360ab2f17add421ac8a86eb7d3e37fa8e5b36edda64678792f039e554d183`。详细日志为 `final-guest.log`、`final-host.log`、`final-tcp.log`。

## 边界

- 服务端最多 4 个 SSH 连接，每连接最多 4 个活动通道，仍受进程 32 个描述符等系统资源预算限制；超出预算不代表无限排队或完整 OpenSSH 并发规模。
- 每连接最多配置 2 个远程监听，且受 TCP 全局监听槽位限制。启动 SSH、HTTP、HTTPS 后可用槽位会减少。远程转发只监听客体回环；不含公网网关绑定、IPv6 或 DNS 目的地址解析。
- PTY 尚无完整 POSIX 会话、前台进程组和作业控制。SFTP 时间戳等前一轮明确缺失的操作仍未补齐。
- HTTP/HTTPS 仍是基础文件服务；证书自动续期、完整 HTTP 缓存与 Keep-Alive 不在本轮。没有新增浏览器渲染或点击下载的端到端证据；下载保留检查来自真实客体 `net` 消费者。

协议依据：[RFC 4254](https://www.rfc-editor.org/rfc/rfc4254)、[RFC 4253](https://www.rfc-editor.org/rfc/rfc4253)。
