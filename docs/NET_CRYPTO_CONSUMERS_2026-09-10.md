# net 与 crypto 的实际消费者（2026-09-10）

2026-09-10 第二轮更正：自动下载统一到 `/download`，保存上限提高到 64 MiB，并新增浏览器下载与实际 WebCrypto 密钥操作。下文保留第一轮验收记录；当前能力和新证据见 [第二轮报告](NET_DOWNLOAD_KEYS_2026-09-10.md)。

本轮交付是可运行的 `/bin/net` 和 TLS 接线。构建与验收使用独立的
`build-net-consumers`，源码在当前共享工作树；未把已有的大量未提交改动当成本轮成果。

## 同时进行的工作

通过 Codex 任务状态和最新上下文复核，除本任务外，system 还有两条活跃工作线：

- 浏览器：Cookie、请求上下文及 DeepSeek 登录流程；最新进展正在查用户协议文字重叠、
  短信请求是否真正发出和二维码显示。验证码窗口关闭不能作为短信发送成功的证据。
- 输入法：任务范围为 kernel/lib/ime，目标是修复输入法并接通使用路径；任务仍显示运行中，
  没有取得可据此宣称完成的最新验收输出。

这份状态是本次读取的快照。本轮实现集中在 net/TLS、net 的构建规则及对应测试。

## 消费者与用途

| 原有实现 | 新消费路径 | 解决的问题 |
|---|---|---|
| X448 | TLS 1.2/1.3 客户端、TLS 1.3 服务端的组协商、密钥生成及共享秘密计算 | 可以与只选择 X448 的 TLS 对端交换真实应用数据 |
| BLAKE3 | `net checksum`、`net verify`、`net get`、`net save` | 检查本地文件、管道输入和下载内容，默认 256 位摘要 |
| BLAKE2b | 同上 | 兼容发布方提供的完整 BLAKE2b-512 校验值 |
| SHA-256/SHA-512 | 与上述算法共用文件和下载适配层 | 使用常见发布校验值完成下载、比较、保存和回读 |

这些路径调用仓库原有的密码实现。无密钥摘要用于内容一致性；它们不建立发布者身份，
也没有被接入登录口令或包签名信任策略。已有的其他 crypto 消费路径继续保留。

## 使用方法

```text
net checksum blake3 /tmp/model.bin
net checksum blake2b /tmp/one.bin /tmp/two.bin
net checksum sha256 -
net verify sha256 EXPECTED_HEX /tmp/model.bin
net get https://example.org/model.bin
net get https://example.org/model.bin blake3 EXPECTED_HEX
net save https://example.org/model.bin /tmp/model.bin sha256 EXPECTED_HEX
```

`EXPECTED_HEX` 替换成完整期望摘要。SHA-256/BLAKE3 为 64 个十六进制字符，
SHA-512/BLAKE2b 为 128 个，大小写都可。退出码：0 成功，1 参数/传输/存储错误，
2 摘要不匹配。未指定算法的 `net get URL` 输出 SHA-256、完整字节数及兼容旧脚本的 FNV-1a。

`net save` 先收齐、确认 HTTP 成功和摘要匹配，再创建或覆盖目标文件；写完后重新打开文件校验。
传输失败、404、206、正文截断及摘要不匹配均不会进入目标文件写入步骤。
底层整体写文件接口不提供原子替换：实际存储写入失败时可能已改变目标，命令返回失败。

## net 行为变化及边界

- 旧 `net get` 使用全局 HTTP syscall 和 128 KiB 固定缓冲；现在每个进程经 socket syscall
  使用仓库 HTTP/1.1 状态机，按响应结束条件取得完整正文并增量计算摘要。
- 支持 Content-Length、chunked 和以 EOF 结束的正文；支持最多 5 次重定向，修正相对路径
  和仅查询参数的 Location 解析，并拒绝 HTTPS 降为 HTTP。
- 明确拒绝 URL 字段截断、无效端口、userinfo、不支持的 scheme 和 IPv6 字面地址。
  此项不代表整个网络栈不支持 IPv6。
- 下载计算摘要上限 64 MiB；为保证写入前已校验，`save` 先缓存在内存，上限 8 MiB。
  整个连接及所有重定向共用 60 秒期限。本地文件校验按 4 KiB 读取，不受下载容量上限约束。
