# LogitOS 网络与 socket bug 清单（2026-09-09）

本轮追加 **9 项缺陷：3 项 P1、6 项 P2**，与 [上一轮 13 项清单](BUG_REVIEW_2026-09-09.md) 不重复。P1 优先处理内核内存边界、阻塞进度和传输可靠性；P2 为功能与协议兼容性错误。优先级是修复排序，不代表已证明可利用性。

审查对象为 `fe748e31e5d236cd74c1ca49f896b18170c7d54d` 加当前未提交工作树。三个子代理分别检查 TCP/UDP、DNS/DHCP、HTTP/WebSocket，主代理检查 socket/libc 调用链并复核结果。实现文件未修改；本轮新增此报告，探针在 `/tmp/logitos-netreview-20260909/`，构建使用命令行 `BUILD=build-netreview-*` 隔离。

**证据边界：**6 项为实际协议源码的主机状态机/帧复现，1 项为实际 libc 源码加模拟 syscall 返回值复现，1 项为实际 UNIX socket 队列状态加等待谓词核查，1 项为静态长度计算与调用链核查。没有 guest、真实网卡或真实服务器的端到端复现。探针使用内存传输层与虚拟时钟，没有向外部服务发包。本轮所有子代理正常完成。

| ID | 优先级 | 缺陷 | 证据 |
|---|---|---|---|
| N01 | P1 | TCP 重传超时后拒绝有效累计 ACK | 实际 TCP 状态机主机复现 |
| N02 | P1 | UDP 长度窄化导致接收越界读取风险、发送虚报长度 | 静态调用链 |
| N03 | P1 | UNIX record socket 满队列的等待条件错误，可能忙循环 | 实际队列状态与生产等待谓词核查 |
| N04 | P2 | TCP 写半关闭后安静超过 2 秒，接收方向也被终止 | 实际 TCP 状态机主机复现 |
| N05 | P2 | libc 把 DNS 失败返回成成功解析到 255.255.255.255 | 实际 resolver，模拟 syscall 返回值 |
| N06 | P2 | DNS 别名缓存忽略 CNAME 自身 TTL | 实际 DNS 状态机主机复现 |
| N07 | P2 | DHCP 租约过期后仍保留旧 IP 配置 | 实际 DHCP 状态机主机复现 |
| N08 | P2 | WebSocket 空帧在接收块边界不能完成 | 实际解析器主机复现 |
| N09 | P2 | HTTP/2 跨帧请求头、零请求体时漏发 END_STREAM | 实际输出帧主机复现 |

## N01 — TCP 重传后将已经发出的字节判为从未发送

位置：[tcp.c:1422](/Users/wangzhe/system/LogitOS/c/net/transport/tcp.c:1422)、[tcp.c:1434](/Users/wangzhe/system/LogitOS/c/net/transport/tcp.c:1434)、[tcp.c:1659](/Users/wangzhe/system/LogitOS/c/net/transport/tcp.c:1659)。

普通场景：客户端发送多个段，ACK 延迟或丢失触发一次 RTO，随后收到确认此前全部已发送数据的累计 ACK。RTO 将 `snd_nxt` 回退到重传位置；ACK 路径却仍用它作为已发送序号上界，拒绝超过该值的 ACK。另一个字段 `snd_max` 已经保存真正的最高已发送位置。

使用实际握手、正确校验和和既有测试报文生成器，只改变 ACK 前是否发生 RTO：

```text
DELAYED_ACK timeout=0 queued=4380 original_sent=4380 next_before=4380 accepted=4380 timer=0
DELAYED_ACK timeout=1 queued=4380 original_sent=4380 next_before=1460 accepted=0 timer=1
```

合法 ACK 没有推进发送进度，重传计时继续运行，可能造成重复重传和连接延迟；这里不声称每次都会永久卡死。建议 ACK 合法上界与重传游标分离，核查 ACK 推进后 `snd_nxt` 的恢复。源码旧注释“acknowledges bytes never sent”应保留并说明在 RTO 后不成立的原因。

## N02 — UDP 在比较或传参时缩成 16 位，实际长度仍是 int

位置：[udp.c:104](/Users/wangzhe/system/LogitOS/c/net/transport/udp.c:104)、[lsock.c:568](/Users/wangzhe/system/LogitOS/c/net/core/lsock.c:568)、[syscall.c:1071](/Users/wangzhe/system/LogitOS/c/kernel/exec/syscall.c:1071)。

