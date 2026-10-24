# HTML / Streams 联合验证 — 2026-09-30 至 2026-10-01

收到“开始验证”后，三个智能体分别检查克隆/流、表单、计时器；root 整合、修复并运行真实 QEMU 浏览器。本文是本批功能的专项证据，不是全部 HTML 标准或全部仓库测试通过的声明。所有改动留在原树；未提交、未引入第三方依赖。

## 实际浏览器

正常页面与 Dedicated Worker 共 **457 项，0 失败**，实际扫描输出显示绿色结果和 `pass=457 fail=0`。驱动只发 QMP 鼠标/键盘事件，断言在 guest 的普通 HTML/JS 中执行。完整断言记录通过实际 Fetch POST `/report` 回传；主机核对各组覆盖和失败标签，不执行这些算法，也不依赖可能交错的串口行来还原断言。

| 功能组 | Window | Worker |
| --- | ---: | ---: |
| structuredClone 公共行为 | 57 | 57 |
| 表单约束与提交 | 69 | — |
| FormData / Blob / File 消费 | 28 | — |
| Streams / 文本流 / Fetch Body | 79 | 79 |
| 公共计时器 | 39 | 38 |
| 真实 required 点击、输入、Enter、按钮提交 | 11 | — |

正常 UI 检查中，首次空值按钮点击产生 trusted invalid 并拦截 submit；输入 `guest` 后 Enter 与第二次按钮点击各提交一次，真实 submitter、输入值、isTrusted 全部通过。该阶段取消导航以保留后续断言。

随后独立表单允许真实导航：submitter 的 `formaction/formmethod` 覆盖 form 的 POST；外部关联控件、重复名称、CRLF、UTF-8，以及 `formdata` 监听器的 set/append 必须出现在服务端收到的 GET 中。结果也必须到达扫描输出。独立 namespace 检查另有 **20 个唯一函数、0 个错误构造器**，不包含在 457 项中；21 个属性引用中 clipboard read/readText 指向同一个函数，按身份去重。

这里有两层容量：app 的 `target[700]` 和 HTTP 的 `URL_PATH_MAX=512`。800 字符值撞上前者；400 字符值构成的 538 字节 query 可以通过前者，但会超过后者。两次在正常版都被拒绝且服务器没有收到请求，第三次完整小提交成功。故障版两个值都实际被截到 **362 字符**，两个红结果均到达扫描输出。GET 替换 action 的 `discard=1` 查询，并保留 `location.hash === '#complete'`；fragment 不出现在 HTTP request target。

故障版本只替换真实产品链接中的对应生产文件，使用相同页面和 Worker：

| 故障 | 真实观察 | 其他功能组 |
| --- | --- | --- |
| 克隆不 detach | Window 8 + Worker 8 失败；总计 441 PASS / 16 FAIL | 全绿 |
| 忽略表单约束 | 33 项脚本约束 + 2 项真实按钮失败；总计 418 / 35 | 全绿 |
| 去掉 timer clamp | Window 4 + Worker 3 失败；总计 450 / 7 | 全绿 |
| 恢复原生 URL 截断 | 800 / 400 字符值均只送出 362；两次实际网络请求、两次红画面 | API 457 / 0 |

忽略约束时四个 invalid 回调不会发生，所以总断言数从 457 降到 453；这不是丢失 fixture。驱动逐组检查实际计数，禁止缺失 Streams 组或把其他组失败算成控制成功。对保留日志删掉整个 Streams 段、向表单控制注入克隆失败，覆盖审计均实际拒绝。Streams `ready` 提前履行的故障控制在 host 执行，79 项中恰好 1 项失败；没有把它称为 Streams guest 故障注入。

## 主机专项与回归

以下计数是各门本身的口径，包含 harness 的额外检查，不能与上表混作一套 WPT 分数。

