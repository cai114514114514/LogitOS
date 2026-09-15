# 下载、WebCrypto 密钥与 net 回归检查（第二轮）

本轮接续 `NET_CRYPTO_CONSUMERS_2026-09-10.md`。工作目录是共享脏树，构建与验证使用 `BUILD=build-net-round2`。浏览器 iframe／Cookie／登录和内核／输入法任务仍在并行推进；本报告只认下面记录的产物与测试，不把整棵工作树的改动归为本轮成果。

## 直接使用

浏览器现在处理服务器 `Content-Disposition: attachment`、常见二进制响应、同源 `<a download>`、网页生成的 Blob／data URL。文件统一保存到 **`/download`**，同名文件自动编号。按 Ctrl+J 查看下载记录。

命令行也使用相同目录和文件名规则：

```sh
net download https://example.org/file.zip
net download https://example.org/file.zip sha256 EXPECTED_HEX
net checksum sha256 /download/file.zip
net verify sha256 EXPECTED_HEX /download/file.zip
```

`EXPECTED_HEX` 换成发布方提供的完整摘要；也支持 sha512、blake2b、blake3。显式指定路径的 `net save URL PATH ALG HEX` 仍会覆盖 PATH，且必须提供期望摘要。自动下载先完成传输与校验，再提交临时文件；失败不发布不完整的最终文件。

新增可用页面 `examples/browser/signed-report.html`：输入交付记录，生成 SHA-256 摘要、Ed25519 签名和公钥，下载 JSON；选择收到的 JSON 后可以验证内容。私钥不可导出，不写入文件。公钥来源仍需另行核对，文件自带公钥并不证明发送者身份。

在宿主机运行：

```sh
python3 -m http.server 8877 --bind 127.0.0.1 --directory examples/browser
```

LogitOS 的 QEMU 浏览器打开 `http://10.0.2.2:8877/signed-report.html`；宿主浏览器可用 `http://127.0.0.1:8877/signed-report.html`。这是实际使用 WebCrypto 和浏览器下载接口的示例页面，独立于测试向量。

## 补齐的密钥操作

| 能力 | 本轮实际实现 |
|---|---|
| 生成密钥 | AES／HMAC、Ed25519、X25519、ECDSA／ECDH 的 P-256、P-384、P-521 |
| 签名与验证 | Ed25519、ECDSA 配合 SHA-256／384／512；复用现有 native crypto |
| 密钥协商 | X25519、ECDH 三条 NIST 曲线；deriveBits 与派生 AES／HMAC 密钥 |
| 导入／导出 | EC／OKP 的 raw、JWK、SPKI、PKCS8；核对私钥、公钥和曲线；有界 DER 解析 |
| 授权边界 | 内部槽位决定 usages 与 extractable；修改公开属性不能增加权限 |
| 随机数 | generateKey 和 getRandomValues 使用强随机源；源不可用时报错，移除时钟 PRNG 回退 |