接收端使用 `queued_len > (uint16_t)max ? max : queued_len`。应用传入一个正常的 65,536 字节接收缓冲时，比较用的容量变成 0；若队列中的数据报非空，复制长度却选择原始的 65,536，超过该数据报存储范围。用户缓冲范围检查不等于内核复制源范围检查。

发送端也直接把 `int len` 转为 `uint16_t`，成功后返回原始 `len`。例如超过支持上限的 65,536 字节请求窄化为零长度，底层成功时上层却报告全部发送。正确行为应是明确拒绝不支持的长度，而非模 65,536 截断。

这是同一长度约束不一致问题的两个入口，合并计数。建议接收按完整宽度求安全最小值，发送在窄化前检查协议及后端支持上限。**仅静态核实；没有执行越界复制，没有据此声称已证明数据泄漏或 guest 崩溃。**

## N03 — UNIX record socket 的写入条件与等待条件不一致

位置：[unix.c:275](/Users/wangzhe/system/LogitOS/c/net/core/unix.c:275)、[unix.c:600](/Users/wangzhe/system/LogitOS/c/net/core/unix.c:600)、[unix.c:620](/Users/wangzhe/system/LogitOS/c/net/core/unix.c:620)、[wait.h:132](/Users/wangzhe/system/LogitOS/c/kernel/core/wait.h:132)。

record 写入要求同时满足记录槽未满、剩余字节足够容纳整个 record。连接式 DGRAM/SEQPACKET 的等待谓词却只检查还有任意字节空间；按路径发送的 DGRAM 则只检查记录槽。正常生产者快于消费者即可触发“不允许写，但等待条件已满足”，循环立即重试。

安全探针调用实际非阻塞接口构造队列状态，再核对生产源码谓词，未调用会忙循环的阻塞写：

```text
record_slots_full: write=-2 bytes=32 records=32 blocking_predicate=1
whole_record_no_room: write=-2 free_bytes=100 blocking_predicate=1
after_peer_drains: write=200
datagram_bytes_full: write=-2 bytes=4096 records=1 blocking_predicate=1
outstanding_allocations=0
```

`-2` 是该层 EAGAIN；非阻塞拒绝本身正确。错误在于同一状态走阻塞路径时，`wait_event` 不会进入 `sched_block_self_unlock`。结合 BKL 调度约束，存在持续占用内核、阻止接收者进入内核释放队列的风险。**队列状态已实测；忙循环和 guest 调度后果为调用链推导，未执行 guest 卡死复现。**

建议共享完整的 record 可写判断，供写入、等待和中断退出条件使用；保留 stream 可以部分写入的语义。不能仅将两个错误谓词互换。

## N04 — SHUT_WR 后 2 秒安静期误杀仍需接收的连接

位置：[tcp.c:1682](/Users/wangzhe/system/LogitOS/c/net/transport/tcp.c:1682)、[tcp.c:2167](/Users/wangzhe/system/LogitOS/c/net/transport/tcp.c:2167)、[lsock.c:503](/Users/wangzhe/system/LogitOS/c/net/core/lsock.c:503)。

客户端发完请求调用 `shutdown(SHUT_WR)`，服务端确认 FIN 后处理任务，再返回响应，是普通半关闭用法。实现将该连接放入与整体 close 相同的 FIN_WAIT 回收策略；对端安静超过 2 秒就 `conn_closed()`，即使应用仍持有 socket、对端未发 FIN。

```text
HALF_CLOSE quiet_ticks=100 alive_before_reply=1 recv=8 peer_fin=0
HALF_CLOSE quiet_ticks=300 alive_before_reply=0 recv=-1 peer_fin=0
```

虚拟时钟为 100 Hz。1 秒后响应可读取，3 秒后响应无法读取。保留旧决策：回收注释为避免浏览器连接槽泄漏；但 `tcp_shutdown_write` 注释明确承诺保留接收，该回收策略不能直接套用到仍由应用持有的半关闭连接。建议区分 close 后待回收连接与 SHUT_WR 连接，而非只延长固定超时。

## N05 — DNS 失败值跨 ABI 符号扩展后未被 libc 识别

