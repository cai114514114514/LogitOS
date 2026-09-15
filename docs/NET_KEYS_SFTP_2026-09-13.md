# SSH 更多密钥、SFTP 链接与下载指南（2026-09-13）

接续 [9 月 11 日的 80 项服务器验收](NET_SSH_MULTIPLEX_2026-09-11.md)。本轮独立产品构建位于 `build-net-keys-0913`，此前镜像和验收目录保留。

## 密钥现在能用于什么

已有 RSA 和三种 NIST 曲线的密码学运算，现在接入真实 SSH 公钥登录。使用标准 OpenSSH 在自己的电脑生成密钥，把 `.pub` 的完整公钥行加入账户的 `~/.ssh/authorized_keys`，即可用于远程命令、文件传输和已有端口转发。

| 个人密钥 | SSH 签名算法 | 本轮验证的大小 |
| --- | --- | --- |
| Ed25519 | `ssh-ed25519` | 既有默认登录继续通过 |
| ECDSA P-256 | `ecdsa-sha2-nistp256`，SHA-256 | 256 位 |
| ECDSA P-384 | `ecdsa-sha2-nistp384`，SHA-384 | 384 位 |
| ECDSA P-521 | `ecdsa-sha2-nistp521`，SHA-512 | 521 位 |
| RSA | `rsa-sha2-256`、`rsa-sha2-512` | 2048、3072、4096 位，各跑两种签名 |

RSA 公钥编码仍为 `ssh-rsa`，不能把授权行前缀改成 `rsa-sha2-512`。服务端响应客户端的 `ext-info-c`，在首次 NEWKEYS 后发送 `server-sig-algs`；这个协商信息让 OpenSSH 真正选用 SHA-2。后续换钥不重复发送初次扩展消息。

`authorized_keys` 的读取上限从 4 KiB 扩到 16 KiB，公钥解码和签名数据缓冲也覆盖 RSA-4096。验收把新增公钥放在文件的第 4 KiB 之后，检查实际账户文件读取。授权行支持空白分隔、注释和 CRLF；带 `restrict`、`command=` 等选项的行继续排除，避免丢掉配置中的限制后授予登录权。

`c/net/ssh/ssh_pubkey.c` 负责算法与公钥/签名编码的一致性，调用已有 Ed25519、ECDSA、RSA 和 SHA-2 实现。ECDSA 曲线及 Barrett 表的首次初始化在启动认证工作线程之前完成；线程创建后共享参数只读，避免并发首次使用发生初始化竞争。

服务器主机身份仍是原有的持久化 Ed25519。新增的是**个人登录密钥的导入与使用**；本轮没有给客体实现 RSA 私钥生成、OpenSSH 私钥解密或 RSA 主机身份。

## 文件管理与实际下载

SFTP 新增：

- `ln -s` 创建软链接，保留相对目标文字，按链接所在目录解析。参数顺序遵循实际 OpenSSH v3 的 `targetpath, linkpath`。
- `ln` 使用 `hardlink@openssh.com` 创建硬链接，直接调用账户权限下的 VFS。验证删除原文件名后仍可取回完整数据。
- `limits@openssh.com` 声明实际容量：完整包 65540 字节、READ 数据 65520 字节、WRITE 数据 65511 字节、8 个句柄。WRITE 限额扣除了消息与句柄封装，避免把缓冲容量误报成数据容量。

文件服务首页新增 [可下载的密钥与 SFTP 指南](../fsroot/www/ssh-keys.txt)，地址为 `/ssh-keys.txt`。浏览器入口带 `download` 属性；客体 `net download` 实际把该文件保存到 `/download`。完整使用步骤在指南中，包含个人密钥生成、安装公钥、登录和链接命令。

## 验收方法

```sh
make -j6 BUILD=build-net-keys-0913 build-net-keys-0913/logit.iso build-net-keys-0913/disk.img
make BUILD=build-net-keys-0913 test-ssh test-server-semantics test-sftpd test-mk-wired
make BUILD=build-net-keys-0913 test-servers-os
```

