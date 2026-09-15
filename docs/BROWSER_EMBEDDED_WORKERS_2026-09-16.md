# 嵌入 Worker：实现与验证（2026-09-16）

目标仍为原生 iframe 内的人工验证，再进入搜索结果验收。本阶段消除了子文档完全没有 Worker 的实现缺口；真实 Google 本次返回了不同的 429 页面，没有进入验证 iframe，故不能证明真实验证组件已经运行，更不能宣称搜索通过。

## 实现边界

- `js_worker_context` 为每个文档保存稳定地址的 Worker 表、任务链、计时器编号与调度快照。不能照搬其它模块的结构体快照：QuickJS watchdog 持有表内 Worker 指针，搬移存储会留下悬空引用。
- `js_page_context_enable_workers` 显式启用已具备 WebAPI/platform 的关闭状态子文档。切换、pending、pump、close 和 destroy 都使用当前文档的 Worker owner；关闭子页不再关闭父页 Worker。没有开启的轻量 context 仍不安装 Worker。
- 修复了真实回调生命周期错误：Worker 给所属页面回调时只有 `js_page_slice_begin`、没有 `js_page_slice_end`，新 context 的进入计数因此无法归零，后续切换文档失败。不是简单忽略切换失败，而是补齐对应收尾并加入独立负对照。
- 嵌入页面目前只允许有原生 Blob 快照的 classic Worker。网络入口明确抛出 `NotSupportedError`，直到实现独立响应 CSP 的所有权；module/nested Worker 等原有限制不变。顶层旧路径保持原有行为。
- Blob Worker 使用所属 frame 的 CSP：创建遵循 worker-src → child-src → script-src → default-src；有相关 directive 时暂要求显式 `blob:`，不把 `*` 当作 Blob 授权，也尚未实现 Blob 对 `'self'` 的来源匹配。策略交集、安全拒绝优先，不伪装完整匹配能力。
- `importScripts` 使用 script-src/default-src，不使用 script-src-elem；允许的脚本通过嵌入 transport 加载，检查每次重定向、状态、JS MIME、每脚本 4 MiB 和每 frame 8 MiB 累计预算。同步加载不重入页面 JS。Blob 相对地址仍不可解析，绝对地址可用。
- Worker 字符串代码生成和 fetch 初始/重定向请求服从原生策略；fetch 继承 creator origin，但 site-for-cookies 使用祖先派生值，跨站祖先保持不透明。没有复制、篡改真实网站 Cookie 或验证令牌。
- 新嵌入 Worker 暂不暴露 WebAssembly，避免在未接入其独立 CSP 编译权限时默许代码生成。当前仍是合作式调度，每 owner 最多 8 个 Worker；不是 CPU 并行实现，也没有速度提升结论。

