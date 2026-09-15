# XHR 响应头、真实增量与回调异常的 guest 验收

`tests/qmp/xhr_stream_guest.py` 在独立 QEMU 中运行本地普通 HTML/JS，经真实 HTTP/1.1 `Transfer-Encoding: chunked` 接收三段合成 JSON。夹具用两条请求区分正常 MIME 分类与异常回调：一个请求根据 `request.HEADERS_RECEIVED` 读取 Content-Type，另一请求在 readystatechange、progress property/listener 和 load 回调中抛出本地测试异常。

两次都由真实鼠标点击 `(238,648)` 开始。独立 QMP socket、单一 Session、`-snapshot`、`restrict=on` 与唯一 localhost guestfwd 保证不连接 DS 或用户 VM。guestfwd 对每条连接使用独立 forwarding process，支持导航加两条并行请求；不靠共享 chardev 串行化 HTTP。

## 结果

当前 `build-ds-render-fixed/snapshot-xhr`：8 项全部通过，页面最终 8 个状态像素均为绿色。正常和异常请求都收到 45 字节、HTTP 200、4 次 progress、各一次 load/loadend、0 次 error。这里记录观测到的 progress 数量，测试只要求必要的增量与顺序，不硬编码总数。

1. XHR 实例可读到 UNSENT/OPENED/HEADERS_RECEIVED/LOADING/DONE 的 0–4 常量。
2. 一次性响应头 hook 在 HEADERS_RECEIVED 成功运行一次并读取 HTTP 200/application/json。
3. 普通 JS adapter 根据响应头选择 JSON 解析，成功得到合成结果，没有将这些字节交给 SSE 分支。这是消费者正确获知 MIME 的验证，不是声称 XHR 自带 SSE 分类器。
4. 两条请求的第一、第二段 responseText 前缀都在服务端 EOF 前出现。服务端第一段等待主机实际观察进度后才发第二段；最后一段和 EOF 必须等 early 截图完成才释放。
5. 最终 progress 暴露完整文本与累计字节数，随后依次是 load、loadend。
6. 响应头回调抛异常仍保持 HTTP 200，最终 load/end 各一次、error 为零。
7. progress property 与 listener 抛异常之后，后面的 listener 仍执行；本次 property 抛异常 4 次，后续 listener 也运行 4 次，请求完成。
8. load 回调抛异常仍继续 loadend，并保留 DONE 状态。

旧 `build-ds-render-fixed/snapshot`（已含渲染修复，未含本轮 XHR 修复）：上述组合断言 0/8，但正常请求网络正控制仍为 HTTP 200、45 字节、load/end 各一次、error 为零，其增量前缀也可见。失败不是本地服务器或 HTTP 通道没有工作：响应头 hook 为零、adapter 留在错误分支；抛响应头异常的另一请求变为 status 0/error 1，没有 load。这个旧版组合对照不分别隔离每种异常；对应单行为负控制由 host XHR suite 覆盖。

## 时间与像素证据

当前运行主机 monotonic 时间：normal 第二段进度观察在 `226822.726264291`，noisy 在 `226822.80736775`；early 截图完成在 `226823.811216416`；两条 EOF 分别在其后 `226823.811283833` 和 `226823.811336625`。early 图中两条增量指示均为绿色，最终结果仍 pending；after 图才显示 8 项全绿。两张图及旧版 after 图均已人工查看。

证据目录：

- `build-ds-render-fixed/xhr-guest-http11-current/`
- `build-ds-render-fixed/xhr-guest-http11-old/`

各含 `fixture.html`、`before.png`、`early.png`、`after.png`、`serial.log`、`qemu.log`、`results.json`。JSON 保存逐项布尔结果、像素、HTTP/事件计数、服务器发块/EOF 与主机观察时间、PID 和镜像哈希。首轮默认 HTTP/1.0 的夹具试运行保留在无 `http11` 的目录，不用于本页最终结论。

新旧 fixture SHA256 均为 `06ffab873813959167041bffc2fcc595159ab896bbe75c7e8e37ccae0f05b305`。当前 browser SHA256 为 `fa4cd5c8646cb55bfd4467cec5ac118759ef8ac1f88f458a33443d0ce9eef6a7`，对照为 `ab4caaa03e8210d7dd0ba2c3f5f955410b341b3e02504b4eb396f80918c669ad`。私有 PID 76579/76578 均已结束，冻结 ISO/disk 前后哈希一致。

```sh
python3 tests/qmp/xhr_stream_guest.py --snapshot build-ds-render-fixed/snapshot-xhr --out build-ds-render-fixed/xhr-guest-http11-current
python3 tests/qmp/xhr_stream_guest.py --snapshot build-ds-render-fixed/snapshot --out build-ds-render-fixed/xhr-guest-http11-old --expect-old
```

本项证明普通 XHR 公共行为在真实 guest 中修复，不等于实际 DS 聊天、认证或服务端响应已经成功。未读取真实账号、验证码、Cookie 或请求正文。