`test-servers-os` 已把新密钥和 SFTP 主机门禁作为前置依赖。新增的两种控制只在私有构建中关闭功能：

1. 只保留 Ed25519 的认证构建，必须先通过 Ed25519，再在独立生成的合法 P-256 公钥检查上实际失败。
2. 关闭 SFTP 链接操作的构建，必须先完成普通文件上传，再在标准 OpenSSH 的软链接创建操作上实际失败。

密码学主机门禁通过 Python `cryptography` 独立生成密钥和签名，共 **51 项**，覆盖 RFC 4252 签名数据、授权行、公钥编码、各签名算法与扩展消息。SFTP 主机门禁由标准 `sftp -D` 驱动相同产品协议代码，验证约 4 MiB 文件的传输、续传、权限、链接、删除与限额响应。既有 SSH wire/packet/KEX **169 项**、服务器语义 **17 项**和 Make 片段可达性也通过。

真实客体验收使用产品二进制、临时个人密钥和账户、私有 QEMU 磁盘、标准 OpenSSH/SFTP/HTTP/HTTPS 客户端。新检查包含九种公钥登录组合、三个 NIST 密钥并发登录、RSA-4096 文件传输、SFTP 链接和重建重启后的持久性；此前多通道、主动换钥、PTY、转发、HTTPS 身份与下载检查继续执行。

最终通过以 `build-net-keys-0913/server-guest/result.json` 的 `passed` 和 `complete` 同时为 true 为准。`final-guest.log`、`final-host.log` 和 `pubkey-host.log` 保存日志。运行产品请使用 `build-net-keys-0913/disk.img`；`server-guest/disk.img` 是包含临时测试账户的验收盘。

**最终结果：99/99 项真实客体检查通过，两个完成标志均为 true。** 产品盘构建于 **2026-09-13 12:15:08（Asia/Shanghai）**。RSA-4096 授权、硬链接的数据、下载到 `/download/ssh-keys.txt` 的指南均通过重建后再次启动的检查。对应 ISO、SSH 和 HTTP 程序与运行时记录逐一比较一致；`server-guest/artifacts.json` 保存产品文件和本轮相关源码的 SHA-256。ISO 指纹为 `19f314e3fa1ebe2b9ee0fb32a9d3d2c4acc058189fe468002fdd7cfacec57e80`。

## 已核对但仍未补齐的边界

- SSH 转发目标目前仍限于 `localhost` 或 IPv4 字面地址。DNS 内核已有独立查询池，但现有用户 ABI 的 `SYS_NET_DNS/RESULT` 仍指向全局查询；直接在多连接 SSH 中复用它会发生查询覆盖。本轮没有这样接线。
- SSH 主机证书、硬件安全密钥、PQ 混合密钥交换、更多主机身份算法及完整授权选项仍未实现。RSA-SHA1、DSA 不在此次算法列表中。
- SFTP 时间戳写入、原子独占创建、部分句柄元数据操作仍缺内核接口；`put -p` 不能声称支持。PTY 仍缺完整会话、前台进程组和作业控制。
- HTTPS 的 PEM 私钥导入、CSR/ACME 和自动续期仍未实现。
- 本轮新增下载的运行证据来自客体 `net` 与 HTTP 客户端；没有重跑浏览器渲染和点击下载的端到端测试。本轮开始核对上下文时，浏览器加载优化和 libc 扩充在其他任务进行，其完成情况不计入本轮验收。

协议依据：[RFC 8332：RSA SHA-2](https://www.rfc-editor.org/rfc/rfc8332)、[RFC 5656：ECDSA](https://www.rfc-editor.org/rfc/rfc5656)、[RFC 8308：扩展协商](https://www.rfc-editor.org/rfc/rfc8308.html)、[OpenSSH 的 SFTP 扩展说明](https://github.com/openssh/openssh-portable/blob/master/PROTOCOL)。