位置：[netdb.c:28](/Users/wangzhe/system/LogitOS/c/apps/libc/src/netdb.c:28)、[syscall.c:843](/Users/wangzhe/system/LogitOS/c/kernel/exec/syscall.c:843)。

内核把 `dns_result()` 的失败值 `0xFFFFFFFF` 经 `(int)`、`(long)` 扩展为 64 位 `-1`。libc 却将返回值转换为 `unsigned long` 后与 `0xFFFFFFFFul` 比较；64 位全 1 不等于低 32 位全 1，于是把失败当成功，并截断出地址 `255.255.255.255`。`getaddrinfo` 和共用 resolver 的 `gethostbyname` 均受影响。

实际 `netdb.c` 探针仅将 x86 `int 0x80` 包装替换成返回指定值的 syscall 接缝，保持解析分支不变；正常返回值作对照：

```text
success_control: rc=0 ip=01020304
dns_failure: rc=0 expected=-2 ip=ffffffff
exit=1
```

该探针确实以失败退出，预期是 `EAI_NONAME`。这不是实际 DNS 网络查询测试；内核返回值来源由静态调用链验证。建议统一 ABI 的无符号 IPv4/失败值表示，并同步所有调用者；若在 libc 规范化，先显式转换至协议规定宽度再判断。

## N06 — CNAME 的短 TTL 被最终地址的长 TTL 覆盖

位置：[dns.c:430](/Users/wangzhe/system/LogitOS/c/net/dns/dns.c:430)、[dns.c:447](/Users/wangzhe/system/LogitOS/c/net/dns/dns.c:447)、[dns.c:459](/Users/wangzhe/system/LogitOS/c/net/dns/dns.c:459)。

跟随 CNAME 只改目标名，未更新 `min_ttl`；A/AAAA 才更新缓存期限，最终地址却以原始别名缓存。普通回答为 `alias.test CNAME real.test TTL=30`、`real.test A 192.0.2.10 TTL=3600`：

```text
cname ttl_alias=30 ttl_address=3600 cached_seconds=3600
after_seconds=31 extra_queries=0 done=1
```

