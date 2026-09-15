# LogitOS SSH、SFTP 与 HTTPS 接线（2026-09-11）

本文更新 `NET_SERVERS_2026-09-10.md` 的功能边界。沿用已有账户、公钥认证、私有主机身份、`/download` 和系统盘重建时的配置保留机制。

## 这一轮补齐的消费者

- SSH 现在分配真实内核 PTY。`termios`、`isatty` 和窗口尺寸 ioctl 使用同一设备；交互 shell 支持回显、原始模式、Ctrl+C 和运行中的窗口调整。远程命令不申请 PTY 时仍有独立 stdout/stderr。
- `/bin/sftpd` 是独立的 SFTP v3 子系统，认证后以账户 uid/gid 和 home 运行。标准 OpenSSH 客户端可以上传、下载、续传、列目录、创建目录、改权限、删除和重命名。实现 `posix-rename@openssh.com` 与 `fsync@openssh.com` 扩展。
- SSH 处理客户端发起的重新换钥，保留首次会话 ID，切换两个方向的新密钥；大文件传输可以跨越原来的 2 MiB 接收窗口。修复了通道声明的最大数据长度未扣除 SSH 消息头的问题。
- 管理员可以显式开启 `direct-tcpip` 本地端口转发。它使用普通 socket 的 IPv4 `connect` 和 `shutdown(SHUT_WR)`，并在通道关闭后继续接受下一条连接。
- `/bin/httpsd` 直接消费已有 TLS 服务端、X.509、ECDSA/P-256、AEAD 和随机数实现。首次启动生成持久化身份；加载已有身份会核对证书与私钥，不自动替换不匹配的身份。
- HTTP 和 HTTPS 最多同时处理 4 个请求。每个工作进程拥有独立缓冲区及 TLS 状态。
- libc 的 IPv4 STREAM `socket/bind/listen/accept/connect/getsockname/send/recv/shutdown` 已接到相同描述符实现，统一转换网络字节序；支持 `SO_REUSEADDR` 和 `SO_RCVTIMEO`。监听器实际按指定地址匹配，绑定回环地址不会变成通配监听。

本机回环也补上了真实接收路径：仅由内核内部投递的帧可以携带回环地址，TCP 的校验使用实际源/目的地址。发送后先排队，再由网络处理循环投递，避免第三次握手和数据 ACK 在发送状态更新之前递归到达。网卡输入不会因为源 IP 或 MAC 看起来本地就获得这个标记。

阻塞 socket 等待现在检查中断信号。此前一个正在 `accept` 的子进程收到终止信号后仍反复等待，连带父进程的回收无法结束；标准 C 客体对照在这条路径曾超时 180 秒，日志保留在 `build-net-server-next/accept-before.log`。修复后，对照能失败并回收子进程，普通双向通信随后通过。socket 控制使用独立中断错误码映射为 `EINTR`，避免与“文件描述符已满”混淆。

## 使用方法

先按前一份文档创建自己的账户与公钥。以下假设账户为 `alice`，宿主机到客体 SSH 端口映射为 `2222 → 22`。

```sh
ssh -t -p 2222 alice@127.0.0.1
sftp -P 2222 alice@127.0.0.1
```

SFTP 中可以使用 `put`、`get`、`reget`、`ls`、`mkdir`、`chmod`、`rename` 和 `rm`。下载到 LogitOS 的浏览器文件仍统一放在 `/download`；SFTP 默认进入账户自己的 home。

在客体 root 控制台启用转发：

```sh
touch /etc/sshd.forward.enabled
```

文件须为 root 所有且不能被组或其他用户写入。宿主机随后可以运行：

```sh
ssh -N -p 2222 -L 127.0.0.1:9080:127.0.0.1:8080 alice@127.0.0.1
```

访问 `http://127.0.0.1:9080/` 即通过 SSH 访问客体 HTTP 服务。转发目标目前接受 `localhost` 或 IPv4 字面地址。

HTTPS 在客体中运行：

```sh
httpsd 8443 /www localhost /etc/httpsd &
touch /etc/httpsd.enabled
```

默认身份是 `/etc/httpsd.key`（32 字节 P-256 私钥，600）和 `/etc/httpsd.der`（DER 证书，644）。自签名证书需要客户端显式信任。可在通过已核对主机指纹的 SSH 连接取回公有证书后，把它加入该客户端的信任配置；无需取出服务端私钥。QEMU 还需配置相应的 `8443 → 8443` 端口映射。

已有 CA 证书可使用相同文件格式；私钥是原始 P-256 标量，叶证书后可放 `.chain1.der`、`.chain2.der`。当前没有 PEM 私钥导入、CSR/ACME 或证书自动续期命令。开机自动服务使用默认参数，日志分别位于 `/var/log/sshd.log`、`/var/log/httpd.log`、`/var/log/httpsd.log`。

## 验证

独立产品构建目录为 `build-net-server-next`。运行产品镜像应使用该目录的 `disk.img`；`server-guest/disk.img` 仅是含临时测试账户的验收盘。

