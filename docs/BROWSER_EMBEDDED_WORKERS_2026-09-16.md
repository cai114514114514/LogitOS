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

## 第八阶段：子文档原生焦点、键盘目标与 CSS 焦点状态

上段“未操作公共验证服务”描述的是第七阶段，不能套用于本阶段：随后在旧 PID 28565 的真实组件上只点击了一次启动复选框，未选择挑战答案、未提交验证。原生事件暴露 `focus is not a function (it is undefined)`；截图 `challenge-open-fuhnc1a9/after.png` 没有图像挑战。这是通用 HTMLElement 能力缺失：子页面不安装仍依赖全局编辑表的 js_forms，因而连 focus/blur 也缺失。

焦点 holder、节点代次和事件派发器现由独立 `focus_context` 持有，随 `js_page_context` 切换，不把子节点泄漏给父文档。`embedded_focus.inc` 安装真实 `focus()`、`blur()`、`document.activeElement`，复用原生可聚焦性、焦点事件与销毁检查。同步样式变更先解析子页面自己的 CSS；已激活页面的 CSS 上下文启用交互代次。父文档的焦点在退出子页面 JS 栈后提交到 iframe 元素，后续原生键盘事件使用子页面 holder。重入焦点处理器切换目标时，不再对已失去焦点的旧目标发送后续 focusin。没有主机名分支、空操作 focus shim、伪造验证状态或复制第三方浏览器信号。

边界：尚无完整的子页面原生文本编辑、焦点滚入视口、focus-visible、嵌套浏览上下文和各文档独立 modal/top-layer。此阶段不能称为完整 HTML 聚焦算法。

`frame-focus-complete-gates.log`：`active-focus` 普通和 ASan/UBSan 通过，消费者检查事件顺序、activeElement、disabled/inert/隐藏/断开节点拒绝、负 tabindex、删除回退、事件重入，以及父子消息往返后的实际键盘目标和 `:focus` 绘制颜色。两个负对照都实际变红：去掉安装报 `blur is not a function`；强制共享 owner 报 `blur owner`；两者都失去点击后画面、父消息及键盘/CSS 消费者，共 3 项断言失败。负对照是正门禁的先决条件。既有 `active-ports-restore` 通过；focus-style-flush 正常 9/0，两个预期负对照各 9/3。

`frame-focus-regression.log`：Page contexts 34/0，worker-context 31/0，test-mk-wired 378 fragments / 377 reachable / 1 declared。源码检查通过。构建显式更新 browser.aex，未重写旧客机持有的磁盘。复制后的私有磁盘第一次打包因共享 Makefile 新增但本隔离 BUILD 中不存在的 as-typed-capture.aex 而拒绝，失败记录保留；重试仅从临时打包命令移除此无关测试程序，不修改 Makefile，成功保留 5 个用户状态 inode。

```text
a5f85e3b776ba913973b3b19f54154653a923cfc56a93ed944521e045239195c  focus-work.stTdFk/browser.aex
3adae602751da79281ad201e42427508765c2c73039cb0c7a033d32c4f03b7e1  focus-work.stTdFk/disk.img
```

客机驱动新增 `--focus`，本地消费者要求原生 click 触发 blur/focus、跨端口/Worker 往返后仍持有焦点，再用真实键盘 x 绘制 FRAME-KEY-PASS。`guest-frame-focus/focus-key-pass.png` 已目视核对；随后不同 CSP 标签往返也重新完成了消息和画面消费者。公共页面观察及启动控制的实测结果将在下文追加，以上本地门禁不代表公共验证或搜索成功。

本阶段客机最终记录：`guest-frame-focus/results.json` 中五项消费者均 true，公共搜索验收仍为 false；`/parent` 1 次，`/other` 1 次，child/script/Worker/import 各 2 次，body 4 次。PID 75210 保持运行，QMP `/tmp/active-frame-z97m59kw/qmp.sock`，使用上述独立磁盘；旧 PID 28565 的磁盘与进程均未动。页面 `public-observation.png` 显示真实官方 demo 的内嵌复选框，而非挑战题。

