# LogitOS 网络服务与 SSH 接线（2026-09-10）

2026-09-11 更新：下文保留该轮结果；PTY、SFTP、换钥、转发和 HTTPS 的后续实现与验证见 [新版说明](NET_SERVERS_2026-09-11.md)。

这一轮把系统推进为可用的基础 SSH/HTTP 服务器。服务由用户明确启用，系统盘不包含默认账户、密码、SSH 主机密钥或启用文件。

## 本轮实现

- 默认系统盘安装 `/bin/sshd`；HTTP 默认首页位于 `/www/index.html`，连接说明下载、连接统计和已有的 Ed25519 签名报告页面。报告页面是实际的 WebCrypto 消费者，签名/下载实现沿用前一轮。
- SSH 公钥与密码登录、`sh -c` 远程命令、独立 stdout/stderr、原始退出码、stdin EOF 和二进制文件传输。命令进入认证账户的 home，清除补充组后切换 uid/gid。
- 输出线程用 poll 同时读取两条输出管道。连接线程退出前 join 输出线程，accept 复用静态栈前 join 原连接线程；子进程关闭继承的监听器、其他连接和管道描述符。
- 未实现的 PTY/SFTP 请求明确失败；拒绝这些请求后结束该会话，避免执行客户端已经放弃的后续排队命令。请求回复使用客户端的 channel 编号。
- 主机身份加载与创建都检查强熵；已有损坏/权限不合要求的主机密钥报错，不悄悄换身份。新密钥从创建时即私有，写入失败会阻止启动。认证数据使用连接自己的缓冲区，authorized_keys 拒绝不合要求的所有者或可被组/其他用户写入的文件。
- TCP 已接受连接在应用关闭描述符之前保留自己的连接槽，即使传输已结束。这修复了“取消 SSH 命令后，下一次连接被旧会话清理断开”的实际退化。该规则也覆盖服务器端的超时/关闭状态。
- 本地 socket 读循环重新检查 SHUT_RD，使守护进程从另一线程发出的读取关闭能被正在等待的读操作看见。当前默认等待检查间隔为 200 ms。
- HTTP 流式 GET，单段字节 Range（限定区间、直到末尾、后缀）、206/416、完整长度、所有 HEAD 路径不发送响应体。未知/多段范围退回完整 GET；HEAD 忽略 Range，If-Range 因尚无验证器而退回完整 GET。
- HTTP 支持 URL 编码的中文/空格文件名、目录首页、常用脚本/字体/图片/音视频/PDF/Wasm MIME。路径长度不再静默截断，拒绝符号链接路径，流式响应按声明长度发送并记录短传输。socket 读写有 15 秒上限；仍为顺序处理，每连接一个请求。

## 首次启用

在首次启动、尚未登录其他账户的 root 控制台中：

```sh
login -a alice
```

按提示设置密码。公钥登录可选；将自己的 Ed25519 **公钥** 放进 `/home/alice/.ssh/authorized_keys`，再使用 `stat -c` 设置目录 700、文件 600，使用 `stat -o UID:GID` 设置为该账户所有。UID/GID 以 enrollment 输出为准，首个普通账户一般为 1000。

立即运行：

```sh
sshd &
httpd 8080 /www &
```

如需以后开机自动启动，再创建：

```sh
touch /etc/sshd.enabled
touch /etc/httpd.enabled
```

启用文件须为 root 所有，组和其他用户不能写。启动入口在控制台 login 认证之前，服务使用独立 EOF 输入和 `/var/log/sshd.log`、`/var/log/httpd.log`。此系统没有 `/dev/null`，启动代码使用关闭写端的管道提供 EOF。

移除相应启用文件可取消下次开机启动；文件本身不充当停止当前进程的控制接口。修改启用配置需要 root 权限。

系统盘正常重建会严格保留 `/browser`、`/state`、`/home` 和 `/download`。`/etc` 使用显式 `--preserve-merge`：已有账户、主机密钥、服务启用文件和用户配置的内容/元数据优先，新加入的默认文件保留；目录与文件类型冲突时拒绝重建。保留过程先在私有副本上回放日志并检查文件系统，再原子替换原盘。

## 从宿主机使用

QEMU 的用户网络需要明确配置转发，例如：