```sh
make -j6 BUILD=build-net-server-next all build-net-server-next/disk.img
make BUILD=build-net-server-next test-sftpd test-server-semantics test-server-profile test-tcp-host test-ssh test-mk-wired
make BUILD=build-net-server-next test-servers-os
```

主机 SFTP 门禁调用标准 OpenSSH `sftp -D`，仅用 POSIX 描述符适配器替代客体 ABI；协议实现与产品相同。去掉写入能力的私有控制返回 `SFTP batch failed`，正向 4 MiB 文件操作通过。PTY 的真实内核控制关闭回显后出现 `PTY_FAIL canonical echo`；正常设备检查通过。既有 Range、TCP 描述符寿命和配置保留控制也被实际观察到失败。

标准 C 客体额外运行真实 libc 客户端和服务端，检查回环绑定、地址字节序、接受连接、临时端口、半关闭和二进制响应；省略客户端端口转换的控制出现 `INET_FAIL client connect`，随后正向运行 `INET_FAILURES=0`。

客体门禁使用标准 OpenSSH/SFTP 及 Python HTTPS 客户端，通过临时账户、私有 QEMU 磁盘和本机映射执行。它检查交互终端、大文件与换钥、连续转发、半关闭、并发 HTTP/HTTPS、证书信任、Range/HEAD，以及重建后再次开机的服务与身份。只以结果文件同时为 `passed: true` 和 `complete: true` 的运行作为完整通过。

最终产品盘构建于 **2026-09-11 00:36:41（Asia/Shanghai）**，真实客体门禁 **63/63 通过**，`build-net-server-next/server-guest/result.json` 同时为 `passed: true`、`complete: true`。当前 ISO、SSH 和 HTTP 文件哈希与验收快照一致，完整构建文件指纹另存 `server-guest/artifacts.json`。ISO SHA-256 为 `4871cae78c77d469380cb5aa79f5fa58c592a279733266fe13b83920c40adfeb`。

主机侧 TCP 245 项、SSH wire/packet/KEX 169 项、HTTP/SSH 语义 17 项、配置保留 7 项和标准 SFTP 文件操作通过。IP 路由/ARP 正向检查及路由控制通过，测试片段可达性检查通过。额外尝试的旧 `test-eth-host` 在 macOS 编译阶段因 `arp_input/ip_input/ip6_input` 的弱符号重复定义停止，未执行，不计作通过；其编译日志在 `build-net-server-next/loopback-host.log`。本轮回环的实际通信证据来自上述完整客体运行。

## 仍然明确的边界

- PTY 尚无完整 POSIX 会话/前台进程组和作业控制；信号交给 SSH 所拥有的 shell，再由已有 shell 处理前台命令。非规范模式只支持 `VMIN=1, VTIME=0`，其他组合明确失败。
- SFTP 尚不支持设置时间戳、原子的独占创建、创建符号链接，以及部分仅凭句柄修改元数据的操作；`put -p` 等依赖时间戳的选项会失败，不应当作完整 OpenSSH 文件系统兼容性。
  - **2026-09-13 更正**：软链接、硬链接和传输限额声明已接入；个人登录公钥也增加三种 NIST ECDSA 与 RSA SHA-2，见 [更多密钥与 SFTP 接线](NET_KEYS_SFTP_2026-09-13.md)。时间戳等其余边界仍保留。
- 每条 SSH 连接只允许一个活动通道，允许一个后续转发打开请求等待关闭交叠。尚无同时多通道、远程 `-R` 转发、转发目标 DNS 解析或服务端按时间/字节主动发起换钥。
  - **同日第二轮更正**：多通道、回环 `-R` 与服务端主动换钥已接上，见 [第二轮实现与验收](NET_SSH_MULTIPLEX_2026-09-11.md)。DNS 目的地址解析仍未接上；上面的 63 项数据对应凌晨版本。
- 普通 socket 目前只有有界阻塞 IPv4 connect；非阻塞 connect、显式源地址绑定和 IPv6 描述符连接尚未接上。本轮回环验收在带网卡的客体运行。
- libc 的 `getpeername/getsockopt`、带地址的 UDP 收发和其他 socket 选项仍未接上；不要把 IPv4 STREAM 通路通过解读成完整 POSIX 网络兼容。
- HTTP 每连接一个请求，没有 Keep-Alive、多段 Range 或缓存验证器。HTTPS 默认证书一年到期，需要管理员更新；没有自动续期。
- 浏览器的签名页通过 HTTP/HTTPS 被提供，但本轮未重跑浏览器渲染与点击下载的端到端门禁，不能把标准客户端验证说成新的浏览器交互证据。

协议依据：[RFC 4253](https://www.rfc-editor.org/rfc/rfc4253)、[RFC 4254](https://www.rfc-editor.org/rfc/rfc4254)、[SFTP v3 草案](https://datatracker.ietf.org/doc/html/draft-ietf-secsh-filexfer-02)。