- 请求 `Accept-Encoding: identity`，拒绝未支持的内容编码。未为这条 CLI 路径链接替代解压实现。
  TLS 要求 HTTP/1.1 ALPN；这里没有新建 HTTP/2 下载器或续传功能。
- `ping`/`dns` 超时改用单调时间，避免 RTC 调整或跨午夜影响截止时间。

X448 在现有组列表之后作为兼容选项，不改变默认混合密钥交换/X25519 首次 key share。
TLS 1.3 需要它时经 HelloRetryRequest 生成；客户端、服务端均使用原有 `x448.c`。
长度使用完整 56 字节，TLS 层拒绝全零共享秘密；TLS 1.2 的 premaster 缓冲同步扩大到
`TLS_KX_SS_MAX`，避免沿用只容纳 P-384 的 48 字节长度。
协议依据：[RFC 8422](https://www.rfc-editor.org/rfc/rfc8422.html)、
[RFC 8446 §7.4.2](https://www.rfc-editor.org/rfc/rfc8446.html#section-7.4.2)。

## 已完成的验证

| 层次 | 实测结果 | 证据位置（相对 build-net-consumers） |
|---|---|---|
| 宿主机真实 CLI、HTTP 解析及密码源码，系统调用边界替换为 POSIX | ASan/UBSan 下 80 项命令检查通过 | `gates.log` |
| 摘要负对照 | 改坏摘要输出后，本地校验、HTTP 校验、保存均明确拒绝 | `gates.log` |
| OpenSSL 互操作矩阵（1.2/1.3 客户端、1.3 服务端） | 52 个组合通过、0 失败；6 个 TLS 1.2 混合组组合不作支持声明 | `tls-full.log`、`tls-full/evidence/` |
| X448 针对性对照 | 正常 24 个组合通过；关闭 X448 后正好 12 个 X448 失败，12 个 X25519 仍通过 | `tls-verified.log`、`tls-verified/` |
| 普通信任库的真实系统镜像 | 17 项通过，包含精确文件内容、分块传输和失败保留原文件 | `net-guest/serial.log`、`net-guest/result.json` |
| 同一客体的旧/新 net 对照 | 300,123 字节资源：旧版仅 131,072，新版完整 300,123 | `net-guest/serial.log`、`net-guest/artifacts.json` |
| 独立测试 CA 的真实 TLS 镜像 | 18 项通过，新增 X448 TLS 1.3 HTTPS 下载、证书校验及文件校验 | `net-guest-tls/serial.log`、`net-guest-tls/result.json` |
| 构建及门禁接线 | 内核 ISO、包含真实 net.aex 的磁盘构建成功；test-mk-wired 通过 | `full-build.log`、`rebuild.log` |

客体验收配置为 BIOS、512 MiB、4 vCPU、e1000。HTTPS 测试使用本机回环服务和临时 CA，
只替换独立测试镜像的 roots 对象，不修改正式根证书包、普通内核或普通 ISO。
这证明真实客体的握手、证书校验与下载链路；不等于已遍历公网服务或全部硬件。

互操作装置本身也修正了一个假成功来源：服务端期待 64 字节，而客户端只发送 19 字节；
现在核对双方退出状态及精确回显正文，服务端超时不能再被客户端成功状态掩盖。

## 重跑

```sh
make -j6 BUILD=build-net-consumers build-net-consumers/logit.iso build-net-consumers/disk.img
make BUILD=build-net-consumers test-net-consumers test-mk-wired
OPENSSL=/opt/homebrew/opt/openssl@3/bin/openssl make BUILD=build-net-consumers test-tls-x448
OPENSSL=/opt/homebrew/opt/openssl@3/bin/openssl make BUILD=build-net-consumers test-tls-matrix
make BUILD=build-net-consumers test-net-consumers-os
OPENSSL=/opt/homebrew/opt/openssl@3/bin/openssl make BUILD=build-net-consumers test-net-consumers-tls-os
```

OpenSSL 路径按机器调整，互操作需要支持这些组及套件的 OpenSSL。
新的宿主机门禁已接入 `ci-host`，两个客体门禁接入 `ci-boot`；摘要/X448 负对照均为正门禁前置。
完整 TLS 矩阵本次单独指定 `BUILD=.../tls-full` 和 `TLS_MATRIX_PORT=15846` 运行，
以保留每组双方日志及回显。旧版 net ELF 从本轮修改前保存的源码构建；重跑前后对照可以给
`tests/boot/run-net-consumers.py` 传 `--before-net build-net-consumers/net-before.elf`。