| 门 | 正常结果 | 控制 / sanitizer |
| --- | --- | --- |
| HTML clone | 58 / 0 | detach 控制 58 / 8；ASan/UBSan 58 / 0 |
| HTML forms | 103 / 0 | 忽略约束实际让 native 和 JS 消费者失败；ASan 103 / 0 |
| 既有 forms / select | 195 / 0；39 / 0 | 事件、双向状态、native reset 控制均报红 |
| HTML timers page / worker | 51 / 0；65 / 0 | clamp 控制 5 / 4 失败；ASan/严格 UBSan |
| 既有 page runtime / timers | 45 / 0；32 / 0 | epoch、容量、receiver、checkpoint 和真实 UAF 控制报红 |
| HTML Streams | 79 / 0 | ready 控制唯一失败；ASan/严格 UBSan 79 / 0 |
| Abort / Fetch Body / 既有 stream | 43 / 0；77 / 0；62 / 0 | 各控制按预期失败；ASan/UBSan 正常 |
| Worker Fetch / 既有 Worker | 41 / 0；28 / 0 | 所属者、缺安装、pump、终止控制报红 |
| IDL shape | 57 / 0 | 缺少形状处理的控制仍报红 |
| Native operation shape | 48 / 0 | 25 项依赖该处理的检查全部报红，2 项独立 IDL 检查保持绿；ASan/严格 UBSan 48 / 0 |
| navigation base / form task ordering | 9 个场景全部通过 | 4 个基准控制精确 3/3/3/4 FAIL，2 个栈内提交控制各 2 FAIL |
| WebAPI / Headers | 234 / 0 | 原六个 Headers/XHR 控制精确 234 / 6；ASan/严格 UBSan 234 / 0 |
| HTTP URL 容量与 fragment | 43 / 0 | 截断控制精确 43 / 14；ASan/严格 UBSan 43 / 0 |
| TCP / socket TCB 生命周期 | 50 / 0 | 不 pin 控制 15 FAIL；ICMP 提前释放控制 5 FAIL；ASan/严格 UBSan 50 / 0 |
| 既有 TCP / IPv6 fallback / closed drain | 260 / 0；45 / 0；84 / 0 | 原故障控制保持击中，旧模型只补 owned 接口适配 |

macOS/Apple Silicon 不支持这些门所用的 LeakSanitizer；recipe 明确打印不可用，同时执行 ASan/UBSan。这些结果没有证明无泄漏。

## 验证发现与修复

- FormData 工厂少一个方法分隔符，导致 bootstrap SyntaxError，并连带使 Fetch 未安装。保留异常栈诊断后修复。
- 表单缓存的 JSValue 原先在 JS runtime 释放后才清理。清理已交给 page owner，在释放 context/runtime 前执行。
- Document 事件处理器新增 invalid/formdata 后，IDL 形状的手抄名单漏项。现在名单来自 DOM 的同一权威表。
- WebIDL data 属性变为 getter 后，20 个已有 namespace 方法漏过不可构造检查。IDL 现在将真实原值以私有 record 对象交给一次 quiet walk，不公开额外 global、不执行 getter、不改变作者函数。首版 private Array 仍然 48 / 2：QuickJS 的 quiet descriptor API 不读取 packed Array 元素；改为 null-prototype 对象的 own numeric slots 后 48 / 0，保留这一静默漏种子的陷阱注释。
- QuickJS 接受 `v` flag 并不代表实现 Unicode sets。实际 intersection/subtraction/string-set 等检查发现别名行为，pattern 验证继续明确不可用。
- `-2147483648.5` 转换触发 QuickJS 的 signed negation UB；共享 timer 转换改用定义良好的截断、模运算和转换，保留 Symbol/BigInt/异常拒绝。
- Streams 实际失败包括 author `.call/.apply` 干扰、迭代选项异常漏锁、HWM=0 下关闭源卡住、结束读取与错误传播、已派发 write 的 shutdown 顺序、transform erroring 后仍执行、TextEncoderStream 转换以及 pipeThrough readable 锁检查。79 项回归验证这些边界。
- 新 native reset 绕过了旧的 JS Map reset 控制；控制已接到实际 native 消费者，并观察其报红。
- 导航装置此前把“已画出页面”当作“JS runtime 已完整安装”，过早输入/关闭取消了加载。现在等待实际 complete 生命周期和表单 API，不按预期 URL 等待，原 URL 断言保持严格。
- 旧 WebAPI fixture 将合法相对 URL `::::not a url` 当作非法输入，随后 fs_reset 复用了仍在请求中的假 socket，污染 POST 的五项检查。换成真实非法 absolute IPv6 host；保留原 TypeError 要求和六个 Headers 控制，更新已增长的总检查数。
- 原 native GET 把 8192 字节 entry buffer 填入 700 字节 URL 时静默截断。现在保留有界导航实现，超限在排队前明确拒绝；真实 wire 故障版必须证明部分值确实被送出，正常版必须留在源页且不发请求。
- 下游 HTTP splitter 的 512 字节 path 也曾截断并返回成功。所有 bounded parse/resolve/render 在容量不足时返回失败；保留原有简单 HTTP 语法，不把它称作完整 WHATWG URL 实现。导航保存 fragment，transport parse 在 `#` 前结束路径。
- TCP broker 原先保存 unowned TCB 的裸 index；RST/ICMP 提前释放后，另一个 dial 复用槽，旧 socket 清理会关闭新 SYN。真实 `tcp.c+sock.c` 检查观察到新 SYN-ACK 得到 RST；在发 SYN 前 pin 所有 IPv4/IPv6 open，并修 ICMP 的 owned 槽释放出口。旧 socket 保留原槽，新 dial 使用其他槽且正确 ACK。正常 FIN/ACK 及正式 close 后安全复用也实测通过。
- 结果装置两处失真已纠正：多核 kernel printf 插入串口曾把真实 457 / 0 误读为 forms 47 / 69，改以完整网络报告核对；两个故障请求截断后 URL 相同会复用缓存，增加位于截断之前的不同字段，强制两次独立 wire 观察。原失败记录没有删除。