别名已过期仍直接命中，DNS 切换或故障转移可能长时间沿用旧目标。[RFC 1035 §4.1.3](https://www.rfc-editor.org/rfc/rfc1035.html#section-4.1.3) 规定 TTL 是该资源记录允许缓存的时间。30 秒测试值避开代码明确声明的 5 秒 floor，这一问题不能归因于该既有取舍。建议最终别名缓存期限覆盖所依赖的完整 CNAME 链。

## N07 — DHCP 重试期间没有维护租约到期边界

位置：[dhcp.c:143](/Users/wangzhe/system/LogitOS/c/net/core/dhcp.c:143)、[dhcp.c:217](/Users/wangzhe/system/LogitOS/c/net/core/dhcp.c:217)。

取得 lease 后只保存半程续租时间 `renew_at`，没有保存 expiry。续租失败进入 DISCOVER，旧 `net_cfg.ip/mask/gw` 仍保留。实际状态机先取得 10 秒租约，再模拟 DHCP 服务暂时不可达，推进至 25 秒：

```text
initial_result=0 lease_seconds=10 elapsed_ticks=2500
current_ip=0a00020f initial_ip=0a00020f state=1 discovers=17 requests=6
```

已重新 SELECTING，配置仍是原地址。若服务器回收后将其分配给其他设备，存在地址冲突风险；没有实际制造地址冲突。[RFC 2131 §4.4.5](https://www.rfc-editor.org/rfc/rfc2131.html#section-4.4.5) 要求租约到期后停止使用旧地址。

保留并纠正旧注释“dead server must not take down a working config”：服务器短暂失联时保留尚未过期的租约有依据，但不能据此无限使用已过期地址。建议明确维护 T1/T2/expiry，过期撤销租约配置，继续获取新租约。发送路径已有 route_sync，本轮未把“路由必然不刷新”计为缺陷。

## N08 — WebSocket 空 Ping 在块边界停留为 AGAIN

位置：[ws.c:136](/Users/wangzhe/system/LogitOS/c/net/http/ws.c:136)、[ws.c:160](/Users/wangzhe/system/LogitOS/c/net/http/ws.c:160)、[js_websocket.c:623](/Users/wangzhe/system/LogitOS/c/apps/browser/js_websocket.c:623)。

零 payload 帧的头部读完后 `continue`，外层循环因输入耗尽退出，没进入不需要额外字节的 PAYLOAD 完成分支。浏览器消费者收到 AGAIN 也退出。单独到达的空 Ping 可能一直不触发 Pong；空消息与空 Close 有相同完成边界。

```text
WS ping payload=1 consumed=3 status=1 expect_frame=1
WS ping payload=0 consumed=2 status=0 expect_frame=1
  empty_feed_status=0
WS empty_ping_plus_next_frame consumed=2 status=1
```

同批有后续字节时正确；额外调用零长度 feed 仍不能完成。因此问题取决于分块，不是完全不支持空帧。建议输入耗尽后仍执行无需输入的状态转换。已验证实际解析器，尚未做 guest 中的 Pong/JS 事件验证。

## N09 — HTTP/2 END_STREAM 错误依赖 END_HEADERS 同帧

位置：[http2.c:479](/Users/wangzhe/system/LogitOS/c/net/http/http2.c:479)、[http2.c:487](/Users/wangzhe/system/LogitOS/c/net/http/http2.c:487)。

无请求体且编码后的请求头大于单帧容量时，输出 HEADERS + CONTINUATION。代码仅在 `first && last && blen == 0` 设置 END_STREAM；随后却把零体请求无条件标为 HALF_CLOSED_LOCAL，并清除 `req_end_stream`，后续不会补发空 DATA。

合法自定义 header 为 4 项各 7,000 字节，均在实现单值及总大小限额内。实际内存 transport 捕获输出，两次 pump 后：

```text
H2 header_value=0 stream=1 headers=1 continuation=0 end_stream=1 local_state=3 owing_end=0
H2 header_value=7000 stream=1 headers=1 continuation=1 end_stream=0 local_state=3 owing_end=0
```

客户端认为请求结束，服务器却未收到请求流结束信号，等待完整请求体的服务器可能一直等待。建议无体请求在首个 HEADERS 上标记 END_STREAM，独立处理 END_HEADERS。已验证发出的帧和本地状态，尚未做 guest 页面请求超时验证。

## 验证记录与尚未计入的候选

现有门禁通过：`make BUILD=build-netreview-transport-0909 test-tcp-host` 为 **241 passed / 0 failed**；`make BUILD=build-netreview-main-0909 test-unix-host` 为 **132/132 checks，UNIX-OK**；实际 `h2_test.c` 主机测试为 **2312 checks / 0 failures**。UNIX 编译有两条既有 weak 声明次序警告，不是功能失败。这些绿灯没有覆盖本报告新增边界，不足以推翻复现。

DNS/DHCP 探针使用 ASan/UBSan，重复运行无 sanitizer 诊断。主代理再次执行所有七个探针/测试二进制，结果一致；DNS/HTTP 源码指纹与子代理快照一致。**除 netdb 回归探针返回 1 外，其余观察型探针退出 0 仅表示成功输出状态，不代表被测实现正确。** 没有实施修复、没有新增正式测试门禁，所以未声称完成修复验收或全量构建。

证据目录 `/tmp/logitos-netreview-20260909/` 下：

- `transport/functional.c`、`functional.log`：TCP 对照和异常场景。
- `main/netdb_probe.c`、`netdb_probe_impl.c`、`netdb.log`：resolver syscall 接缝与失败断言。
- `main/unix_ready_probe.c`、`unix-ready.log`：实际队列状态、谓词与释放后对照。
- `dns/dns_legal_probe.c`、`dhcp_lease_probe.c`、`probe-results.json`：虚拟回答与租约时钟。
- `http/probe.c`、`probe.log`：WS/H2 帧级结果，包含未计入的 H1 候选。
- `source-manifest.json`：本轮关键源码 SHA-256 与 mtime；`recheck-results.json`：主代理复跑的 stdout/stderr/退出码。

这些临时证据可能被系统清理；关键输入、输出和源码定位已保留于本报告。探针是审查工具，尚未作为项目正式门禁集成。

HTTP/1 的 HEAD/304 加 Content-Encoding、无 body 时，库探针观察到 decode=-7，但实际消费者的 HEAD/null-body 和 304 缓存路径可能遮蔽影响，未据此写成用户可见故障。乱序 CNAME/A 回答也未计入主清单；本轮没有把仅出现不兼容现象但语义依据未厘清的候选算作确认 bug。