在驱动退出、释放 QMP 后，根据已检查截图，仅向新客机的启动框发送一次原生点击（198,542），没有选题或点击 Submit。串口边界为第 80894 字节之后；`public-after-start.png` 已目视核对：复选框仍在，尚未出现题目。新边界中未重现 `focus is not a function`，仍有 3 次 `cannot read property 'parse' of undefined`。这支持“焦点异常不再重现”，不支持“验证完成”或“已能人工做题”。启动前已经存在同类 parse 异常及超时，因此也不能仅凭点击后日志认定全部由本次点击引发。

下一缺口尚未定位 receiver：源码确认旧同源 iframe 的 `makeWindow()` 仍返回不转发新 realm 内建对象的外观对象，而子页面的 legacy-frame installer 仍被隔离关闭；这是需要独立最小消费者验证的候选原因，不是 parse 异常的已证实根因。不能把父页面 JSON 塞入其中冒充独立 realm，也不能据此声称已支持同源跨文档访问。本阶段未改这些接口。

## 第九阶段：辅助 iframe 的独立内建对象

保留上段尚未定位的结论作为当时证据边界；本阶段进一步只检查公开脚本中 parse 接收者的 API 路径，确认回调获取 iframe.contentWindow.JSON。没有分析或实现挑战答案、信号伪装、令牌或挑战算法。通用本地消费者随即复现缺口：network child 未安装 DOMParser，空白 iframe 文档为 null；即使顶层拥有解析文档，makeWindow 的外观对象也不转发该文档的独立 ECMAScript 内建对象。

实现：DOMParser 的节点/文档原型从进程全局借用指针移到每个 JSContext 的 class-prototype 槽，再为 network child 启用它。辅助 iframe 使用 creator 的同一 JSRuntime 上的独立 JSContext，窗口读取转发到该 context 的真实全局对象；JSON、Object、Array 和 parse 结果的原型不是父页面复制品。窗口按当前 r.doc 获取对象，因此 srcdoc 导航保留窗口 identity、替换文档/内建对象，移除后不会通过旧窗口重新创建 realm。补上子页面缺失 src/srcdoc IDL descriptor 时的真实属性反射/导航，并让 about:blank 继承创建者语境，而不把 URL.origin 的 null 当成跨源网络地址。

js_frame 每个 creator 保留 8 个辅助 realm 上限，全进程最多 64 个；清理按 creator，而非子页面关闭时清空父兄弟全部 context。普通 legacy 子文档保留原有脚本执行路径；network child 创建的辅助 realm 只供独立内建对象访问，不启用旧版嵌入脚本/DOM 路径。新增 QuickJS 只读 code-generation policy getter，使新 realm 继承实际策略；meta 策略收紧同步更新既有辅助 context，包括已取得的 Function 构造器。此处没有扩大 CSP 许可。

仍非完整 WindowProxy：属性描述符/枚举转发、辅助 realm 内的 document/DOM 与事件循环、嵌套网络 iframe 像素仍未实现。原有 postMessage 空操作外观不被当成本阶段成果，跨源真实网络文档继续走原生端口桥。不能据本阶段称已支持任意同源跨文档访问。

测试仪器先修两处真实缺陷：fake_site_add 保留同 URL 的首次样本，最初正对照仍执行普通 465 字节脚本；已改为重置记录后添加实际消费者，后续门禁消费独立 realm 的完整脚本。另，后续 make fragment 对递归源码变量追加 js_platform.c，recipe 能看到、先前展开的 prerequisites 看不到；为本门禁显式补上修改的 TU，避免平台源码变更后运行旧二进制。早期失败日志保留，不能用 first-positive 的 PASS 替代最终证据。

`frame-realm-fresh-gates.log`：active-realm 普通及 ASan/UBSan 均 PASS，要求真实父子消息/点击/画面、JSON.parse 返回对象归属独立原型、原型污染隔离、窗口别名、CSP 拒绝、导航换 realm、移除不复活、12 次顺序创建释放及父 DOMParser 不串 context。FRAME_NO_REALM_GLOBALS 负对照实际报 fresh intrinsics，并让画面、父消息、父辅助 realm 共 3 项断言失败，是正门禁先决条件。