## 保留的失败与限制

第一次表单故障 guest 在 `/streams.js` 到达 HTTP handler 前出现 socket 错误，页面以 `pass=0 fail=1` 提前退出；此运行不是有效控制证据，原 serial/requests 留存。重跑完整观察到 418/35。

取消机制 guest 的故障控制实际观察到 10 PASS + 唯一依赖状态排序失败；正常版首次有 `/hold` dial 在 timeout 前返回 `SOCK_E_CONN`，当时 `timed.aborted=false` 且服务器没有收到 `/hold`。重跑 11 / 0，并保存虚拟网络 pcap。未发现新增 JS generation/owner 关闭错误连接的证据，不能把 ABI 的宽泛错误码直接解释为服务器 backlog 或 ECONNREFUSED。

后续完整门又观察到资源连接失败。`guest-navigation-1790819679/network.pcap` 的 timers.js dial 为 guest SYN → peer SYN-ACK → guest RST，无 final ACK/GET：这条证据把拒绝定位到 guest 丢失了匹配 TCP 状态，不能归咎对端没有监听。上述真实生命周期缺陷已修复，但 controlled RST/ICMP 复现不等于唯一证明该次无 peer RST 故障的完整因果链；正常 FIN 对照没有复现同样问题。保留这一区分，不发布“所有偶发网络失败消失”的结论。DNS TCP fallback 与旧 blocking HTTP 仍有 unowned index 调用链，未在没有本次调用证据的情况下扩大修改。

默认流已验证；byte streams/BYOB、可转移流/port、完整 Worker 消息类型序列化、Unicode-set pattern、日期时间控件、原生 POST/dialog、文件选择器、完整校验气泡仍未实现。字符串 timer handler 因 TrustedScript/CSP 缺失而明确拒绝。原生 HTTP URL 仍是有限 splitter，未声称完整相对 URL 标准算法。未运行全部 WPT、全部 ci-host/ci-boot，未发布性能提升数值。

## 复现入口

```sh
make -j6 BUILD=build-html-verify all build-html-verify/disk.img
make BUILD=build-html-verify test-html-priority-guest
make BUILD=build-html-verify test-abort-guest
make BUILD=build-html-verify test-mk-wired
```

guest 门以 prerequisite 串联克隆、表单、timer、native GET 截断控制再运行正常版，Streams host 控制也是 prerequisite。QEMU 使用 snapshot，不回写验收磁盘中的用户状态。普通 HTTP fixture 不按站点名选择生产行为；未修改活动服务或其他用户磁盘。

最终含 TCP 修复的内核构建和上述两个 guest 门均返回 **0**。接线检查为 **414 fragments / 413 reachable / 1 declared**，`git diff --check` 通过。完整门本轮通过并不证明所有历史间歇网络失败已经唯一归因或永不再现。

## 最终证据

- [当前实际画面与 457/0](../../build-html-verify/html-priority/guest-current-1790824354/screen.png)、[完整 guest 网络报告](../../build-html-verify/html-priority/guest-current-1790824354/report.json)。
- [真实 GET 结果画面](../../build-html-verify/html-priority/guest-current-1790824354/navigation.png)、[服务端实际条目](../../build-html-verify/html-priority/guest-current-1790824354/navigation.json)、[两层实际截断控制](../../build-html-verify/html-priority/guest-navigation-1790824316/navigation.json)。
- [五轮最终 guest 结果与覆盖](../../build-html-verify/html-priority/latest-runs.json)、[完整 guest 门日志](../../build-html-verify/html-priority/host-logs/logitos-html-kernel-guest.log)、[取消回归日志](../../build-html-verify/html-priority/host-logs/logitos-abort-kernel-final.log)。
- [TCP 生命周期正控](../../build-html-tcb/sock-tcb/current.log)、[unowned 负控](../../build-html-tcb/sock-tcb/unowned.log)、[ICMP 负控](../../build-html-tcb/sock-tcb/icmp.log)。
- [源码与构建哈希 manifest](../../build-html-verify/html-priority/manifest.json)。从实际 LogitFS 镜像读取 `/browser.aex`，字节与最终链接产物一致；manifest 同时记录新 ISO、ELF、磁盘和源码哈希。

这些路径是保留的 scratch 构建证据；清理该构建树会移除图片和日志，但复现入口及测试源码保留在原树。