Ed25519 JWK 接受 `EdDSA` 和当前 `Ed25519` 标识。参考：[Web Cryptography Level 2](https://www.w3.org/TR/WebCryptoAPI/)、[RFC 9864](https://www.rfc-editor.org/rfc/rfc9864.html)。下载规则参考 [HTML 下载算法](https://html.spec.whatwg.org/multipage/links.html#downloading-resources)。

## 找到并修正的实际缺口

- 原下载分支在 `js_page_close`／`dom_free` 后才决定留在当前页，造成画面看似保留但页面脚本已失效。现在先确定响应类型，附件不销毁原页面；普通文档只取一次响应。
- 缓存只存响应体，附件命中缓存后丢失类型和文件名。现在元数据跟随同一个 URL／Cookie 缓存项，覆盖条目时清除旧元数据；保留普通导航缓存。
- 原页面占满 16 个请求槽时，新导航无法开始。增加独立的第 17 个导航槽，子资源容量保持 16。
- 下载不再提前增加导航历史条目；写入后核对完整字节，通过临时目录和拒绝覆盖的 rename 提交。
- 同名下载的客体失败：`read_file(path, buffer, 1)` 对较大文件返回错误，不能用来判断不存在。增加独立 exists 适配，宿主测试也改为客体的完整读取契约。
- 实际文件系统目录项只有 60 字节；自动命名为碰撞编号保留空间，UTF-8 截断不切断码点。`filename*` 支持 UTF-8 和语言字段。
- 下载队列的消费点必须在正常事件循环中，不能落在负对照的条件编译分支里；真实点击／Blob 客体测试覆盖此接线。
- 整合新 native CryptoKey 回调时，修正固定长度回调数组，调用与释放数量改为从数组推导。
- 共享树构建补上 `css_vars.c` 的字符串函数声明，并从内核源码集合排除使用 POSIX API 的 `c/lib/agent` 用户态库；未改其功能实现。

## 检查与证据

| 检查 | 结果 | 本轮日志／产物，位于 build-net-round2 |
|---|---|---|
| TCP、IPv4、DHCP、IPv6、DNS、HTTP/2、关闭后接收缓冲 | 基线均通过：241／50／16／148／119／51／42／29／168／84 项 | `net-baseline.log` |
| 新密钥 API | 74 项通过，独立 Node/OpenSSL 密钥、签名、DER、共享秘密参考值 | `consumer-gates.log`、`final-integration-host.log` |
| 原有 WebCrypto 操作 | 152 项参考向量与拒绝检查保持通过 | `consumer-gates.log` |
| 自动保存 | ASan／UBSan 50 项通过；禁用提交的负对照触发 40 项失败 | `final-integration-host.log` |
| CLI 下载 | 86 项命令检查通过；包含 9 MiB、空文件、同名、失败保留目的文件 | `consumer-gates.log`、`history-consumer-checks.log` |
| 浏览器集成 | loader ASan、HTTP/2 168、缓存失效 652、正文边界 6、加载调度通过 | `final-integration-host.log` |
| 密码库全面功能门禁 | CRC32C、BLAKE2b/s/3、cSHAKE/KMAC、scrypt、Argon2、XChaCha、GCM-SIV、X448、secp256k1、ML-DSA、ML-KEM、ECDSA、P-521 通过；差分 140,214 项零失败 | `crypto-inventory-openssl3.log` |
| 普通客体 CLI | 21 项通过，包括 9 MiB 逐字节核对 | `net-guest/result.json`、`net-guest/serial.log` |
| HTTPS 客体 CLI | 22 项通过，独立测试 CA、TLS 1.3 X448、正常证书校验 | `net-guest-tls/result.json`、`net-guest-tls/serial.log` |
| 浏览器真实客体 | 74 项密钥检查；两次原生附件点击只请求网络一次；Blob、空文件、download 属性、旧页面计时器；示例页面原生签名按钮 | `download-keys-guest/result.json`、`serial.log`、`example.png` |
| 真实磁盘与独立验证 | 退出 QEMU 后读取私有可写 LogitFS 磁盘，核对实际文件字节；独立 Ed25519 实现验证两个导出的 JSON 签名 | `download-keys-guest/result.json`、`disk.img` |
| 根证书 | 编译包含 130 项；来源账本 88 项、历史例外 42 项 | `check-roots`、`tools/roots/check_provenance.py` |

负对照是正门禁的前置。日志中的预期 FAIL 不代表正常实现失败。最初系统 `/usr/bin/openssl` 是 LibreSSL，P-521 门禁明确拒绝该环境；改用 `/opt/homebrew/opt/openssl@3/bin/openssl` 后整组通过，没有放宽门禁。

客体启动最初出现鼠标未就绪问题，在旧内核也观察到。测试改为等待实际 Finder 窗口和指针发布后再点击；最终浏览器验收使用当前 `build-net-round2/logit.iso`，不是旧内核替代。启动时序的失败不能据此归为 net 回归。最终结果保存了 ISO／磁盘／browser.aex 的哈希，运行前后相同。

门禁：`make BUILD=build-net-round2 test-download-keys-os` 已接入 ci-boot。普通／TLS CLI 客体门禁仍使用 `test-net-consumers-os`／`test-net-consumers-tls-os`。

## 全部检查后的剩余范围

| 范围 | 现状与下一步 |
|---|---|
| RSA | TLS／X.509 已有验签消费者；浏览器 RSA 密钥及私钥运算仍缺少完整后端，本轮未伪装支持 |
| 其他 AES 模式、wrapKey／unwrapKey | 生成／保存密钥材料不等于已有加解密；浏览器实际加解密仍是 AES-GCM，128 位 tag |
| SHA-1 HMAC、Ed448、浏览器 X448 | 尚未接入 WebCrypto；X448 的实用消费者是上一轮补齐的 TLS |
| BLAKE2s、cSHAKE／KMAC、scrypt／Argon2、XChaCha／GCM-SIV、secp256k1、ML-DSA | 功能向量通过，但仍缺直接应用消费者；静态调用清单在 `crypto-consumer-inventory.json`，不能把测试或导出符号当产品消费者 |
| 根证书信任 | 130 项可编译不等于信任策略全部现代化；42 项历史例外保留来源账本，本轮没有盲目增加信任或关闭验证 |
| 大文件 | HTTP 下载上限 64 MiB；保存仍在内存缓冲后写盘，并非无限流式落盘。普通文档解析维持 16 MiB；Blob URL 表仍有原来的 8 MiB 总配额 |
| 下载体验 | 队列有界、同名编号、完成历史；尚无断点续传、取消或跨重启恢复下载队列 |
| 网络覆盖 | 本轮是协议门禁、可控真实客体 HTTP／HTTPS 与可用消费者验证；不等于任意公网网站、证书链与登录流程全部通过 |

用户可继续使用原有示例／训练／其他工作区；本轮没有提交或清理共享树中的其他改动。