`frame-realm-build-final.log` 的 test-frame 既有脚本执行/释放负对照和正对照通过，新增 6 项独立 runtime/策略继承/保留构造器/关闭 child 后 parent 仍可用的检查通过。还修复该原有 standalone 链接门禁：DOMParser 到 live DOM 的 import/adopt 接口改为真正可选；无 live DOM 依赖时不暴露接口，不返回假成功。`frame-realm-regression.log` 中 Page contexts 34/0，frame-bootstrap-wiring 的 parser/dynamic srcdoc、sandbox 和 malformed URL 负对照及正对照通过。`frame-realm-wiring.log` 378/377/1。

制品来自显式 browser.aex 重建，新私有磁盘保留 5 个用户状态 inode，未改已有客机背板：

```text
0fec9afe3f9f9021776d89e3d203a411e5492a4503b8ba7dc68006bd76c1a83f  realm-work.3Avx7r/browser.aex
0297ceba20155c902e53b487be650c1b418add62218afb89d0928549402a4588  realm-work.3Avx7r/disk.img
```

`active_frame_guest.py --realms --large-script --tab-roundtrip` 要求父、子各一次真实辅助 realm 消费，以及原有焦点、键盘、端口、Worker 和标签恢复绘制。新客机 PID 9349 的本地消费者已通过，公共页面的最终观察在下文追加；在实测之前不声称 parse 错误消失或挑战出现。

`guest-frame-realms/results.json` 最终本地六项消费者均 true，公共搜索标志仍 false。PID 9349 / QMP `/tmp/active-frame-ch9rprw0/qmp.sock` 留在可见窗口。官方 demo 的新日志没有重现 parse 或 focus 缺失，但初始化有 reCAPTCHA Timeout。第一次点击（串口边界 72242 字节）发生在驱动 90 秒观察之后，未出现题目，因此不能排除控件已经超时。

为排除此测试因素，在同一活客机中仅新加载一次 demo。35 秒内未等到新的 robot 绘制串口标记，观察脚本超时；没有据此重启或替换客机。随后同一个 QMP 的截图确认控件已经显示，才向 (198,542) 做一次启动点击（边界 94516 字节）。`fresh-after-start.png` 目视确认真实复选框变成蓝色加载圈：原生启动事件执行，但仍无题目。该边界后又有两次 reCAPTCHA Timeout，无 parse/focus 缺失、无新的明确网络请求记录。这里的超时原因仍未定位，不能直接归咎服务端或宣称通信已经成功；也未完全证明“刚就绪即点击”，因为串口就绪等待本身未命中。

本阶段只做上述两次普通启动点击，未选择挑战答案、未提交表单、未自动刷新重试。保留新现场，下一项应查启动后的异步通信/初始化链路；Worker.postMessage 非空 transfer 仍为显式拒绝的已知能力边界，但尚未证明它就是本次超时的原因。当前目标仍未完成，不能把蓝色圈当成可手工作答的验证码。

## 第十阶段：消息预算诊断与大消息消费者

2026-09-16 续接时，`build-active-frame` 已位于 `/Users/wangzhe/.Trash/`，旧客机仍持有该目录中的文件。本轮没有移动或删除它们；新制品与证据在 `build-frame-budget/`。只读核实旧诊断客机 30066 的实际文件句柄及日志，确认原生父子端口有双向消息往返，随后 `port-error kind=byte-budget`。不能把笼统的 clone-refused 当成“不支持对象类型”。Worker 的第二参数被忽略也有记录，但当时不足以证明其内容非空或它就是超时根因。

新增 opt-in 元数据诊断只输出固定类别、字节数、队列/所有权状态，不输出消息、来源参数、验证内容或令牌。`guest-before/serial.log` 实测被拒绝消息 **300,617 字节**，retained=0、limit=65,536、total=8,388,608；因此是单条上限，而非队列总量耗尽。随后仍有超时。只向已目视确认的启动框做一次普通点击，未答题或提交。

