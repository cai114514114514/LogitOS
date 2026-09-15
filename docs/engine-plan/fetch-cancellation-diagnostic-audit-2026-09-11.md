# Fetch cancellation diagnostic audit

2026-09-11，只读审查。主代理从 attempt06 的既有日志中仅抽取固定浏览器原因文本，确认请求 id 2、4098 的 `fetch-detail` 前一条原因均为 `aborted`；另外两个 phase=1 请求也走相同原因。没有 `could not reopen after the preflight`。本子任务未读取站点载荷、重放请求、运行测试或操作虚拟机，未修改生产代码。

结论：这两条 `state=2, error_code=0, phase=0, boundary=transport-or-policy, status=0` 属于主动取消，不能归类为 EOF、连接打开失败或 CORS 拒绝。它们本身不能证明服务端异常，也不能解释其他请求的应用返回码。

## 字段与代码对应

* `js_webapi.c:1999` 的 `js_fetch_abort` 调用 `fetch_fail(...,"aborted","AbortError")`。取消一个已经启动但尚未收到响应头的请求时，`WF_XFER=2` 可以与 `conn.err=0`、`resp.err=0`、`resp.code=0` 同时存在。这个分支没有设置 `failure_at`，所以 `fetch_trace_detail` 在 `js_webapi.c:1043` 使用默认标签 `transport-or-policy`。默认标签并不是已判断出的错误类别。
* 正常 HTTP/1 在响应头前收到 EOF 时，`http1.c:805` 将其记为 `H1_E_TRUNC`；`h1_conn_pump:1084` 把非零错误写入连接。HTTP/2 的关闭、RESET、REFUSED 等在 `browser_rt.c:2574` 映射为非零通用错误。故 `error_code=0` 不能单独用作 EOF 证据。
* 实际 CORS 拒绝有显式标签：`cors-response`、`cors-preflight`、`redirect-cors`（`js_webapi.c:1415`、`:1759`、`:1572`）。OPTIONS 成功仅证明该次预检成功；后续实际请求仍有自己的策略检查。此处观察到的是取消路径。
* 只看数值时还有其他候选：`fetch_redial` 在 `js_webapi.c:1530` 清空旧连接后，若新 open 失败，原 `WF_XFER` 状态可以保留，错误与状态码已经归零。预检/重定向调用者会打印固定原因 `could not reopen after the preflight` 或 `redirect target could not be opened`。本次既有日志明确没有匹配该候选。

## 现有日志的判断边界

`fetch-request` 不是字节已经发到网络或服务器已经收到请求的证明：成功路径在 `bxfer_start` 完成协议启动后打印（`js_webapi.c:1253`），HTTP/1 真正写入发生在后续 `h1_conn_pump`；并且 `fetch_dial` 的 open 失败路径也打印该事件（`:1272`）。诊断中的 method/phase 表示当前意图，不能替代传输层实际 send 观测。

当前固定原因文本足够把本次取消与预检后 open 失败区分开；单独保留 `fetch-detail` 数字与默认 boundary 则不够。HTTP/2 底层错误也会映射到共享错误码，现有字段不能完整区分每一种底层关闭原因。未来若扩诊断，应增加浏览器内部固定类别（例如取消、重拨失败）及协议/连接状态等数值，不需要任何 URL path/query、消息正文或站点验证内容。本次无需新增夹具或改变策略。
