# Parent fetch handoff and initial deadline

2026-09-11。本轮修复两个可用普通本地请求证明的浏览器调度缺口：父页面收到 Worker 结果后，新请求在下一个 Worker 计算开始前获得一次网络服务机会；请求从创建到首次服务之间的长调度间隔也进入既有空闲计时补偿。没有修改站点代码、凭据策略、服务端规则、30 秒空闲阈值或 250 ms 间隔阈值。

## 具体调用链

旧顺序是 `js_page_run_due` 的网络阶段 → Worker 阶段 → 父消息回调与父微任务 → 下一个 Worker。父回调中的 `fetch()` 已经通过 `fetch_dial` 打开 socket，但 HTTP 构造与发送仍需 `fetch_step`。`js_page_pump` 只运行微任务，不会驱动 HTTP。因此父消息已送达不能证明它触发的请求已经发送。

`js_worker_run_due_for_page(deadline, &handoff)` 现在在完成父通知批次后返回；普通 task 扫描中遇到父通知也遵循同一边界。`js_page.c` 在新的父页面 CPU slice 中调用 `js_webapi_fetch_checkpoint`，再完成父微任务检查点，随后直接返回外层输入与绘制循环。Worker 剩余队列、snapshot 与续跑位置保留；页面后续动画和定时器阶段也保留。新的 checkpoint 复用 `fetch_step` 的有限 slot/step 扫描，不调用计时器、历史回调或其他 Worker，不等待握手。原独立 `js_worker_run_due` / `_until` 没有页面阶段可交回，继续保留原行为。

对应代码：`c/apps/browser/js_worker.c:1541`、`js_page.c:803`、`js_webapi.c:3473`；声明位于 `js_worker.h` 和 `js_webapi.h`。

网络的边界仍然分明：`browser_rt.c:2151` 打开非阻塞 socket；内核 `wm.c:6142` → `net_poll` → `sock_pump` 能独立推进 DNS/TCP/TLS。浏览器的 HTTP/1 写请求在 `c/net/http/http1.c:1051`，HTTP/2 推进在 `browser_rt.c:2694`，仍由浏览器 pump 调用。已就绪连接可以在下一次 native 计算前发送；未就绪连接只获得一次服务机会，之后正常恢复 Worker，不停住 Worker 等待远端。

## 首次服务的计时基线

`WF_DIAL=1` 表示连接/发送前阶段，不是尚未执行 `sock_open` 的 JavaScript 队列。旧 `fetch_dial` 创建 30 秒 deadline，却把 `last_step` 设成 0。`fetch_step` 在 poll/send 前检查 deadline，并且只补偿已有 `last_step` 的大间隔，因此首次 pump 前阻塞会直接产生本地超时。旧 `zaiblank_test.c:230` 先 pump 一次再模拟阻塞，没有覆盖首次服务边界。

现在 `fetch_dial` 用同一次时钟读取设置 deadline 与 `last_step`，用独立 `last_step_valid` 表明时间零也是有效基线。后续沿用既有补偿算法。正常持续 poll 的连接与响应空闲仍在原来的约 30 秒后超时；内核 DNS/TCP/TLS 自身的期限与错误不变。代码位于 `js_webapi.c:1275`、`:1677`。

## 验证

| 范围 | 当前实现 | 旧行为对照 |
| --- | --- | --- |
| 父消息直接 fetch / 父 Promise 微任务 fetch | 18/18；两案均 `first-add=1, fetch-created=2, first-send=3, second-add=4` | 保存的三份真实旧源与 `JS_TASK_NO_PARENT_FETCH_HANDOFF` 均精确 2 项顺序失败；正常 HTTP 200 与算术 42 仍通过 |
| 首次泵延迟、时间零、持续连接/响应轮询 | 15/15；同输入 ASan/UBSan 15/15 | 保存的真实旧 `js_webapi.c` 与 `WEBAPI_FETCH_NO_INITIAL_BASELINE` 均精确 3 项失败；活跃轮询的正常超时控制仍通过 |
| 原有限调度 | 60/60 | 完整旧调度控制 22 项、隐藏 pending jobs 6 项、取消父优先扫描 2 项精确失败 |
| 原功能回归 | page-runtime 45/45；page-timers 26/26；stream 62/62；buffered 32/32；webapi 227/227；Worker fetch 41/41；独立 Worker 28/28 | 相关目标原有负控制通过 |

父 fetch 顺序 fixture 使用真实页面入口、HTTP parser 和可短写的本地字节流，以网络 vtable 的实际 `send` 为观测点。它没有把 socket open 视为请求发送。两个 Worker 都只进行有限加法，观察器把受控时钟前进 20 ms；没有测量或执行站点计算。

常用命令：

```sh
make BUILD=build-parent-fetch-fix test-fetch-initial-deadline
make BUILD=build-parent-fetch-order test-worker-parent-fetch test-worker-fairness
```

首泵日志位于 `build-parent-fetch-fix/fetch-initial-deadline/`，其中 `actual-old.log` 使用保存的旧生产源码，`san.log` 使用同一组普通输入的 ASan/UBSan；禁用 leak 检查，没有声称 LSan 通过。父顺序与调度日志位于 `build-parent-fetch-order/final-gates.log`、`worker-parent-fetch/`、`worker-fairness/`，详见同目录文档 `worker-parent-fetch-handoff-2026-09-11.md`。普通功能回归归档在 `build-parent-fetch-fix/evidence/`。同一组 18 项顺序输入的 ASan/UBSan 也已通过；本轮没有新增 GC、无效内存或故障复现用例。

## 验收边界

此报告是本地 host 功能与对照证据，真实 guest 由主代理对冻结镜像单独验收。本轮没有操作登录中的虚拟机或发送外部网站请求。单次同步 native 调用仍不可抢占；修复的是它返回之后的后续任务与网络服务顺序。不能以本地成功推断真实网站已经正常回答，也不能把某个服务端拒绝码归因于本地超时。