通用端口单条预算从 64 KiB 改为 **1 MiB**，总量仍 **8 MiB**，队列长度与端点数量不变。这些数限制保留的序列化包，不声称限制 `JS_WriteObject` 的瞬时分配；序列化仍受 runtime 内存限制。新 host 消费者验证 256 KiB 缓冲长度和边界字节、异步性、2 MiB 拒绝后端口仍归发送者、累计预算以及丢弃后预算回收。普通及 ASan/UBSan 均 **32/0**；恢复 64 KiB 的负对照 **32/4**，原不分离及不关闭负对照仍按预期失败，都是正门禁先决条件。诊断消费者六项检查通过；test-mk-wired 为 378/377/1。

`guest-after/results.json` 新增双向 300 KiB 真端口消费者，原生点击后父子绘制、Worker、焦点键盘和独立 realm 均通过。串口真实排队及派发两份 **307,206 字节**包；官方组件随后 **301,065 字节**消息也成功入队、派发，不再出现该次预算拒绝。但页面仍超时，点击后也未确认题目；因此只证明排除了一个实际阻塞，不证明验证成功。此时制品：

```text
a7d853bfa083280b56fb8c1da7b7a0d4f1a5d1a42d92642b89960448131dff64  after/browser.aex
3d1d7f541d004c6a721ceb189f264cdb1c70ce04f7928968ba32320596278bcd  after/disk.img
```

## 第十一阶段：Worker 返回 MessagePort 的所有权转移

修正旧注释“非空 transfer 一律拒绝”：此前只有父到 Worker 显式拒绝，Worker 自身的 `postMessage` C 绑定实际上忽略第二参数。现为 Worker 到父文档接入原生 prepare/commit/read 包：所有克隆、分配及 author 属性读取先于分离；排队时只持有序列化字节与原生在途端点，直到父任务派发才创建父 realm 包装。取消/close/document teardown 丢弃包并关闭在途端点，避免提前创建包装后取消消息却遗留端点。接收者通过冻结的 `MessageEvent.ports` 取得端口。

范围明确：父到 Worker 的非空 transfer 仍显式拒绝；ArrayBuffer 转移、端口嵌入 data 图仍拒绝，未声称完整 Worker transfer 或完整 MessageEvent 语义。未改验证算法、浏览器身份、CSP 或来源隔离。

新 `test-worker-ports` 普通及 ASan/UBSan 各 **7/0**：异步传递、接收 realm/冻结数组、转移前排队与端口往返、克隆/重复列表拒绝不分离、270 次关闭丢弃后不耗尽 256 端点池、文档关闭无残留任务。旧忽略参数负对照 **7/3**；漏掉包释放负对照 **7/1**，命中端点耗尽检查；均为正门禁先决条件。真实客机新增 `--worker-port`，要求 Worker 将端口转给子文档，ready 与原生点击回声必须经过该端口才绘制 PASS。客机与公共页面结果在完成后追加，不能以 host 测试提前宣称验证码可用。

### 第十一阶段客机结果与尚未通过的边界

`worker-context-regression.log` 普通及 ASan/UBSan **31/0**；既有 `test-worker` **28/0**（含其 terminate/silent-error 负对照）。`guest-worker/results.json` 本地七项消费者均 true，包括 Worker 返回端口、双向 300 KiB、焦点键盘和辅助 realm；`embedded-pass.png` 与 `focus-key-pass.png` 是对应客机画面，公共搜索标志仍为 false。

```text
3106febc1b482c0a6277c5a10fa3e0c2840db3565faa323a5da329bce9826f96  worker/browser.aex
e30358eaa7df8ceeb7f5d9f162fc51766f0514b5c4c49ef47fb1d5fc9011c266  worker/disk.img
```

当前新窗口 PID **22533** / QMP `/tmp/active-frame-nbr94j15/qmp.sock`；所有路径均相对新 `build-frame-budget/`。首次普通公共导航中 api.js 因 `no source address for any dst` 失败，`public-observation.png` 只有 demo 表单，没有验证框，不能把此次失败归因于 Worker 修复。在同客机普通 Ctrl+R 一次（串口边界 58,754）后，846,252 字节子脚本、36,291 字节内联脚本与 102 字节网络 Worker 均加载。`after-reload.png` 已目视确认内嵌复选框。

