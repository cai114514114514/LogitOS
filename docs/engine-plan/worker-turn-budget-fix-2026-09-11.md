# Worker 与页面任务轮次预算，2026-09-11

本次修复的是有限计算结束之后，浏览器仍继续成批执行其他 Worker 和页面任务、延迟处理输入与绘制的问题。生产调度使用共享的 8 ms 轮次预算；仅在完整回调或 Promise reaction 返回之后检查，保留同步 JavaScript/Wasm 调用的返回语义。单次原生 Wasm 调用依然不可抢占，本改动不承诺该调用执行期间界面可交互。

源码审计确认原 `js_worker_run_due()` 先扫所有 fetch owner，再耗尽到期任务快照。`worker_drain_jobs()` 只有 100,000 条数上限；一个 reaction 可以同步运行有限但很长的原生代码。页面随后还会执行动画与整批到期定时器。原有序号快照能阻止新建的零延时定时器链在同一批次无限延长，却不能限制已有任务的累计时间。浏览器外层输入轮询位于 `browser.c` 主循环开头，必须等这些函数返回。

落地行为：

- `js_task_budget.h` 提供唯一的 8 ms 阈值和时间比较。`js_worker_run_due_until()` 接收页面的同一 deadline；原无参数入口保持可用。
- Worker 的 owner 扫描位置和任务快照分别保留。暂停后继续处理剩余 owner，再处理原快照中的任务，不把新建定时器加入旧快照。
- 剩余 Promise jobs 通过 `JS_IsJobPending()` 接入 `pending/next_due`。同一个 Worker 继续自己的 jobs 后才接收下一任务；已经完成网络传输的 Worker 也不会因为没有 socket 或 timer 而睡死。
- 创建期间收到的消息保留在 Worker 所有的链表中，逐条取出；预算耗尽后链表仍有效，新消息追加到其后。回调可能关闭 Worker 时，重新选择队列任务，不跨回调保留可被取消的 task 指针。
- 每轮入口先按 due/sequence 顺序递送入口快照中已到期的父页面消息。上一 Worker 完成后的结果，不必再等待另一 Worker 的下一段长计算。各 Worker 自身的事件顺序保持不变。
- 页面保存 rejection、fetch、WebSocket、Worker、动画、timer 阶段位置。恢复轮的空尾部可以在剩余预算内进入一次新阶段扫描，避免无工作却多空转一轮。页面的微任务检查点保持完整；Worker 返回后的页面检查点重置自己的 watchdog，不将另一 realm 的计算时间算入它的运行额度。
- close/terminate 仍释放所属资源；页面关闭时清除阶段状态。原 watchdog 阈值没有放宽。

产品范围为 `c/apps/browser/js_worker.c`、`js_worker.h`、`js_page.c`、`js_page.h` 和新增 `js_task_budget.h`。没有修改 Wasm 指令执行算法、origin/CORS、网络策略、站点脚本或站点模块。并行的 Wasm reader 内联优化属于独立改动与独立证据。

独立 host 夹具使用真实 Worker/page 调度器、本地 HTTP 解析路径、普通加法和可控时钟：一次合法 native 加法返回 42，同时把测试时钟前进 20 ms。没有无限循环、故障输入、挑战数据或外网请求。

| 验证 | 结果 |
|---|---|
| 新 Worker 公平性夹具 | 59/59 |
| 同一有限输入 ASan/UBSan | 59/59，无诊断 |
| 修改前真实源码 | 59 项中精确 21 失败 |
| `JS_TASK_UNBOUNDED_TURN` 对照 | 精确 21 失败 |
| `JS_TASK_HIDE_PENDING_JOBS` 对照 | 精确 6 失败 |
| `JS_TASK_NO_PARENT_SWEEP` 对照 | 精确 1 失败 |
| 门禁接线检查 | 293 fragments，通过 |

三个编译对照逐条比较失败断言集合；编译失败、额外异常或无关失败不能当作预期结果。覆盖 ready/startup 消息、真实 fetch 完成后的剩余 jobs、同 Worker jobs/task 顺序、两个 fetch owner 的父消息优先、页面共享入口、terminate 和页面重新打开。独立夹具及日志位于 `tests/unit/worker_fairness_test.c`、`tests/worker_fairness.mk`、`build-worker-fairness-fixture/final-gates.log`。

最终源的相关正常回归：原 Worker 28/28，纯 globals 21/21，page-runtime 45/45，page-timers 26/26，worker-fetch 41/41，worker-wasm-normal 7/7，WAAPI paint 39/39。原套件要求的行为对照也通过门禁；日志保存在 `build-worker-turn-fix/evidence/related-final.log` 和 `original-worker-final.log`，调度源散列为同目录 `source-hashes.json`。同一 59 项有限输入在 ASan/UBSan 下通过，日志为 `build-worker-fairness-fixture/worker-fairness/san.log`；macOS 配置为 `detect_leaks=0`，不声称 LeakSanitizer 覆盖。未运行 Wasm GC/故障门。

真实 guest 新旧对照由主任务的 `tests/qmp/worker_turn_guest.py` 独立验证。本文的 host 结果不替代 guest 输入/绘制证据，也不证明任何站点聊天请求成功。微任务次序的边界遵循 [HTML 事件循环与 microtask checkpoint](https://html.spec.whatwg.org/multipage/webappapis.html#perform-a-microtask-checkpoint)：可以调度独立 Worker event loop，不能把同步 Wasm 返回伪装为 Promise，也不能从其原生调用栈重入页面事件。