```sh
-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=tcp:127.0.0.1:8080-:8080,hostfwd=tcp:127.0.0.1:8081-:8081 -device e1000,netdev=n0
```

然后可以运行：

```sh
ssh -T -p 2222 alice@127.0.0.1 'pwd'
ssh -T -p 2222 alice@127.0.0.1 'cat > /home/alice/report.bin' < report.bin
ssh -T -p 2222 alice@127.0.0.1 'cat /home/alice/report.bin' > report-copy.bin
curl http://127.0.0.1:8080/
curl -H 'Range: bytes=0-99' http://127.0.0.1:8080/welcome.txt
```

首次连接时核对 SSH 主机指纹。要通过 HTTP 分享下载目录，可另外运行 `httpd 8081 /download`，然后用对应文件地址访问。系统浏览器和 `net download` 的保存目录仍为 `/download`。

可用构建命令：

```sh
make -j6 BUILD=build-net-server build-net-server/logit.iso build-net-server/disk.img
make BUILD=build-net-server test-server-semantics test-server-profile test-tcp-host test-ssh
make BUILD=build-net-server test-servers-os
```

真实客体门禁使用临时账户/客户端密钥、独立可写磁盘和 localhost 转发，结束时关闭自己创建的 QEMU。它不会启用宿主机 SSH，也不会改用户的 known_hosts。运行系统时不要使用门禁生成的 `server-guest/disk.img` 作为自己的系统盘；普通产品镜像是 `build-net-server/disk.img`。

## 验证与边界

主机：HTTP/SSH 语义 17 项；SSH wire/packet/KEX 共 169 项；TCP 245 项；配置合并 7 项；已有磁盘保留门禁 15 项。移除 Range 支持的控制出现 7 项失败；移除已接受连接所有权的控制出现 1 项明确失败。TCP 控制是状态层检查，没有生成网络故障流量。跳过配置合并的控制也明确报错，无法把丢失用户配置当成成功。

真实客体结果见 `build-net-server/server-guest/result.json`；该文件必须同时满足 `passed: true` 和 `complete: true`。检查内容包括真实 OpenSSH 公钥/密码认证、两个输出流及退出码、二进制往返、10 次重新连接、取消命令后的下一次连接、HTTP 字节一致性和 HEAD 的线上字节，以及重建系统盘再重启后的身份、账户、文件和自动启动。

最终产品构建于 2026-09-10 23:34（Asia/Shanghai）完成，完整产品应用参与的客体门禁 **39/39 通过**，结果同时为 `passed: true`、`complete: true`。运行 ISO 的 SHA-256 为 `57c9d495943eff269b8d65cf183d061f504796d7ee2c4f7164920f37883cd92d`，服务 AEX 指纹记录在结果文件中。该结果取代此前使用旧应用辅助定位问题的诊断运行；没有把诊断运行计作最终产品验收。`test-mk-wired` 同时通过，267 个片段均可达或有显式声明。

这仍不是完整的 OpenSSH 功能集：没有 PTY/完整终端作业控制、SFTP、连接复用多 channel、转发或 rekey。当前协商为 Curve25519、Ed25519、AES-128-CTR、HMAC-SHA256；长连接碰到客户端要求 rekey 时会被明确关闭。每个 daemon 最多 4 个 SSH 会话，预算来自 32 个进程文件描述符。HTTP 没有 TLS 终止、并发请求处理、Keep-Alive、多段 Range 或缓存验证器；目录需由可信用户管理，目前没有原子的 openat/O_NOFOLLOW 路径遍历接口。

同步修复了可选 agent libc 接入后 login、pkgverify、greeter、sshd 的独立 memcpy/memset 与 libc 重复定义，以及 agent.conf 的重复打包；保留独立构建能力，未删除这些消费者。

浏览器 JS/签名交互的实际执行证据在前一轮的 `NET_DOWNLOAD_KEYS_2026-09-10.md`；本轮服务器测试验证页面内容能从客体 HTTP 取回，未将这项说成新的浏览器端到端交互测试。单用户 `/download` 的多账户权限/目录隔离仍需单独完善。

协议依据：[RFC 4254](https://www.rfc-editor.org/rfc/rfc4254)、[RFC 9110](https://www.rfc-editor.org/rfc/rfc9110)。