真实组件这次使用了新 outbound native-packet；端口 11/12 均 start，随后 Worker 端派发 **1,145 字节**，父端派发 **48 字节**回复，证明新增边界有实际公共消费者，不只是空数组被接受。只向可见启动框做一次原生点击（串口边界 **89,195**），未选择题目、未提交表单。`after-start.png` 仍只有勾选框，没有题目，并出现两次 reCAPTCHA Timeout。未观察到新的 port-error/port-budget 或端口回调异常；不能据此排除其它异步/API/网络缺口。**验证码题目和 Google 搜索验收仍未完成**；窗口保留，未标记目标完成，未自动刷新重试或处理验证答案。

## 第十二阶段：Worker 时间上限定位与 libc 记账事务（仍未弹出题目）

补查上一阶段串口发现确实有 Worker watchdog 中断，不能再把 Timeout 只当作未知 API/网络问题。新增 opt-in `worker-budget` / `worker-job` 元数据，仅输出固定阶段、客机时长、轮询数、最大间隔、完成/中断状态，不记录消息内容或验证数据；普通构建不含这些诊断。原 **8,000 ms / 400,000 polls** 上限没有修改。每 poll 对应 10,000 次分支/调用，不是字节码数。合成时钟门禁 `test-worker-budget-diagnostics` **3/0**，有限 Promise 正常完成、无限循环仍被中断；关闭诊断负对照明确输出 `FAIL: Worker budget rail has bounded numeric execution metadata`，并作为正门禁先决条件。

诊断装置也出过两次错误，均保留日志：最初每次读时钟就跳 1 秒，导致外层 12 ms 调度器根本无法启动 Worker；改为每 32 次跳变并保持推进到 Worker 结束。新分配器测试最初错误地链接私有 `malloc_cur`，改为测试专用翻译单元包含真实 allocator 后观察其内部计数，没有为了测试给产品导出该全局变量。

参考客机 `guest-profile/serial.log` 两次命中 job 时间上限：**8,010 ms / 799 polls** 和 **8,010 ms / 797 polls**，max-gap 都为 50 ms。只读 QMP 用户态 RIP 定位（`vm-samples.log`，匹配当时 ELF）显示 `JS_CallInternal`、`malloc_usable_size`、分配/释放和锁路径为热点；这不是 CPU 百分比或速度测试。旧 live build 仍在 Trash，未恢复或覆盖它；本阶段都在 `build-frame-budget/` 私有磁盘上。

### 一次加锁完成分配/释放与容量记账

libc 新增内部 `__libc_malloc_size`、`__libc_realloc_size`、`__libc_free_size`，把操作与取得真实容量放入同一锁事务。保留所有 header 检查、边界、线程锁和失败语义；free 返回合并相邻空闲块**之前**的容量，失败容量为 0。QuickJS 的 LogitOS 默认 allocator 消费它，其他 host 与 `JS_NewRuntime2` 自定义 allocator 不变。不增加未受保护的 header 缓存，不把请求大小当成 usable-size。

`test-malloc-sized` 的 split/fused 两种路径各 **15/0**，ASan/UBSan 各 **15/0**：四线程×4,000 次分配/增长/逐字节检查、强制搬移、缩小、溢出拒绝保留旧内存、零尺寸释放、合并前容量、QuickJS 计算结果/配额/完整释放。相同 JS 工作负载的锁事务 **3,329,510 → 1,742,898**；两者 `QJS_ACCOUNT count=881 size=70080 result=72078000` 完全一致。负对照使用未融合 split 路径，明确输出 `FAIL: QuickJS allocator transactions: 3329510 -> 3329510`。`test-malloc` 原有 ASan/UBSan 回归 0 failures；`test-worker-ports-san` 7/0（含既有负对照），mk-wired **378 fragments / 377 reachable / 1 declared**。

### 客机对照与公共页面边界