策略依据： [CSP worker-src](https://www.w3.org/TR/CSP3/#directive-worker-src) 和 [HTML Worker processing model](https://html.spec.whatwg.org/multipage/workers.html#worker-processing-model)。策略回调当前查询活 frame，后加 meta 可进一步收紧；没有声称实现完整的 Worker policy-container 快照。

## 宿主验证

`build-active-frame/worker-context-final.log`：第一版联合消费者普通版及 ASan/UBSan 均为 16/16；共享 owner 负对照 10 项、3 项失败；删除回调收尾负对照 10 项、2 项失败，均命中指定断言。

进一步的 Cookie 消费者用第一方页面设置本机测试域的 Strict cookie，再建立跨站子文档。子 Worker 必须实际完成自己的 HTTP 响应，且不能发送该 cookie；将 site-for-cookies 错换成 creator origin 的负对照必须只失败在泄露断言。这是本机内存 HTTP fixture，不操作真实网站会话。最终 `worker-context-cookie-final2.log` 普通版及 ASan/UBSan 均为 **21/21**；共享 owner 为 11 项/3 项预期失败，缺失收尾为 11 项/2 项预期失败，错误 Cookie site 为 21 项/1 项预期失败，三个负对照都是正门禁前置。

补测第一版曾错误地在跨站子文档内设置 Strict cookie，正负两版均无法建立 cookie fixture；这是原有 Cookie 限制的正确拒绝，不能把“没有 Cookie 可泄露”算作隔离通过。已改为先第一方设置并断言可读，然后跨站嵌入，最终错误 site 负对照确实检测到发送该 cookie。保留原失败日志 `worker-context-cookie-final.log`，未放宽产品策略或测试断言。

其它本阶段完成的回归：

- iframe 策略 111/111，原有策略负对照通过预期失败检查。
- Worker 原目标 28/28，worker-globals 21/21；对应负对照按原门禁检查。
- page/WebAPI contexts 85/85，普通版与 ASan/UBSan 均通过；page owner 字段 AST 检查通过。
- Worker fetch 41/41，普通版与 ASan/UBSan 均通过；missing/foreign-owner/unpolled 三个负对照分别为 1、2、3 项预期失败。日志 `worker-fetch-regression.log`。
- 原生 active-frame 正常/sanitizer 回归通过，日志 `worker-frame-build.log`；接线为 377 fragments、376 reachable、1 declared（共享树观察值）。

本节不是整个浏览器全绿报告。前一记录中的旧 WebAPI 234 项/6 项失败，以及尚未做 HEAD 归因的 message-port-platform 226 项/4 项失败，未在此修复。Sanitizer 在 macOS 关闭 leak detector，不能据此宣称全局无泄漏。

## 真实客机消费者

```sh
python3 tests/qmp/active_frame_guest.py \
  --iso build-active-frame/work/logit.iso \
  --disk build-active-frame/work/disk.img \
  --out build-active-frame/guest-workers \
  --workers --large-script \
  --public-url 'https://www.google.com/search?q=LogitOS' --keep-open
```

`guest-workers/results.json` 的本机消费者通过：父页面 Worker 与子页面 Worker 同时存在；子页实际执行 1,050,014 字节外部脚本；子 Worker 导入 19 字节脚本，子页面和子 Worker 各完成一次 20 字节 fetch。原生点击的消息经过子 Worker、父窗口、父 Worker，再回复子窗口，最终父页 `PARENT-MESSAGE-PASS`、内嵌子页 `FRAME-GUEST-PASS` 均实际绘制。已检查 `embedded-pass.png`，不是只看 console 或外开页面。

该 AEX 随后普通导航至 Google：200 入口响应后转到 429（2,569 字节），截图为“异常流量，请稍后再试”，未创建验证 iframe。再进行一次普通地区搜索地址导航后仍是 429（2,817 字节），`public-regional.png` 同样没有可操作验证界面。没有连续刷新、自动点击验证或分析求解挑战算法。原有 load listener 仍报告缺失函数；不能将这次“没有 Worker undefined”当作真实验证组件已通过，因为它根本没有进入此前的子页面路径。

`public_search_accepted` 明确为 false。真实网站当前响应与上一轮不同，下一次验收必须从实时响应继续，不能以重放旧页面或本机 fixture 代替网站通过。搜索结果页面仍待验收。

## 制品与运行状态

独立构建和磁盘重打包完成，保留 5 个用户状态 inode；QEMU 使用 snapshot，未修改默认运行磁盘。共享目录有大量其它并行改动，本记录只归因上述 Worker 工作，没有提交 Git。

```text
5d1493e499cac8358522a483263938c2c4a019998e8295247c971b111b60d5d1  browser.aex
b2d073f2c2161deb12ef956fc17a4f15bd8e0dc87f6107723a49e0daacfeb288  disk.img
0f80979f37e8a17e590b45e7579f26231ae137607f99047c2f9926784190ec73  logit.iso
```

本次独立 QEMU PID 21875，QMP `/tmp/active-frame-tzc89dhs/qmp.sock`，留在真实 Google 429 页面供人工查看；fixture HTTP 服务已退出，因此原本机页面不可刷新。进程号和 socket 仅为本次记录，继续工作前必须重新核对，不得据此终止其它客机。

## 持续目标：直到出现验证码（首次检查）

2026-09-16 00:19（Asia/Shanghai）重新核对 PID 21875 的完整启动参数，确认仍为上述独立 snapshot 客机，未重启或更换制品。通过该客机当前页面的普通 JS 诊断入口，只读取 iframe 数量及 script 的类型、是否有 src 和文本长度，没有读取或求解挑战内容。串口第 1764 行为 `PAGE-INVENTORY 0 []`：当前 DOM 没有 iframe 或 script 节点。

随后仅做一次正常 Ctrl+R 刷新；现有实现对这次 reload 绕过本地 HTTP 缓存。串口第 1790 行记录重新收到 `status=429 len=2817`，第 1808 行仍为 0 external classic、0 external module、0 inline。页面继续报告原有 load handler 所调用的函数未定义。本轮没有下载到此前的验证子页面，不能据此继续推断其 Worker 或布局状态。

已请求用户在当前网络下用平常使用的 Safari/Chrome 手动对照，只反馈“同样无验证码／有验证码／正常搜索”；不索取 Cookie，不改网络，不伪装浏览器。Google 官方说明也区分未显示 reCAPTCHA 时的浏览器/JavaScript 检查和持续异常流量的网络问题：[异常流量帮助](https://support.google.com/websearch/answer/86640?hl=zh-Hans)。这不证明具体封锁原因，故待对照结果后再定位。目标仍未完成，本轮不以本机 fixture 或替代验证码作验收，也未把目标标记完成或阻塞终止。

## 第五阶段：真实组件暴露的网络 Worker 缺口

为分离搜索 429 和浏览器组件问题，同一旧客机普通导航至 Google 的 `/recaptcha/api2/demo`，仅观察、不点击验证。真实 iframe 执行了 844,362 字节外部脚本，随后明确失败于 `NotSupportedError: Embedded network Worker policy is not implemented`。这是新的产品缺口，不再只等待搜索响应变化。旧客机证据 `guest-workers/demo-network-worker-missing.png`；其 PID 21875 已核对后退出以重建独立磁盘。

本阶段更新第四阶段“只允许 Blob Worker”的边界：

- 原生入口回调在 deferred startup 加载 HTTP(S) classic Worker，入口及每次重定向须满足创建页面的 worker-src 回退策略和严格同源。网络 Worker 的相对入口以所属文档 URL 解析，不再借用顶层 bfetch 全局 base。
- 入口只接受完整策略元数据、2xx、JS MIME、有界响应，复制最终 URL 和响应 CSP；无法实现的响应 sandbox/Trusted Types 要求仍拒绝。入口和 imports 纳入每 frame 8 MiB 总预算、单脚本 4 MiB 上限。
- 每个网络 Worker 独立持有响应策略，importScripts/connect/eval 不再借用创建页面的 script-src。相对 import 和 fetch 以入口响应最终 URL 为基准；SameSite 则仍保留文档祖先限制，不能误用最终 URL 作为第一方。
- 响应策略释放晚于 Worker fetch/runtime 关闭；Blob 路径保留创建页面策略，不释放 frame 本体。顶层原路径保持不变。本阶段仍不构成完整 Worker/COEP/跨源隔离实现，也未开启嵌入 Worker 的 WebAssembly。

`network-worker-gates.log`：联合上下文测试普通版和 ASan/UBSan 均为 **28/28**；共享 owner 11 项/3 项预期失败，缺失收尾 11 项/2 项预期失败，错误 Cookie site 28 项/2 项预期失败，错用创建页面策略 28 项/3 项预期失败。后者证明响应策略确实被消费者使用，而非只保存字段。iframe 策略 **118/118**；接线 378 fragments、377 reachable、1 declared（共享工作树观察值）。

`network-worker-regressions.log`：原 Worker 28/28、globals 21/21、Worker fetch 普通与 sanitizer 各 41/41，原负对照及 active/active-large 通过；`network-worker-frame-san.log` 的 passive/active 正常和 sanitizer 模式通过。没有处理前文旧红门禁，也不宣称全部浏览器测试已通过。

### 独立客机与测试仪器修正

第一次 `guest-network-worker` 实际收到 327 字节网络 Worker、19 字节 import 和两份各 20 字节响应，并走完消息联动，但内核 kbench 的并行输出打断了最终 `[dl]` 行。脚本因缺失完整行失败退出，报告保留为 false；核对进程终止后才重跑。

驱动增加仅针对这类断行的取证分支：先等真实子页消息和只由绘制日志输出的最终文字，再在完整前缀丢失时截图、重采样已有 display list，要求整个 fixture 区域前后像素完全一致。若额外按键实际修复了旧画面，仍判失败，不能用重采样掩盖先前的消息后布局问题。新分支本身不改变产品布局或放宽最终完整绘制文字要求；本次最终重跑直接收到了完整行，未触发该分支，故该分支的原生触发路径仍待独立复现。

`guest-network-worker-final/results.json` 本机消费者为 PASS，已目视检查 `embedded-pass.png`：1,049,677 字节子脚本、327 字节网络 Worker、19 字节 import、两次各 20 字节 fetch，父、子页面原位置均绘制最终成功文字。测试同时要求网络 Worker 的响应 CSP 禁止 eval；不是只检查构造器存在。

```text
10db9d1e472fecf9a1dc983922f837d86855f259dda177889f365cfab7c419b0  browser.aex
9d07ddef84059f78586b47ae1db428ef8c9aa282b7ec7ebc6276425a38c31da5  disk.img
0f80979f37e8a17e590b45e7579f26231ae137607f99047c2f9926784190ec73  logit.iso
```

制品打包保留 5 个用户状态 inode。最终测试参数使用 `--network-worker --large-script --public-url https://www.google.com/recaptcha/api2/demo --keep-open`，没有用 demo 冒充 Google 搜索验收。首次真实演示页访问中，客机 socket 报 `no source address for any dst`，入口 `/recaptcha/api.js` 下载失败，未进入 iframe。这是本次实测网络错误，不归因于 Worker 策略。该客机保留运行，随后只做一次正常重载观察；后续结果需追加，不能沿用首次失败或旧制品画面作最终结论。

### 重载后真实组件结果与下一项缺口

一次正常 Ctrl+R 后，网络错误不再阻断本次入口：真实子页执行 **846,252 字节**外部脚本和 **36,255 字节**内联脚本，均 `evaluated=1`；实际网络 Worker 入口 **102 字节**被加载并持有响应策略。不能再把该次结果描述为“网络 Worker 未实现”。

接下来出现两项明确兼容性错误：`DataCloneError: Window transfer lists are not supported`，以及 Worker 中的 `ReferenceError: 'MessageChannel' is not defined`。随后组件显示“Please upgrade to a supported browser to get a reCAPTCHA challenge”，截图 `guest-network-worker-final/demo-after-reload.png` 已检查。**这不是可操作验证码，更不是 Google 搜索通过。** 未点击、选择或提交任何真实验证答案，没有分析挑战算法或修改验证参数。

已只读核对现有消息实现：`js_message_port.inc` 是页面 runtime 内的 WeakMap/定时器通道，注释和实现均明确不支持跨 runtime 转移；`embedded_window.inc` 拒绝 transfer 参数；Worker 的序列化路径也拒绝非空 transfer list。下一步必须实现真正的跨 realm 端口所有权转移、队列与关闭/导航围栏，并给 Worker 接入 MessageChannel，不能通过空构造器或把转移当复制蒙混过去。本轮未修改这三个端口实现路径。

当前独立客机 PID **76725**，QMP `/tmp/active-frame-y2hua5uz/qmp.sock`，仍运行上述新制品并停留真实组件页面。搜索原始目标保持未完成；此次组件对照带来了可执行的下一项浏览器修复，不满足目标阻塞终止条件。

## 第六阶段：原生 MessageChannel 与 Window 端口转移

更新第五阶段“端口实现路径未修改”的边界：新增 `js_ports.c/.h` 原生端口代理，页面与 Worker 各自安装、调度、关闭。跨 runtime 的消息只携带序列化字节及原生端点身份，不复制外来 JSValue。Window 的 transfer list 现可移动端口，接收者通过 `MessageEvent.ports` 的只读冻结数组取得本 realm 包装；克隆失败不分离端口，成功转移后旧包装不能发送或关闭新所有者，转移前排队消息保留。错误目标 origin、iframe 生命周期失效及整页关闭丢弃相应消息与在途端口。回调只在接收 realm 的有界异步 pump 中执行，不赋予用户激活。

边界仍明确：256 个端点、每消息 16 个转移端口/64 KiB、总消息字节 8 MiB、每端口 64 条队列/32 个监听器。包装持有到 close/transfer/realm teardown，不是完整的可达性 GC；端口嵌入 data 对象图、ArrayBuffer 转移、Worker.postMessage 的非空 transfer list 仍显式拒绝。原生端口提供消息监听与 start/close，尚不构成完整 EventTarget 接口及全部 MessagePort 标准实现。未修改站点识别、挑战参数或验证结果，也没有求解验证码。

顺带纠正网络 Worker 清理路径：之前延迟 reap 会释放响应策略，但直接 document close 没有释放；现在两条路径均释放，并由保留一个活 Worker 后关闭文档的测试检查恰好释放一次。

### 测试与消费者

- `native-ports-final-gates.log`：两个真实 QuickJS runtime 的原生端口 **27/27**，普通版和 ASan/UBSan 各通过；不分离端口的负对照 **10 项/1 项失败**，不关闭端口 **27 项/1 项失败**，均命中指定断言。新增 `active-ports` 让原生子页点击经转移端口完成双向消息与子页重绘，普通/ASan/UBSan 通过；禁用 Window 投递的同制品负对照如期失败。原 active 模式链接新模块后仍通过。
- `native-ports-regressions-final.log`：Worker 联合上下文普通版和 ASan/UBSan 各 **31/31**；新增 Worker 内部 MessageChannel 调度与整页关闭释放断言。页面消息检查原生版、原生 ASan 和旧 JS fallback 各 **21/21**。旧 clone/close 负对照继续与 fallback 成对执行，分别 3/1 项预期失败；不能把旧 JS 变异误称为新原生实现的负对照。原生所有权负对照是页面门禁前置依赖。
- `test-mk-wired` 通过，当前共享树观察值 378 fragments、377 reachable、1 declared。旧 WebAPI/platform 红门禁不在此次修复范围，不宣称全浏览器门禁全绿。

核实并退出旧 PID 76725 后重打独立磁盘，保留 5 个用户状态 inode。制品为：

```text
06e897208f10cf9e1b3a719a9750d5ee58fd85248d3d8f4523594e3319fdc966  browser.aex
347eeb8383c57209ff0072e927ff5339084f6d2509ee57b31d60c47fb75d27c1  disk.img
0f80979f37e8a17e590b45e7579f26231ae137607f99047c2f9926784190ec73  logit.iso
```

`guest-native-ports/results.json` 的本机消费者 PASS：**1,049,726 字节**子脚本、**469 字节**网络 Worker、19 字节 import、两份各 20 字节 fetch。子页传出端口，父页验证 origin/source/接收 realm/冻结数组；子页原生点击经过子 Worker 内部端口、跨文档端口、父 Worker 后回到子页，父子实际绘制完成文字。`embedded-pass.png` 已目视核对。此消费者没有点击真实验证码。

### 真实组件观察，不冒充搜索验收

同客机一次普通导航到 Google 官方演示页，实际子页执行 844,362 字节外部脚本与 36,434 字节内联脚本，网络 Worker 入口 102 字节。前一轮的 Window transfer 拒绝和 Worker MessageChannel 未定义错误未在本次串口中再出现。**勾选框和 “I'm not a robot” 已在原 iframe 位置显示**，`public-observation.png` 已目视检查，不再是升级浏览器提示。

仍出现 `TypeError: cannot read property 'parse' of undefined` 以及后续超时；该错误的具体对象尚未定位，不能推断为已修复或宣称验证可用。没有点击勾选框，尚未证明挑战弹窗、人工提交、验证完成或 Google 搜索结果。演示页仅作兼容性对照，`public_search_accepted` 保持 false。

当前独立客机 PID **28565**，QMP `/tmp/active-frame-r2jbbshu/qmp.sock`，串口 `build-active-frame/guest-native-ports/serial.log`。本机 HTTP 服务已退出，不要刷新旧 fixture 页面。上述进程信息只作记录，后续操作必须重新核实完整参数。

### 同制品搜索与标签恢复复核

一次普通 Google 搜索导航收到 **HTTP 200 / 91,869 字节 / 5 段内联脚本**，随后页面自己的导航触发新请求，客机报 `no source address for any dst`，该请求 status=0。`search-observation.png` 仅显示无法访问搜索时的提示链接，没有搜索结果或验证码；不能继续沿用旧版本“该次搜索为 429”的说法，也不能把 200 当成功。

切回原演示页标签，缓存恢复路径重新执行脚本后出现 `passive refused: source, parent policy, or viewport`，画面退回“打开嵌入页面”按钮。`manual-ready.png` 实际记录的是这次**未恢复**状态，文件名不等于通过证据。随后仅普通 Ctrl+R 一次，等待新响应和实际画面；此标签恢复问题仍未定位或修复，不得靠刷新宣称已解决。

这一次 Ctrl+R 后入口和子页均重新收到 200，执行 846,252 字节外部脚本与 36,078 字节内联脚本，102 字节网络 Worker 入口加载；`demo-manual-reload.png` 已确认勾选框再次内嵌显示。`parse` 异常仍存在。窗口停在此页交给用户手动操作，不再切标签或代点验证码；是否出现挑战、能否提交，等待用户的真实结果。目标继续保持 active，尚未完成真实搜索验收。

## 第七阶段：标签恢复保留原页面响应策略

已从代码定位第六阶段的恢复失败：`load_once_impl` 在保留 HTML 的 hydration 分支调用 `passive_frames_set_parent` 时传 `known=0`，因此重新创建的网络 iframe 必然被拒绝。这是缺失元数据，不是 viewport 的已证实问题。

`tab` 现拥有与原页面字节一起保留的响应 CSP 字符串和完整性标志；新导航记录本响应，恢复读取本标签，释放内容时清理策略。它不读最后一次其它页面请求的全局元数据，也不把未知/超长/分配失败转换成允许加载。空字符串但已知的响应政策与未知政策明确区分，新增分配计入 retained bytes。磁盘 session 仍不保存政策，恢复 URL 后必须正常下载；这不是保存多个活 JS runtime，也不会保留挑战状态。tabs.h 保留原先“恢复不触网”的说法旁的修正：仅指保留的顶层资源，重新创建的网络 iframe/Worker 仍重新取其响应与策略。

`tab-policy-final-gates.log`：新增 7 项策略复制/别名/空值/超长/计数/释放检查，普通与 ASan/UBSan 都通过；`active-ports-restore` 驱动真实 browser loader 在两份不同 CSP 页面之间往返，原页面只取一次，重新创建的 iframe 可再次完成原生点击、端口消息与画面更新，普通与 ASan/UBSan 通过。`PF_TEST_LOST_TAB_POLICY` 负对照只将恢复路径改回未知，产生 4 项指定消费者失败，门禁要求点击后画面断言明确变红。现有 tabs 负对照及完整 loader ASan/UBSan 通过，`test-mk-wired` 仍为 378/377/1。

没有关闭或操作等待人工点击的 PID 28565。新制品复制到独立 `build-active-frame/tab-policy-work.UrEHQQ` 后打包，只修改该新磁盘，保留 5 个用户状态 inode；当前人工窗口仍是第六阶段制品，不能称已更新到本阶段。

```text
1be0077c535925018d3e05e99f83aedb6d8f7b07884a08f4258541fda06006a8  browser.aex
af61ced6a88e9b2c00850ca0e97b054e41ade84f6fbc2986065f4599c9daa242  disk.img
```

独立客机门禁新增 `--tab-roundtrip`，每次等待使用新的串口位置，不能用切走前的成功文字蒙混恢复后的画面。该测试不访问公共验证服务、不点击验证码，完成后只退出它自己创建的 headless 客机。结果记录在 `guest-tab-policy/results.json`；实际运行结论在下文追加。

第一次 `guest-tab-policy` 保留为失败：观察到其它页面首帧便发送切换按键，当时同步加载尚未完成，串口中没有出现恢复加载，最终缺少第二次 FRAME-READY，驱动超时并退出其 PID 66341。没有将超时当成成功或只凭等待超时重启尚活进程。驱动新增明确的 `load-complete` 围栏后，在同一二进制和磁盘上复测。

`guest-tab-policy-final/results.json` 为 PASS，`restored-embedded-pass.png` 已目视核对：第二次父子端口回调及实际绘制文字均来自恢复后的串口区间；`/parent` **1 次**、`/other` **1 次**、`/child` **2 次**，证明顶层 HTML 真正复用且 child 重新载入。新 headless PID 70928 已由驱动正常收尾，人工演示窗口 PID 28565 仍运行。本轮未更换该人工窗口的磁盘或 JS 状态，未访问或操作公共验证服务，验证码人工结果和真实搜索结果仍未获得。