本地 `--worker-bench` 只在自有 fixture 的 Worker 中运行：先暖机，再做五轮相同 4,000×12 对象分配，校验 sum=8,026,000，使用客机 Date.now 包围计算，不计网络与宿主输入等待。每轮单独任务，原 watchdog 仍生效。所有轮次保留在对应 `results.json`：

| 客机 | 五轮 ms | 中位 ms |
| --- | --- | --- |
| guest-fused（首次） | 260, 220, 220, 260, 310 | 260 |
| bench-split | 270, 260, 260, 280, 310 | 270 |
| bench-fused | 230, 230, 230, 240, 220 | 230 |
| bench-split-repeat | 260, 250, 260, 260, 260 | 260 |

只能说明这个受控消费者有改善迹象，首次样本存在重叠，不能据此宣称浏览器或验证码整体加速某个百分比。完整本地跨 origin iframe、Worker 端口、双向 300 KiB、辅助 realm、焦点/原生键盘都通过。公共 demo 首次 `guest-fused` 未出现 watchdog，并返回 **284,152 字节** Worker 回复，但回复前已出现 Timeout；点击可见启动框一次后仍没有题目。诊断复测 `guest-job-diag` 又记录 **8,010 ms / 1,055 polls** 的时间上限，单 job **8,060 ms** 后 interrupted，随后仍有两个 Timeout。因此，之前“一次未中断”不能升级为“中断已修复”。没有调整站点计时器、伪造信号、处理答案或提交表单。

```text
46f95037dd3385080342fc9544a4a9304784516d14eed12b0da54517cb587ef5  fused/browser.aex
a0a797b667ac89e2b2f3491096cddd544998966228421e2ac4836f85e0f75f01  fused/disk.img
6719b233789506e0bcb8cc57a37fefbac8358aeca7265f12a28a39b9ec9a1444  job-diag/browser.aex
262ec9632646b9db181d91128e8074ec6b24d6b1e324fe285f7376586494c838  job-diag/disk.img
```

以上公共观测没有验证码题目，没有搜索结果页验收；`public_search_accepted` 保持 false。剩余明确问题是长 Promise 工作与端到端响应超时，不应通过放宽 watchdog 或伪装站点特征掩盖。最终源码还将 job 诊断格式改为与 64 位 poll 计数匹配的 `%lld`，不改变执行路径；最终私有打包与本地消费者结果在后文记录。

最终 `final/browser.aex` SHA256 `e5d0867f77a4f554adf425a49fbe988959658bca92f124d6325afd7ad0b7d152`；`final/disk.img` SHA256 `8386ef6081789cf6ac403e60ffa0a2750befc779c0d433922c7f9346afd432df`。`guest-final/results.json` 七项本地消费者均 true，客机已正常退出；没有在最终 headless 测试中重复请求公共页面。保留公共观测窗口 PID **20317** / `/tmp/active-frame-rs617h3v/qmp.sock` 与 PID **33302** / `/tmp/active-frame-fa5p6bq8/qmp.sock`。最终 `final-regression.log` 含 Worker ports 普通和 ASan/UBSan 各 7/0、budget 3/0 对应检查以及 mk-wired 通过；诊断截图仍只有嵌入复选框。普通默认磁盘未被覆盖，未改动并行 AetherScript 工作。

## 第十三阶段：分配器候选优化与原生点击取证（题目仍未出现）

以下路径仍相对 `build-frame-budget/`。本阶段没有启用默认小块缓存或默认 LTO，没有改站点脚本、时钟、watchdog、浏览器身份、CSP 或来源检查。

### 先排除构建与测试装置问题

上一阶段 sized allocator 的 `LOGIT_OS` 条件过宽：host 语义探针也定义它，却不链接 mini-libc，导致 `__libc_malloc_size` 等未定义（`hostguard-before.log`）。修正为 freestanding 的 `LOGIT_OS && __STDC_HOSTED__ == 0`，保留专门测试开关。`test-malloc-sized-hostguard` 恢复 host 链接；强制在 host 使用该 API 的负对照必须链接失败，并成为正门禁先决条件。

这不等于 JS 语义门禁通过：`hostguard-after.log` 与 Node 对照仍有 12 个非控制用例差异及预期的 99-control 差异。单独比较 baseline 与 `-O3 -flto`，19 个用例的 stdout/stderr/返回码一致（`lto-language-parity-fixed.log`）；只证明候选与当前实现一致，不证明 Node 兼容。

LTO host 检测最初因 Mach-O 弱 stub 与测试强锁提供者重复定义失败；只在 `malloc_sized_arena.c` 测试包装中禁止那两个 fallback，不更改产品弱符号。四线程 churn 后缓存布局依赖调度，不能要求其后两次 JS usable-size 自然一致；现在先显式排空缓存，再比较记账。`malloc_test.c` 的损坏检测曾假设连续三次分配地址递增，缓存并不承诺地址顺序；改为先按地址排序再执行原来的有界损坏检测，没有删除检测。初始及修正日志都保留。

### 只读热点定位与有界小块复用

`active_frame_guest.py --pc-profile-elf` 在启动前校验相邻 `.aex` 内 ELF 字节与传入 ELF 完全一致，错误 ELF 负对照退出 2，未启动客机。QMP 只保留 CPL3 的浏览器 RIP，不保存其它寄存器。`pc-trace/source-hotspots.log` 的 974 次查询取得 Worker running 前 143、之后 752 个样本，后段包含 JS_CallInternal、malloc/free、bin/trim 和锁路径。**后段包含主页面及 Worker，不是 Worker 专属 CPU 占比，也不是性能基准**；DWARF 名字还可能是内联函数。

实验宏 `MALLOC_SMALL_CACHE`（默认关闭）让 16..512 字节的 32 个 exact class 每类最多缓存 8 块：最多 256 块、67,584 payload 字节，加 header 共 71,680 字节。仍使用原 heap 锁、带 seal 的独立 cached tag、检查过的 arena-relative 链；cached 块不是活分配，usable-size 和 double-free 路径不会将其当成 USED。普通分配无可用块时排空缓存并合并，再作一次正常分配。rebuild 丢弃缓存链，按检查过的物理链恢复，不信任释放后 payload 链。

新增回归还抓到一个候选缺陷：8 个 cached 64-byte 块位于低地址时，普通 free tail 已在 512-byte ceiling 之上，256-byte 请求提前失败。`small-ceiling-before.log` 明确 **10 checks / 1 failure**。现在普通候选不满足 commit ceiling 时也先排空，再通过原 ceiling 检查重试；不提高或跳过上限。排空后 `small_total=0`，不会无限重试。

最终 `test-malloc-small` ASan/UBSan **10/0**；禁用排空负对照 **10/2**（整堆回收、ceiling 下前缀回收），跳过 cached reuse bound 的负对照 **10/1**，均作为正门禁先决条件。实际 QuickJS 消费者 split/fused 两路各 15/0，记账一致 `count=881 size=69376 result=72078000`，锁事务 3,329,490 → 1,742,883。默认 allocator 与 cache-enabled 的完整原有 allocator ASan/UBSan 门禁分别见 `malloc-final-default.log` / `malloc-final-cache.log`，均 0 failures。缓存只在私有候选中启用。

### 客机测量与仍然失败的公共观察

私有 LTO 候选仅将 QuickJS、malloc、pthread、string 编译成 `-O3 -flto`，链接 `--lto-O3`，其它对象不变。没有把全仓库切为新优化参数。受控本地 Worker 同一分配任务、客机时钟五轮结果：

| 构建 | 五轮 ms | 中位 ms | 公共观察 |
| --- | --- | --- | --- |
| guest-lto | 210,210,200,200,210 | 210 | 8,010 ms / 952 polls 中断 |
| guest-small | 200,270,270,250,210 | 250 | 8,010 ms / 1,033 polls 中断 |
| guest-small-lto | 170,170,170,180,170 | 170 | 首 job 6,810 ms 完成，但后续仍超时 |

不能把这些连续、受并行宿主负载影响的样本转换成浏览器整体加速百分比。`guest-small-lto/serial.log` 先记录 job 完成，随后两次 reCAPTCHA Timeout，再记录 3,660 ms job 完成及 **285,614 字节**端口回复。先前“本次没有 watchdog”不能推出页面成功，更不能推出稳定修复；`guest-small-input` 复测仍为 **8,010 ms / 1,058 polls** watchdog，job 8,050 ms interrupted。

### 点击已到达，不再把无题目归因于坐标

新增 opt-in `frame-input` 仅记录固定事件名、slot、坐标、node serial、最近 role=checkbox 祖先 serial、是否允许默认动作及是否派发 click；不记录 author id/class/text、URL 或消息。先于脚本派发保存节点 serial，避免 handler 删除节点后诊断再解引用。每进程最多 64 行，普通构建不包含它。`test-frame-transport-diag` 现在明确要求原生 mouseup 的 click=1，七项检查通过；关闭诊断的负对照包含 `FAIL: native embedded click observable`，不只是宽松接受新日志语法。

`guest-small-input/serial.log` 本地 fixture 为 hit=9 / click=1；公共启动点击（串口边界 63,071）随后记录 down/up 均 hit=88、checkbox=87，down allowed=0，up allowed=1、click=1。起初短观察窗口未见日志，后来确实出现，因此**不是已证明的丢失点击或坐标错误**。只能确认输入完成，不能由此推断服务端为何没有给题目。长 Worker 与页面共用线程导致输入延迟是已知实现限制；端到端超时的全部因果链尚未证明。

之后只开启已有的 `about:input` 非导航诊断，没有再次点公共控件、没有答题或 Submit。`guest-small-input/latest-state.png` 仍是 demo 内嵌复选框；地址栏保留诊断命令，不表示文档被替换。保留当前窗口 PID **80839** / `/tmp/active-frame-kkrpdbmt/qmp.sock`。四个本阶段已取证的私有测试实例 45766、50323、59817、68912 经 PID/磁盘/QMP 三项匹配后正常 quit，日志与磁盘保留（`phase13-owned-cleanup.log`）；之前阶段的窗口和其它服务未动。

最终 host 汇总 `phase13-close-gates.log`：缓存及 sized 消费者、host guard、frame transport 七项、Worker budget、mk-wired 均通过，包含各自预期失败的负对照。最终私有制品 `small-final/` 含 ceiling 回收修复；不覆盖默认磁盘，不再次请求公共验证服务。其本地客机验收结果完成后追加。

最终打包的第一次尝试被 disk guard 拒绝：打包与本轮哈希读取错误地并行，报磁盘由 PID 94959 打开。外层命令又未将 pack 失败作为启动前提，`guest-small-final` 因而启动复制来的旧浏览器，在 aux realm intrinsics 检查失败。没有绕过保护，也不能把这个失败算成新候选的语义回归。改为 `set -e` / `pipefail` 的单独打包步骤，确认成功后才哈希、启动；原失败日志保留。

`small-final/pack-retry.log` 明确打包 325 文件 / 357 inode，保留 5 个 user-state inode。额外从新打包镜像读取实际 `/browser.aex`（不是 `/bin/browser.aex`），逐字节与候选比较，通过结果在 `small-final/packed-browser-check.log`；不再仅信任镜像旁的文件哈希。

`guest-small-final-retry/results.json` 本地七项消费者全部 true，Worker 五轮为 **170,180,170,170,190 ms**，中位 170 ms；这是本地有界分配消费者，不是验证码整体速度。测试客机已退出，`public_search_accepted` 仍 false，本次没有公共导航。对应制品：

```text
fd1957cc13f71ec51d501a57045d9ec76c9736da225b706ec634d3b09fb0ace9  small-final/browser.aex
d2b260cac9d8620a04dc8514a202decbd18a3a56aefd6239c8992395630160bc  small-final/disk.img
```

当前验收边界不变：原生 iframe 输入、端口及本地 Worker 消费者已取证，**公共验证码题目尚未出现，Google 搜索页面仍未验收**。默认构建未启用实验缓存/LTO，普通默认磁盘未覆盖。下一步应定位长任务到回复的端到端调度时序，而不是根据 checkbox 或局部 benchmark 宣称目标完成。
