# A2 → A3 功能迁移

## 完成标准

用户要求的迁移是：**A2 能完成的编程任务，A3 也必须具备对应的语言能力、运行时和标准库，并能独立原生执行。**

此前 A1 → A2 的脚本声明、数值修正和缓存重建只完成了旧工具链内部的迁移，不能作为 A2 → A3 功能完成的证据。完整静态语言继续遵循已经批准的类型、布局和显式底层规则；所有权、动态值和异常语法有明确迁移边界。

下面按原有解析器/VM、原生内建函数和 `fsroot/as/lib` 分组对照。此前的阶段策略是“A2 继续保留”；2026-09-15 用户已将最终目标改为 **只支持 A3，删除全部 A1/A2 引擎、缓存及工具链**。迁移中的旧文件仍是尚未完成的阻塞项，不能作为最终兼容方案。单个子集通过、拒绝未支持语法、增加版本声明，都不能把整项记为完成。

## 功能对照

下表是迁移开始时的历史盘点，保留当时的缺口描述。当前实现与实际验收见
下方按时间更新的“本批实际补齐的能力”，以及最新 A3-only 审计报告。

| A2 能力 | A3 当前实现 | 仍须完成 |
| --- | --- | --- |
| 数值、布尔、位运算、转换 | 静态位宽、检查算术、显式回绕加减乘及左移、移位与整数幂 | 完整数值内建 API 和库包装 |
| 函数、递归、控制流 | 类型推断、泛型实例化、原生 Callable 和回调、if/while/for、break/continue、短路、条件表达式 | 默认参数、完整调用形式、解构等用法对齐 |
| 模块与变量 | 多文件检查、模块变量、显式 `global`、初始化次序及提前读取检查、标准库查找、快照及符号 | 版本化发布与完整 Studio 服务接入 |
| 文本 | 原生动态字符串、拼接/重复、字节索引、slice/sub、替换/拆分/连接、ASCII 大小写及 strip、比较/查找、原生值/容器格式化与标量解析 | 格式化字符串及其余文本接口 |
| 容器 | 连续结构体、定长数组、只读切片、同类型 List/Dict、None/Optional、显式 Any 装箱、相等比较和受检查取值 | Bytes、完整键类型、推导式、其余容器方法和 Any 动态算术/调用 |
| 对象和内存 | 直接布局值、固定字段 class、构造器、绑定方法和组合、精确非移动 GC、编译器生成的栈根/模块根和字段扫描 | 兼容所需继承策略、泛型方法、完整借用规则 |
| 闭包 | 普通及泛型函数值、原生间接调用、回调签名和异常传播 | lambda、捕获、闭包生命周期 |
| 异常 | 原生 typed except、主动抛出、重抛、跨调用传播、错误位置与值记录 | 自定义异常类及层级、与 GC/资源清理整合 |
| 文件、进程、ports | 已有异常传播基础 | 文件/进程接口、管道/重定向、`with` 各出口清理、唯一资源所有权 |
| 底层与系统 | 用户态原生 AEX、显式布局值的基础 | 原始指针、Region/借用、C ABI、unsafe、capabilities、系统包装 |
| 标准库 | 原路径重写 strings、paths、bits、random、mathx、math、sets、stats、seq、dicts、test、aslex，公开接口有宿主原生运行覆盖 | 其余 5 个模块及其依赖逐项移植；不能直接导入 VM 库来充数 |
| 开发工具与自举 | check/build/run/test、结构化结果、宿主交叉编译 | Studio 完整语义服务、宿主连接、调试、A3 自举与系统内编译 |

## 本批实际补齐的能力

### 2026-09-16：结构体/类成员补全迁入统一前端

Studio 的 A3 结构体字段和类方法补全现在来自真实类型检查结果。
`sema/completion_object.c` 单独负责对象成员，使用现有类方法解析器
处理继承覆盖与私有方法。解析器保存成员点号和字段声明 token，避免
节点被解析成全局变量、函数或方法之后丢失查询位置；继承字段定位到
基类实际声明的模块，而不是派生类名称。

覆盖变量、构造器、链式字段、下标、泛型函数返回值以及 `super`。
未完成成员表达式通过恢复后的函数体执行普通类型检查；不会执行
用户初始化代码。泛型调用过去遇到项目中任意错误就丢弃结果类型，
现在仅在该调用产生新错误时停止推断，普通构建仍拒绝含错误的工程。
模块别名被全局变量遮蔽后，即使接收者已转换为全局节点，也不会
回退为旧模块导出猜测。

验证记录位于 `build/a3-cutover/`：

- `object-completion-regression.log`：217 项引擎、33 项绘制检查，
  模块/方法可见性的故障注入，以及泛型、类、继承 O0/O2 原生回归。
- `object-completion-prefixes.log`：ASan/UBSan 下逐个输入前缀、
  未完成表达式、格式化字符串表达式、私有方法及跨模块声明位置。
- `object-completion-guest-final/result.json`：真实来宾 16 项检查，包括
  方法候选显示、鼠标插入和保存后的源码字节。

局部名称、内建容器成员和显式布局对象仍需继续迁移。未闭合字符串
仍使当前词法器放弃整个模块，因此返回空候选；不能宣称已完成全部
错误恢复。最新 `a3-cutover-object-completion.json` 仍有 46 项阻塞记录，
其中 19 个源码尚未迁入 A3；旧引擎、自举和兼容缓存仍未完成删除。

### 2026-09-16：Studio 补全生命周期与 A3 模块查询

后台检查曾在启动时清空候选，使补全在输入停顿后消失。现在检查保留
候选；绘制和点击共用弹窗矩形，点击候选插入、点击外部或 Esc 关闭。
异步查询带请求代次及源码校验，关闭后迟到的结果不会重新弹出。

`as complete` 从 A3 前端的实际导入、作用域和声明查询模块公开成员，
支持导入别名及未保存模块。完整函数名经类型检查变为函数节点之后，
仍能查询；已识别模块的空结果不会回退为旧补全猜测。局部名称、对象
方法及更完整的语言服务仍待迁移，不能据此宣称 Studio 已全部接入 A3。

证据：`build/a3-cutover/completion-final-regression.log` 中 208 项引擎、
33 项绘制检查及对应故障注入通过；语义查询也通过 ASan/UBSan。
`completion-version-regression.log` 验证版本/数值和源码快照未回退。
`completion-semantic-guest-v2/result.json` 记录真实来宾的弹窗保持、
点击插入、外部点击/Esc 关闭、未保存模块补全及实际保存字节验证。
首次扩展 UI 门禁时 QMP 的末尾按键尚未消费便切换标签，污染了下一个
文件的版本声明；门禁增加输入处理等待并保留失败截图，没有修改产品
来适应测试时序。

### 2026-09-16：二进制持久化工具 durcheck 迁入 A3

`fsroot/as/examples/durcheck.as` 在原路径重写为静态 A3，任意二进制数据
使用 `Bytes` 和有边界检查的 `Buffer`。保持五种原始大小、逐字节公式、
256 个缓存块、命令名称、输出及 300 次写入循环；旧磁盘数据仍能验证。
读写、删除和内容不符现在返回失败状态，不会在失败之后报告 DONE。
删除 syscall 显式使用有所有者的 NUL 终止路径，普通文本不冒充 C 字符串。

durability、hugefile、fscrash 和 mmtrace 的对应入口改为安装后的
`/usr/as/bin/durcheck.aex`。mmtrace 中已经迁移的 fib 也改用原生 AEX；
旧解释器的页面轨迹仍属于旧产物，不作为新原生程序的内存基线。

证据位于 `build/a3-cutover/`：

- `durability-native-host.log`：O0/O2 与 ASan/UBSan，验证五种大小的
  实际文件字节、同长度损坏的首个位置、长度错误、缺失文件、写入及
  删除失败、12 次创建/删除和完整 300 轮命令。仅删除 syscall 的宿主
  传输适配为真实 `unlink`；文件读写、生成和比对都运行 A3 代码。
- 同一门禁先实际破坏内容比对，确认“只检查长度”的错误版本被拒绝；
  负对照是正门禁的 Make 前置依赖。
- `durability-native-guest/result.json`：调试/发布两种产物，各在独立
  磁盘启动两次，合计 70 次命令调用。五种大小全部比较真实磁盘字节，
  包括 4,400,000 字节、重启后复验、创建/删除后复验和错误退出码。
  私有磁盘无 VM 和旧缓存，保存源码快照、产物哈希、输出和磁盘。
- `durability-build-wiring.log`：生产 AEX 构建规则和 Make 接线检查通过；
  原测试脚本通过语法检查。原有全套断电恢复脚本本轮未重跑，不能将
  上述干净重启结果扩展为断电恢复证明。

审计 `a3-cutover-durability.json` 仍为失败：剩余 46 个阻塞项，其中
19 个源码仍未迁入 A3。durcheck 已通过原生降低，当前打包选中 27 个
A3 示例；旧引擎和自举尚未删除，完整目标仍未完成。

### 2026-09-16：Studio 工程快照与补全弹窗

检查进程改为接收全部打开标签的不可变源码副本，A3 前端可读取未保存
的导入模块。结果校验入口和其他标签的版本及内容；错误列表记录真实
模块路径，点击导入模块的错误会切换到对应文件，中文位置使用该文件
自己的 UTF-8 字节边界。未打开的磁盘模块仍用字节数和 FNV 校验，
完整磁盘快照和共享语义补全仍未交付。

补全曾在输入后约 850 ms 被自动检查清空；现在检查启动与完成均保留
当前候选。候选框绘制、滚动行和鼠标命中共用布局；点击候选采用，
点击外部或 Esc 关闭，取消待执行补全，外部点击继续送给原控件。

真实截图又发现问题面板拒绝检查结果：LogitOS libc 的 `waitpid`
返回原始退出码，Studio 却直接套用 POSIX 状态宏。转换现在只发生在
Studio 接收状态的边界，不改变 libc 或其他程序的约定。

验证记录位于 `build/a3-cutover/`：

- `studio-popup-gates.log`：190 项引擎检查、33 项绘制/点击检查；
  8 项引擎源代码负对照及 5 项渲染/点击负对照均观察到指定失败。
- `studio-snapshot-final.log`：四个 CLI 命令、未保存导入、中文范围、
  12 种损坏输入，以及 ASan/UBSan 下每个截断字节前缀；构建接线检查通过。
- `studio-popup-guest-v2/result.json`：真实来宾输入 `pri`，候选保持
  四秒并跨过后台检查；问题面板实际显示错误；点击写入 `print` 并保存；
  外部点击、手动重开和 Esc 关闭均通过。保留截图、磁盘和产物 SHA-256。
- `studio-popup-status-negative/result.json`：修正退出码之前的真实产物
  在同一个问题面板断言上失败。初次补全验证虽然通过，截图暴露了这个
  问题，因此不能把 `studio-popup-guest/` 当作完整诊断接入通过的证据。

以上不代表 A2 已删除、来宾内原生工具链或 Studio 调试闭环完成。

### 2026-09-16：恢复普通构建衔接，迁移设置与 GUI 启动工具

此前记录的根 Makefile 并行编辑阻塞已解除：用户明确确认另一任务
停止编辑后，保留它修复的分目录编译和头文件隔离，补回按源码声明
选择 A3 示例、生成原生 AEX、磁盘依赖和安装映射。原有 `make run`
启动逻辑未改。`AS_TYPED_GUEST_BASE` 提前定义，避免较早加载的
Preview 规则把来宾依赖展开成不存在的 `/preview.aex`。

`setcheck.as` 在原路径迁入 A3，拆成有明确类型的读写、控制、窗口
位置、截断和异常配置写入函数。设置常量引用内核 ABI 的公共清单；
字符串显式添加 NUL，读回长度在构造字符串前校验。保留全部原命令
和输出，失败额外返回非零退出码。截断按 Bytes 复制，可保留不完整
UTF-8 前缀；非法长度在写文件前拒绝。设置及用户桌面测试入口现在
启动原生 `/usr/as/bin/setcheck.aex`。

`chlaunch.as` 同样迁入 A3。启动 Chat 前建立调用方窗口，满足 GUI
调用约束；创建窗口或打开失败不会报告成功。原 Chat QMP 测试入口
已改为启动其原生 AEX。七个 Preview 启动源码通过共同的类型化
association 模块运行，保留原来的媒体路径和实际文件关联入口。

本批重新生成的证据位于 `build/a3-cutover/`：

- `settings-host.log`：O0/O2、ASan/UBSan，覆盖所有命令、读写失败、
  错误读回长度、完整窗口位置值、真实文件截断与异常配置字节。
  将即时提交标志改为 0 的负对照被真实调用参数断言拒绝。
- `settings-native-guest/result.json`：调试和发布各四次全新 QEMU
  启动，同一模式反复使用同一私有磁盘。验证设置/窗口位置重启后
  保留、错误期望的退出码为 1、重新加载、空文件与原异常配置后的
  默认值恢复。两个测试盘均不含 VM。这不代替完整多用户隔离或
  物理断电测试；原用户桌面入口仅完成路由修改与 shell 语法检查。
- `chat-launcher-host.log`：八个启动工具的准确路径、GUI 调用顺序、
  失败返回值均通过 O0/O2；移除窗口创建的负对照被断言拒绝。
  `chat-native-guest/result.json`：两种构建均在无 VM 磁盘上打开
  真实 Chat 窗口，并通过缺失配置和输入框提交后异常配置的原 UI
  测试。截图及串口保存在各模式 shots；未在本批重跑流式模型响应。
- `preview-native-serial/result.json`：两种构建各七种文件关联通过。
  校验真实启动器退出码、接收方文件名/大小/格式以及非空画面；WebM
  明确验证 VP9/Opus 不支持的提示，没有称其解码成功。完整串口和
  音频捕获复制到各模式 receiver-evidence，截图保存在 screenshots。
  该关联测试为单核；解码/计时模式仍为四核。
- **保留失败记录** `preview-native-current/result.json`：首次四核
  验证中所有启动器成功，但内核动画日志插入 WAV 文件名日志，导致
  接收方精确匹配失败。没有放宽文件名断言或把该次失败改成通过；
  改为单核复验关联语义，不以它证明四核串口日志可靠。
- `product-disk-final.log`：普通 `make BUILD=build/a3-cutover
  build/a3-cutover/disk.img` 构建成功；`native-installed.json` 对比
  盘内全部 26 个原生示例与本次构建文件，逐字节一致。普通系统盘
  仍有待删除的旧引擎，不能与不含 VM 的专项测试盘混为一谈。
- `studio-integration.log`：176 项引擎检查和六项负对照通过；
  `migration-regression.log`：354 项现有语言检查、107 个内核 ABI
  常量的原生检查及负对照通过。Studio 的独立补全模型尚未完全
  接入统一语义前端，这些回归结果不能证明该项已经完成。
- `a3-cutover-tools.json`：仍有 **48 项未完成要求、20 个旧源码
  文件**。自举编译器、剩余工具、旧引擎/缓存删除和完整 Studio
  语义服务仍须完成，A3-only 门禁继续失败。

### 2026-09-16：权限工具和文件写入屏障工具迁入 A3

`capcheck.as` 已改为明确类型的 Callable、main 和显式 unsafe 指针读取。
文件通过 with 确定性关闭；只将 PermissionError 归为 denied。旧实现
捕获所有错误，缺失文件也会打印 denied，容易把测试环境错误当成权限
隔离成功。原有正常、限定路径和拒绝行为仍保留，新增零授权检查。

`barriers.as` 现在使用 std.abi、定宽计数和 Bytes 写入。sysinfo 返回
长度在创建文本视图前校验；只接受完整 Barriers 行，重复行、缺失行、
查询失败分别拒绝。写前和写后的查询都验证，失败返回非零退出码，不能
因为工具打印了一句错误后正常退出就通过上层测试。

原 `test-as-cap-os` 接入原生来宾门禁，独立 ISO/disk 入口也已替换为
原生执行。它在磁盘所有权检查后复制输入磁盘，逐个读取已关闭的输出
文件和真实退出码，记录安装产物摘要；不会将不同子进程的串口行混合
成一个成功结果。barrier 独立入口和五个残留的输入/UI 测试启动入口
也已使用统一示例路由，已迁移程序缺失 AEX 时不会回退到 VM。

证据位于 `build/a3-cutover/`：

- `capcheck-host-v2.log`：宿主 O0/O2 的完整、限定目录和零权限通过；
  宿主路径在私有目录重定位，权限初始化只用于测试，文件读写仍真实。
  故意恢复广泛异常捕获会掩盖缺失文件，但完整输出判定拒绝了该版本。
- `capcheck-native-guest/result.json`：原始源码 debug/release，每种
  模式三种真实内核授权均通过；限定目录外拒绝、目录内成功、原始指针
  读取和不能扩大路径均检查完整输出。无 VM 的磁盘上运行，同一产物
  的不收窄运行不能充当收窄证据，测试已实际观察其被判定拒绝。
- `capcheck-prebuilt-guest/result.json`：原 shell 入口在已打包磁盘
  副本上完成相同三种授权检查，记录真实安装 AEX 摘要。
- `barrier-host-complete.log`：O0/O2 验证七类计数/失败情况；故意移除
  文件写入的程序仍输出成功，但被实际文件回读拒绝。错误差值和只有
  成功标记的输出也被拒绝。
- `barrier-native-guest/result.json`：debug 的屏障计数 3 → 6，
  release 为 18 → 21，均增加 3；实际设备声明 writeback cache，文件
  内容完整回读。`barrier-standalone.log`：原独立入口同样通过。这些
  结果证明本次 QEMU 正常写入路径，不证明断电恢复或物理磁盘耐久性。
- `input-ps2-native.log`：原 PS/2 工具在无 VM 测试磁盘上运行 A3
  events，三个按键和一次点击均只交付一次，移动使用窗口内容坐标。
- `input-usb-native-v2.log`：移除 PS/2 控制器后，USB 键盘重复输入、
  Shift、鼠标移动/按钮/滚轮到达原生 events；
  `input-usb-native-negative-v4.log`：拔掉 USB 设备后应用收到零输入。
  完整串口与就绪画面保存在 usb-native-positive-evidence /
  usb-native-negative-evidence，内核、私有磁盘和实际 AEX 摘要见
  usb-native-artifacts.json。此前串口就绪等待失败记录没有删除；改为
  画面判定后又发现精简镜像漏了字体，补齐正式字体清单后才通过。
  阴性 v3 是打包尚未结束就启动造成的缺盘失败，不属于通过证据。
  fetch/socket UI、USB tablet 入口已改原生路由并通过语法检查，本批
  没有运行它们各自的完整网络/平板场景。
- `capcheck-barriers-wiring.log`：测试片段连通性检查通过。
- `a3-cutover-capcheck-barriers.json`：59 项未完成要求、29 个 A2
  源文件。自举、其余工具和旧引擎删除仍未完成；根 Makefile 的并行
  回退衔接仍是下节所述的待处理项，没有以直接运行的结果冒充普通
  Make 打包路径已经通过。

### 2026-09-16：事件示例、输入统计与原生测试入口

`events.as` 和 `evqstat.as` 已在原路径迁入 A3。事件示例使用明确的
main、Optional 事件判空和定宽事件类型转换，保留全部六个输出字段。
统计工具使用 std.abi.sysinfo；负返回值或超出缓冲区容量的长度抛出
IOError，只有有效长度才能构造原始文本视图。

原 QMP 输入测试通过统一示例路由启动 AEX；新增有界输入门禁与完整
原生来宾、打包验收共用实际示例和判定函数。它不代替原队列饱和门禁，
本批没有执行大规模输入压力测试。

本批证据位于 `build/a3-cutover/`：

- `input-host-complete.log`：实际源码在 O0/O2 下通过完整字段与统计
  输出检查，负返回值和过长计数均被拒绝。故意交换按钮/修饰键的源码
  仍正常退出，但字段判定拒绝它；撕裂事件行、重复总结及不完整统计
  也被拒绝，不能只凭最终成功标记通过。
- `input-native-guest-v2/result.json`：debug/release 共四个程序通过，
  实际验证移动、左右键按下/释放、双向滚轮、Shift 及释放后的普通键。
  每个事件进程打印 14 条记录并正常退出；窗口内容坐标与截图中的画布
  原点一致。两次交互的窗口队列计数分别从 0 到 15、15 到 30，语义
  丢失均为零。首次失败保存在 input-native-guest：判定器未考虑内核
  在内容首行上绘制的标题栏分隔线，修正后按真实内容原点验证坐标。
- `input-wiring.log`：Make 测试片段连通性检查通过。
- `a3-cutover-input.json`：63 个未完成项、31 个未迁移的 A2 源文件；
  完整迁移仍未完成，旧引擎不能据这两个示例的通过而删除。

**本批构建衔接仍待处理：**并行任务的提交 a03a6467c / 1caa2fe59
把根 Makefile 恢复为已提交旧目录的路径，并移除了原生示例打包清单，
与这里尚未提交的 A3 重构冲突。普通 Make 构建报缺失 as.c，打包负对照
也正确失败；上述来宾运行是直接调用已有编译器重新编译实际源码。
`restore-a3-build.patch` 已准备并通过 git apply --check，只恢复 A3
路径、源清单及打包规则，保留并行任务其他修改；在确认 Makefile 的
并行编辑状态之前没有应用。因此本批不宣称普通打包入口已经通过。

### 2026-09-16：图片查看器、Optional 迭代与文字测量 ABI

`asview.as` 已在原路径重写为静态 A3 原生应用：固定字段 View、
Optional[Image]、带类型的 Siblings 结果，以及明确的 main 和事件循环。
使用 std 前缀避免误导入旁边同名的 sys/strings 教学示例。目录读取失败
不再阻止窗口出现，图片加载仍给出原始路径和错误原因；无参数启动会
保留使用说明，直到用户关闭，而不是刚画完就随进程退出消失。

迁移发现并修复了两个实际问题：

- 检查器在求值 for 迭代源之前清空 Optional 证明，错误拒绝已经判空的
  目录列表。现在先检查一次性求值的迭代源，再清除循环体可能过期的
  事实；while 和循环体回边仍拒绝不安全读取。新增用例在循环中把源置
  None 并收集，确认迭代器持有的原列表仍存活。
- `logit_calls.abi` 保留着旧 `(px << 1) | mono`，实际内核与 C 调用方
  已使用 `(px << 2) | face`。此前宿主探针也复制了旧编码，因而未发现
  问题；真实画面出现半字号宽度测量、错误换行和右对齐。现已修正唯一
  描述源及生成的 abi.as/logit_pack.h，探针独立按内核协议解码，并验证
  mono/bold 两位。故意恢复旧编码的私有库确实触发断言失败。

`make test-asview` 和原有拆分目标接入同一个 A3 来宾门禁，像素负对照
是前置条件。正式打包验收也运行完整查看器场景。原独立 QMP 入口改为
启动打包的 AEX，权限场景通过真实 SYS_CAP_SPAWN 收窄原生子进程；
它使用的 native-capture 随普通磁盘构建打包，不再依赖 `as --scope` VM。

证据位于 `build/a3-cutover/`：

- `asview-native-guest-v4/result.json`：Optional 与实际查看器在 debug/
  release 均通过。每种模式启动 6 个查看器进程，验证 PNG/BMP 像素、
  1:1/fit、空格切换、前后切图、大图裁剪、缺失文件、非图片、真实权限
  拒绝、无参数窗口及 q/Esc/窗口关闭按钮；每个进程验证真实退出码。
- `asview-font-packing-verified.log`：GUI O0/O2 宿主检查及旧编码负对照
  通过；`asview-host-integration.log`：82 个 ABI 包装、21 个轮询场景、
  14 类 Optional 诊断和原生独立链接通过。
- `asview-base-regression.log`：245 项基础回归通过；
  `asview-studio-cli-build.log`：Studio 引擎 176 项及故障对照通过，
  实际 as.aex 重新构建。引擎门禁不代替完整 Studio 窗口验收。
- `asview-qmp-adapter.log`：原独立 shell/QMP 入口的 26 项断言全部通过，
  实际启动原生 AEX 并完成 PNG、BMP/WebP 切换和收窄权限的拒绝场景。
- `asview-packaged-recheck/result.json`：完整复验的 20 个打包示例通过，
  包括时钟、GUI、查看器全部交互场景与原生 shell。它与下方失败记录
  一并保留，不据一次复验宣称间歇性时钟停滞已经定位或修复。
- `a3-cutover-asview.json`：未完成项 67，剩余 A2 源文件 33；45 个
  原内建名称仍有原生生成探针。自举、剩余源码、工具及旧引擎删除仍未
  完成，不能据此宣布只剩 A3。

失败记录保留：第一次测试磁盘漏掉 .aex 后缀，被文件执行权限拒绝；第二、
三次像素判定误把抗锯齿文字中的画布灰色算入画布边界，已改为验证连续
边缘。第四次完整通过。整套打包第一次在时钟示例看到两个时钟均不推进，
`asview-packaged-guest/result.json` 正确为红；同一磁盘/产物单独重启的
`asview-packaged-clock.log` 测得 4000 ms / 4 RTC 秒。该失败不被查看器
验收覆盖，也没有删除时钟检查来换取通过。

### 2026-09-16：时钟示例及测试工具迁入 A3

`fsroot/as/examples/monotonic.as` 已原路径改写为静态 A3：执行逻辑放入
main，RTC 布局字段明确从 i32 转为 i64，读取失败抛出 IOError，避免
复用旧读数冒充测量。仍以独立 CMOS RTC 为基准，保留午夜跨日处理、
循环上限及 10 ms 步进检查；没有把测量替换成常量或宿主计时。

`make test-clock` 现在交叉编译并在无 VM 的私有来宾磁盘上运行；
`run-clock-test.sh` 的预构建磁盘入口也改为同一清单选出的原生路径。
两种入口、原生完整测试及磁盘打包验收共用 `as_clock_test.py` 的测量
判定，动态输出必须通过独立读数检查，不能因为没有固定期望文本而跳过。
打包仍从同一示例清单产生 `/usr/as/bin/monotonic.aex`。

证据位于 `build/a3-cutover/`：

- `clock-native-guest/result.json`：6 次 debug/release 执行通过。
  正常程序分别测得 3200/3960 ms，对照 RTC 均为 4 s，满足原有
  CMOS 量化与 TCG 容差。恒零对照被拒绝为 clock stopped；把毫秒除以
  10 的对照测得 380/390，被拒绝为 wrong clock rate。两个错误版本
  都实际编译并执行，没有修改内核时钟或生产示例。
- `clock-packaged-guest/result.json`：19 个实际打包的 A3 示例通过，
  包括当前时钟源码、GUI 示例和原生 shell，磁盘中没有 A2 VM。
- `clock-standalone.log`：预构建磁盘的独立 shell 入口通过，实际执行
  打包的原生时钟，测得 4010 ms / 4 RTC 秒。
- `clock-pack-wiring.log`、`clock-final-wiring.log`：打包路由负对照及
  Make 接线通过；时钟判定的 9 个拒绝用例作为来宾门禁前置运行。
- `a3-cutover-clock.json`：未完成项进一步降至 69，剩余 A2 源文件
  34 个，45 个内建名称的原生生成探针仍全部通过。审计仍为红；此批
  没有声称剩余标准库、自举、工具及旧引擎删除已经完成。

### 2026-09-16：作用域借用、可变视图与跨函数调用

对下方上一阶段“Slice/MutSlice、安全借用尚未接入”的修正：现在可以在
`with view = owner.borrow(start, stop)` 中取得 `Slice[u8]`，或通过
`borrow_mut` 取得 `MutSlice[u8]`。视图按真实字节布局生成原生代码，
不经过 VM Value；切片是数据指针和长度，作用域另外持有释放借用所需的
运行时记录。允许嵌套借用、读写、遍历、格式化和 `Bytes(view)` 独立复制。
正常退出、异常、return、break、continue 都逆序释放借用，再释放所有者。

`sema/region_borrow.c` 独立检查活跃借用，禁止冲突访问、借用中移动所有者
及暂停父视图的访问。作用域视图不能通过普通赋值、容器、捕获、返回或
Any 逃逸。直接、泛型及间接函数调用都接受声明好的 Slice/MutSlice 参数；
每个实参产生的临时借用持续到调用结束，因此同一可变视图不能重复传参，
后面的实参也不能偷偷读取已独占借出的视图。冲突诊断 AS3403 附带修复
提示、借用起点和同一源码快照的校验值。

最新证据位于 `build/a3-cutover/`：

- `region-borrow-integration.log`：45 类借用诊断、O0/O2 ASan/UBSan、
  实际分配/free 平衡、原生 CLI 和独立链接通过；原有 Region 所有权与
  运行时门禁也通过。停用借用检查的私有编译器错误接受了 26 个冲突程序，
  只检查这些非法程序，没有执行；漏释放负对照被独立计数拒绝。
- Studio 引擎 176 项通过，包含借用冲突定位及原生运行。这是引擎门禁，
  不代表完整窗口交互和宿主构建连接已经验收。
- `region-borrow-guest/result.json`：12 次真实 LogitOS debug/release
  执行通过，包含 Region、借用、预期越界异常以及 Port/Command 回归。
- `region-borrow-cli-verified/result.json`：5 项真实来宾检查通过，包括
  无 A2 缓存检查、AS3402、AS3403 与借用起点、缺少宿主连接的诊断以及
  原生 AEX 执行。
- `region-borrow-base.log`：245 项基础检查通过；
  `region-borrow-imports.log`：模块、覆盖快照和诊断回归通过。
- `a3-cutover-region-borrow.json`：45 个原内建名称都有成功的原生生成
  探针，未完成项从 72 降至 71，仍有 35 个 A2 源文件。名称探针不等于
  完整功能或工具接入验收，审计仍刻意为红。

仍缺少普通视图参数的再次借用入口、GC 容器借用、跨调用返回所有权的
生命周期协议、自举和完整工具适配。当前视图参数只有指针和长度，不能
凭空伪造运行时借用记录；这些缺口仍明确报错。旧引擎和缓存尚未删除。

### 2026-09-16：A3 源码 Region 所有者、移动数据流与原生清理

已把上一阶段的内部运行时接到 A3 源码。`with owner = region(size)` 获取
独立于 GC 的唯一字节区域；len、负下标、字节读写和复合赋值有原生生成。
`with moved = owner.move()` 消耗原所有者，目标按作用域释放。复制、装箱、
容器/字段保存、闭包捕获、跨函数传参/返回仍明确拒绝。

`sema/region.c` 负责调用和类型检查，`sema/region_flow.c` 独立检查所有权。
它在分支汇合处取仍存活所有者的交集，循环按回边计算固定点，异常边携带
抛出位置的状态。移动后读取/写入/len、循环第二轮重复移动、异常后使用
可能已消耗的源都报 AS3402，附源码版本与修复提示。此分析目前保守合并
异常类型，也不根据常量条件排除路径；这些精度限制已写入语言规范。

`backend/llvm/region.c` 生成独立所有者槽，与 `resource.c` 共用逆序清理。
移动必须消耗清理槽并清空局部槽；仅清空局部变量会让外层退出再次 free，
这一陷阱已在代码里说明。Region 字节地址通过专用检查取得，写入 i8，
不会按表达式的 i64 类型误写相邻内存。临时获取槽每次转移后清空，支持
循环反复执行同一源码位置。

证据位于 `build/a3-cutover/`：

- `region-source-integration.log`、`region-source-final-host.log`：28 类诊断、
  O0/O2 ASan/UBSan、实际分配/free 平衡、原生独立链接和正常 CLI 路由通过。
  Studio 引擎 172 项通过，含 Region 实际运行、AS3402 问题面板和源码定位；
  这是无窗口引擎门禁，不冒充完整 GUI 视觉验收。
- 关闭所有权数据流的私有编译器错误接受了 12 个移动后使用程序，证明
  拒绝依赖真实所有权检查；只执行检查，没有运行非法程序。省略真实 free
  的运行时被独立分配平衡断言拒绝。
- `region-source-guest/result.json`：14 次实际 LogitOS 运行通过，覆盖
  Region 正例/异常、Port、管道、Command 和手动分配的 debug/release 回归。
- `region-address-runtime-guest/result.json`：生产运行时 129 项、计数版本
  144 项在来宾通过，包含新地址接口的读写借用冲突；宿主四个负对照
  （漏 free、共享写入、地址访问、暂停父视图）均观察到失败。
- `region-native-cli-guest/result.json`：4 项通过，包括无 A2 缓存检查、
  来宾编译器 AS3402 定位、缺少宿主连接的明确诊断、相同源码的原生 AEX。
- `region-base-regression.log`：245 项基础宿主检查通过；
  `region-port-regression.log`、`region-command-regression.log`：原有所有者
  的 29/12 类诊断及真实 I/O、故障清理通过。

初次正例误把 List 身份相等当成内容相等，断言失败；已改为逐项验证事件
顺序，失败记录保留在 `region-source-positive.log`，不算验收成功。

这仍不是完整 Region 迁移：Slice/MutSlice、安全借用、跨调用所有权协议
和旧工具的完整语义服务仍待完成。`a3-cutover-region-source.json` 仍为
72 项未完成、35 个 A2 源文件。审计刻意没有用仅构造 Region 的探针把
整个旧 region 能力标成迁移完成；旧引擎和缓存因此尚未删除。

### 2026-09-16：Region 所有权运行时与资源检查拆分

新增 `runtime/region.c`、`region_borrow.c` 及对应头文件，实现真正独立于
GC 的区域所有者和借用记录。移动消耗源槽，失败不改变已存在的所有权；
共享借用阻止写入，独占借用阻止所有者访问，嵌套借用暂时停用独占父视图。
任何活跃借用都阻止所有者移动/释放，仍有子借用的父视图也不能提前释放。
区域与借用分配失败都保持原状态，清理空槽可重复执行而不重复 free。

Port 与 Process 共用的绑定、聚合和函数签名逃逸检查已从 `sema/port.c`
移到 `sema/resource.c`，接口更名为 `at_check_resource_binding`。编译器仍
通过 sources.mk 的阶段目录清单收集模块，没有复制新的源文件列表。

本阶段刻意保留的真实缺口：A3 源码 Region 类型、移动数据流、借用逃逸及
Slice/MutSlice 接入尚未完成。运行时借用计数不能替代这些静态证明，不能
据此开放无检查的 region() 或把它记为已迁移。A3-only 审计仍有 72 项，
region 仍是原内建清单的未完成项，剩余 A2 源码仍为 35 个。

证据位于 `build/a3-cutover/`：

- `region-runtime-host.log`：O0/O2 ASan/UBSan 通过；删除真实 free、放开
  共享写入、放开独占父视图访问的三个对照实际失败。Port 与 Command 的
  诊断、清理、真实 I/O 和故障路径在模块拆分后通过。
- `region-resource-integration.log`：原生独立链接、168 项 Studio 引擎
  检查、手动分配门禁和来宾 as.aex 重建通过。
- `region-resource-guest/result.json`：10 次相关 A3 来宾回归通过，涵盖
  Port、管道、Command、错误清理和手动分配的调试/发布产物。
- `region-runtime-guest-verified/result.json`：生产运行时 121 项、计数版本
  136 项检查在真实来宾中通过，记录了源码及两个产物的哈希。
  后续只补充借用记录分配成本的注释并整理格式；重新构建的两个 AEX 哈希
  均与该来宾报告一致。

运行时专用来宾门禁只证明 C 后端，不声称完成源码 Region。它链接生产
native.a 和只替换分配计数入口的版本，分别要求 121 与 136 项检查；计数
版本额外验证分配失败及真实释放平衡。首次构建因 OS 头文件搜索误选 native.h
失败，调整测试头文件优先级后修复；首次来宾装置遗漏 echo 与 /state 导致
捕获失败，补齐私有磁盘后重跑通过，失败日志保留为 region-runtime-guest.log、
region-runtime-guest-recheck.log，不计为验收成功。

### 2026-09-16：原生手动分配与存储子程序

`alloc(i64) -> Ptr[u8]` 和 `dealloc(Ptr[T]) -> None` 已原生实现。
`runtime/allocation.c` 独立维护手动分配，不进入 GC；零初始化、正数长度、
分配失败以及调用时 CAP_RAW 检查均有真实运行覆盖。释放只接受登记中的分配
起始地址，拒绝内部地址、GC 内存、空地址及已移除的地址。检查器要求 unsafe，
LLVM 调用将失败接入现有异常传播并保留调用源码位置。

这仍是显式手动管理：Ptr 是可复制的裸地址，释放不会使其他别名自动变零，
地址被分配器复用后也不能识别旧别名。调用者负责正常及异常出口释放，不能把
登记表当作 Region 生命周期检查。完整规则与旧 VM 的差别已写入语言规范。

原 `storchild.as` 在原路径迁移，使用定宽指针读取、显式 NUL 路径和所有出口
释放。仍未迁移的 storprobe 改为启动已打包的 storchild.aex；不再请求来宾
通过旧 VM 或尚未连接的宿主编译器执行 A3 源码。Make 现在打包 18 个原生示例。
Studio 引擎与 CLI 的真实运行用例也包含 alloc、指针读写和 dealloc。

本阶段证据均位于 `build/a3-cutover/`：

- `allocation-full-host.log`：完整宿主门禁通过，包含 245 项基础检查、
  118 条词法流、168 项 Studio 引擎检查和现有反向对照。
- `allocation-revocation.log`：8 类拒绝诊断、O0/O2 ASan/UBSan，以及
  分配失败、权限撤销、恢复后释放、异常/返回路径真实 free 计数均通过。
  故意跳过 free 和破坏零初始化的对照实际失败；不是用 GC 统计推测手动释放。
- `typed-guest-allocation-full/result.json`：完整 296 次原生来宾执行通过。
  新覆盖调试/发布下的分配、错误位置、六种实际内核授予，以及原 storchild
  对正常、短文件、内容变化及缺失文件的读取；私有磁盘不含 VM。
- `packaged-native-allocation/result.json`：生产 Make 映射的全部 18 个 AEX
  在独立无 VM 来宾中通过；`native-cli-guest-allocation/result.json` 的三项
  工具检查通过。来宾本地构建仍明确报告 AS3501，宿主连接尚未完成。
- `allocation-source-migration/result.json`：剩余 35 个 A2 源码仍通过两套
  旧编译器比对与四次宿主执行；危险/压力场景没有被作为普通示例盲目执行。
- `a3-cutover-allocation.json`：审计仍红，未完成要求从 76 降到 72；
  原内建清单只剩 region 没有原生实现。旧引擎、自举、其他源码和完整工具
  服务仍是阻塞项，尚不能删除 A2。

初始 `allocation-host.log` 和 `allocation-host-recheck.log` 未通过：测试先
误用了尚无 finally 的语法，再复用了类型不同的局部名。测试改为明确的
except 清理及独立变量名后才得到上述完整结果；没有以放宽静态规则换取通过。

### 2026-09-16：原始整数指针

四个旧指针构造器 i8ptr/i16ptr/i32ptr/i64ptr 已接入 A3，返回直接保存机器
地址的 Ptr[T]。检查器和 LLVM 下标运算分别拆到 sema/pointer.c、
backend/llvm/pointer.c，没有新增 VM 包装对象。读取和写入使用已知位宽，
支持非对齐存取、负索引、泛型 Integer 参数、容器、Any、Optional 和闭包。
能力检查在使用时重新执行；整数缩放与地址加减保留溢出诊断和源码位置。

迁移边界明确：索引读取现在产生对应的定宽整数，写入检查值范围；原始地址
不持有 GC 根。它是 unsafe 接口，不冒充尚未实现的安全 Region 借用。
原 `ptr.as` 在原路径迁移，用 Buffer 替代对不可变字符串的写入，保持
`p[0] + p[1] = 1337` 的输出。原生示例打包与旧测试入口从统一版本清单
自动选到它，不再将该示例交给 A2。

定向宿主证据 `build/a3-cutover/pointer-host-order.log` 包含 15 类拒绝诊断，
O0/O2 ASan 的实际位宽／非对齐读写、符号扩展、泛型／Any／Optional、
负索引、地址溢出和赋值顺序验证。移动一个有效的写入地址后，独立字节检查
实际失败；忽略权限撤销后，真实读写操作的拒绝计数也实际失败。
首轮 `pointer-host-split.log` 的顺序测试使用了不支持的 nonlocal 语法，
未计为通过；修正为语言已有的捕获绑定语义后才执行并验证。

后续回归证据（同在 `build/a3-cutover/`）：

- `pointer-full-host.log`：完整宿主回归、245 项基础检查、118 份词法流、
  168 项 Studio 引擎检查通过。Studio 和 CLI 测试实际检查并运行 Ptr[i32]
  读写；原 as/ash 与全部 17 个原生示例的来宾产物已重建。
- `pointer-host-location.log`：未捕获的零指针错误在 O0/O2 均报告
  原始源码第五行，没有执行实际内存读取。
- `typed-guest-pointer-isolated/result.json`：指针、顺序、错误位置、原 ptr
  示例、六种真实内核授权与 GUI，共 24 次来宾执行通过。
- `typed-guest-pointer-full-recheck/result.json`：完整 272 次原生来宾执行
  通过。首轮 `typed-guest-pointer-full/result.json` 在第 36 项之后等待 GUI
  窗口超时；内核及 GUI 产物哈希与隔离成功运行相同。隔离和整轮复测均成功，
  但首次偶发超时原因尚未定位，失败记录保留。
- `packaged-native-pointer/result.json`：生产打包映射中的 17 个实际 AEX
  全部通过无 VM 来宾验收，包括迁移后的 ptr、GUI 和原 shell。
- `native-cli-guest-pointer/result.json`：同一份 Ptr[i32] 源码通过来宾
  A3 check、缺失宿主连接的明确诊断及实际原生执行，未使用旧编译缓存。

`a3-cutover-pointer.json` 当前仍报告 76 项未完成要求；剩余 36 个 A2 源码。
alloc/dealloc、Region、旧自举及完整工具服务接入仍须继续，不能删除旧引擎。

### 2026-09-16：端口统计、原示例打包与迁移测试入口

`port_stats()` 已接入统一检查、LLVM 和原生运行时，返回独立的
`Dict[str, i64]` 快照。open、closed、close_errors 来自真实所有型端口计数；
借用描述符与重复 close 不重复计数。finalized、orphans 保留原来的 GC
最终化含义：A3 使用确定性作用域释放，没有这类最终化事件，因此为 0。
字典构造期间登记根，并使用普通字典同一套字符串哈希，支持后续插入和扩容。

原路径 `ports.as` 已迁移为 A3，保留文件逐行读取、命令管线、重定向、64 次
资源释放和借用标准输出的功能。旧输出 `ports drop` 改为 `ports scopes`，
明确说明现在测试的是作用域关闭。`fsroot/demo.as` 也已迁移，公开函数声明
类型、使用显式 main；平方和及位运算的实际输出保持原样。这是仓库示例源码
修改，没有重建或覆盖用户的持久化磁盘。

Make 现在从显式版本声明选取 16 个已迁移示例，原生编译到 `as-native/`，
随正常磁盘打包到 `/usr/as/bin/`。`/bin/ash` 复制同一个原生 shell 产物。
版本解析失败会让 Make 停止，不能静默丢掉部分程序。原 `run-as-test.sh`
按同一规则启动 A3 产物；不因产物缺失而回退到 VM。迁移来宾测试也改用
完整原生来宾清单，避免把已迁移的 sysdemo/ports 再交给旧字节码路径。

定向证据位于 `build/a3-cutover/`：

- `port-stats-host.log`：29 类编译诊断及 O0/O2 端口行为验证。强制 GC、
  扩容、异常关闭、关闭失败和统计快照均覆盖；移除字典根和关闭计数的对照
  实际失败，不能靠“函数存在”通过。
- `port-stats-demo-host.log`：13 个实际仓库源码的完整输出，O0/O2 ASan 通过。
- `typed-guest-port-stats/result.json`：新统计用例及原 ports 示例在调试／
  发布模式下共 4 次来宾执行通过。
- `typed-guest-port-stats-full/result.json`：完整原生来宾集 252 次执行通过，
  包含新端口统计、迁移后的 ports 和 Studio demo，以及已有原生功能回归。
- `packaged-native-port-stats-fixed/result.json`：Make 实际生成的 16 个
  示例产物全部通过，含窗口画面／按键及 shell 脚本。该验收盘不包含 VM。
  前一轮 `packaged-native-port-stats/result.json` 因私有盘缺少 `/docs`
  在 sysdemo 失败；补齐生产环境已有的目录后重跑，没有放宽程序断言。
  最终重建复验为 `packaged-native-port-stats-final/result.json`，同样 16 项
  全通过，并记录编译器、内核、源码及产物哈希。
- `original-example-gate-port-stats-fixed.log`：原混合迁移测试入口通过，
  已迁移示例直接运行 AEX，ptr/selfhost 仍是明确的 A2 阻塞项。
- `port-stats-source-migration/result.json` 和
  `port-stats-migration-guest/result.json`：剩余 37 个 A2 源码的双编译器
  一致性、4 次宿主和 6 次来宾执行通过。压力／崩溃样例只做编译检查。
- `port-stats-full-host.log`：完整原生宿主门禁、245 项基础检查、118 份
  原生词法流对照、168 项 Studio 引擎检查及前置失败对照通过。Studio
  实际检查端口统计并运行相同源码，核对管线输出和退出码 7；实际 as/ash
  来宾产物重建和 378 个 Make 片段接线检查也通过。
- `native-cli-guest-port-stats/result.json`：包含端口统计的相同源码，在
  来宾完成 A3 check、无宿主连接时的 AS3501 诊断及原生 AEX 运行验证。
  仍未交付宿主构建连接，不把这个明确拒绝当作构建功能完成。
- `a3-cutover-port-stats.json`：仍有 82 项未完成要求。旧引擎、兼容缓存、
  自举、Region／指针和完整 Studio 服务接入仍须继续，尚未达到 A3-only。

### 2026-09-16：原生命令、进程作用域和原 shell

`run(str, ...)` / `run(List[str])` 创建静态 `Command` 描述；`|>`、`<-`、
`->` 在执行前组合管线和两端重定向。`.wait()` 执行并返回最后一段退出码，
`.out()` 捕获完整 UTF-8 stdout；`.pid()`、`.status()`、`.argv()` 保留原接口。
赋值只保存描述，独立的管线/重定向语句会执行并等待。

`with process = command:` 或 `with process = command.start():` 取得唯一
`Process` 作用域所有者。所有出口等待子进程；不允许复制、返回、存入容器
或闭包捕获这个所有者。Command 的参数、管线链接和重定向路径参与精确 GC。
启动失败只终止并回收已经启动的本管线子进程，不留下等待不到的半条管线。

实际来宾测试纠正了一个宿主假设：LogitOS 的 libc `waitpid` 仍返回直接
退出码，不能使用宿主 POSIX 状态字再解码。适配留在原生命令运行时，未修改
其他内核/libc 调用者。宿主脚本测试还发现 print 缓冲导致父子输出乱序，现
在每个完整 print 后刷新一次 stdout；不会逐参数刷新。

原来的 `fsroot/as/examples/ash.as` 已在原路径迁移，使用类型化 CommandLine
结构、嵌套同类型参数列表、作用域 ports 和 std.sys。`/bin/ash` 由同一源码
提前编译并接入常规磁盘打包，原 `test-ash` 与 `test-shell-as` 入口相应改为
运行原生程序。没有引入另一个 shell 实现来代替原工具。

本阶段的定向证据位于 `build/a3-cutover/`：

- `command-lifecycle-host.log`：12 项编译诊断，O0/O2 真正子进程、200000
  字节捕获、退出码、作用域清理；部分 fork 失败、读取/分配失败、关闭 stdio、
  SIGPIPE 和继承高位 fd 的宿主验证。移除回收、输出接线、作用域等待、GC
  参数根的对照均已观察到失败。
- `typed-guest-command-status/result.json`：14 次调试/发布来宾命令和真实
  内核权限测试通过。此前 `typed-guest-command/result.json` 在退出码检查
  失败；修复并重跑后的报告才计为通过。
- `ash-transcript-host-fixed.log`：原 shell 的脚本/交互/EOF/缺失脚本，实参
  边界、错误恢复及文件内容通过。移除重定向后，即使正常输出退出提示，门禁
  仍拒绝该结果。
- `typed-guest-ash/result.json`：4 次原 shell 来宾执行通过，包括 cd/pwd。
- `typed-guest-command-full/result.json`：完整原生来宾集 **246 次**执行全部
  通过，包括命令错误路径、调试/发布模式及已有语言/标准库用例。
- `ash-original-guest.log`：原 `test-shell-as` 的 coreutils 流程在只携带原生
  ash 的私有盘通过；没有用 A2 编译器代跑。
- `command-full-host.log`：完整原生宿主门禁、245 项基础检查、118 份词法
  流比对、168 项 Studio 引擎检查及其失败对照全部通过；as/ash 的真实来宾
  产物重建成功，378 个 Make 片段接线检查通过。Studio 测试执行原生命令
  管线并检查 stdout 和退出码 7；这不等于完整来宾编辑器工作流已完成。
- `native-cli-guest-command/result.json`：同一管线源码在来宾通过 A3 检查、
  缺失宿主连接诊断及原生 AEX 执行三项验证。宿主构建桥接仍待实现。
- `a3-cutover-command-final.json`：仍有 **86 项**未完成要求；剩余 **39 个 A2
  源码**。这批迁移不代表旧引擎、自举、Region 和完整工具服务已可删除。

### 2026-09-16：原生匿名管道

旧 `pipe()` 返回两个动态 Port 的 List；A3 使用
`with reader, writer = pipe():`，在同一个作用域原子取得两个端点。这里保持
匿名管道、读写、EOF 和权限功能，通过明确所有权代替 GC 回收兜底。
名字不能相同，不能复制或逃逸。解析、名称绑定和 LLVM 清理路径共用同一
组端点标识；正常退出、return、break、continue、异常都逆序释放两端。

运行时在创建描述符之前完成两次包装器分配，失败不留下半个管道。写端关闭
后读端收到 EOF；读端关闭后写入通过 IOError 传播并清理，原生初始化忽略
SIGPIPE 以避免进程先被系统终止。底层 exec 保留系统的信号继承语义；后续
高级进程启动器必须为外部命令恢复默认处理。

本批增加端点方向、双端句柄复用、嵌套异常、关闭失败的独立顺序日志、
第二次分配失败以及 CAP_PROC 的正反测试。删除权限判断、遗漏第一次分配
的释放、移除生成的清理或恢复默认 SIGPIPE，测试均必须实际失败。
`pipe` 原生审计探针已接入；run、管线组合、重定向、port_stats、自举和
Studio 全面适配仍是未完成项，不把匿名管道通过当作完整 A3 切换。

验收记录（`build/a3-cutover/`）：

- `typed-guest-pipe-final/result.json`：完整 226 次原生来宾执行通过，包含
  两种构建的匿名管道与六类实际内核授权。该磁盘不包含编译器或 VM。
- `pipe-ports-final.log`：28 类诊断、O0/O2 实际文件／借用／管道操作，以及
  10 个观察到失败的负对照。关闭顺序、失败时的包装器和描述符数独立检查。
- `pipe-full-host.log`：完整宿主门禁通过，包含 245 项基础检查和 118 条
  原生词法工具流；`pipe-wiring.log` 验证 Make 门禁可达性。
- `pipe-studio-final.log`：168 项引擎检查通过。Studio 实际检查并运行
  A3 管道程序，将读端收到的 Bytes 写入借用 stdout，保留退出码 7。
- `native-cli-guest-pipe/result.json`：来宾 `as check` 检查相同双端语法；
  无宿主连接时明确 AS3501；同一源码的原生 AEX 实际输出正确。
- `a3-cutover-pipe.json`：剩余 89 个阻塞项、40 个 A2 源码；旧引擎和缓存
  仍是待删除项。这一批没有宣称原生进程组合或完整工具接入已经完成。

首轮 `typed-guest-pipe/result.json` 在 147 项后因 GUI 同步失败而未通过：
真实串口中 `gui ok` 被 WM 日志交织为 `dgamage fui okrom`。测试现用实际
画面判断可交互，再按 q 退出并读取已关闭的输出文件、核对完整输出与退出码。
中间尝试读取仍打开的文件无法看到已提交长度，也未计为通过。
`typed-guest-pipe-gui-closed/result.json` 四项复测通过；故意设置错误预期输出
的 `typed-guest-pipe-gui-control/result.json` 在真实画面和键盘步骤后被精确
输出断言拒绝。最终完整来宾结果使用修正后的读取方式。

### 2026-09-16：借用端口与现有启动入口

`port(fd)` 现已接入 A3 的静态检查、LLVM 后端及作用域清理。包装器关闭
不会关闭原描述符；正常退出、异常及显式重复 close 均经过验证。0/1/2
保留继承标准流权限，其他 fd 仍要求 CAP_RAW；`.kind()` 保留旧分类。
借用读取不再预读超出调用需求的字节，避免短期包装器丢弃原持有者的数据；
普通拥有型文件端口保留 4 KiB 缓冲。`port_stats`、原生管道和进程组合仍待迁移。

原来的 `as FILE ARGS` 现在识别显式 A3 源码并调用同一原生 run 驱动，
保持实参边界和程序退出码。Studio 的真实引擎测试发现 argv[0] 写成了
显示名 `as`，导致切换到工程目录后找不到相邻运行时；`runner.c` 已改为
保留真实编译器路径。测试使用 A3、借用 stdout 和退出码 7，观察实际输出。
这没有完成 Studio 的统一补全、多文件未保存快照和宿主构建连接。

来宾测试还发现原生命令的缺失工具链判断没有生效：AS_SELFHOST_COMPILER
只用于 main.c，commands.c 看不到它。现在按实际 freestanding 平台判断，
在创建构建目录前给出 AS3501；不再错误地尝试宿主构建，也不会回退 VM。

本批证据（均在 `build/a3-cutover/`）：

- `typed-guest-borrowed-final/result.json`：完整 224 次原生来宾执行。
  新增借用端口的真实读写、六类授权及 stdout 保留，调试／发布都通过。
- `borrowed-ports-full-host.log`：完整宿主回归，含 245 项基础检查；
  `borrowed-port-verified-build.log` 包含 21 类 Port 诊断和正反执行。
  移除借用 close 保护、允许过量预读时，原持有者后续读取／写入实际失败。
- `native-cli-studio-gate.log` 中旧启动方式已观察到失败；修复后的
  `borrowed-studio-integration.log` 通过 168 项真实引擎检查、6 个前置反例、
  Studio 原生链接、发布符号和 Make 接入检查。该证据不包含窗口 UI 操作。
- `native-cli-guest-fixed/result.json`：不携带 A2 编译器缓存的私有盘上，
  A3 check 通过，简写启动明确报告缺失宿主连接，同一源码的预编译 AEX
  实际输出 `native CLI guest`。这证明入口分流及能力边界，不是构建桥完成。
- `borrowed-source-migration/result.json`：剩余 40 个 A2 源码继续通过
  双编译器字节码比对及 4 次宿主执行；`a3-cutover-borrowed.json` 仍有
  90 项源码/API 清单要求未完成，标准库仍是 16/17。

### 2026-09-16：图片、窗口库和原生临时根

原路径 `image.as`、`gui.as` 及 `examples/guidemo.as` 已改为 A3。
图片库保留格式探测、RGBA 像素、缩放布局、权限拒绝、读取诊断和
5/10/20 MiB 重试；GUI 保留绘制包装、复用事件、文本测量和图片入口。
布局的定宽字段使用检查转换，像素容量在传原始指针之前验证。

真实来宾最初发现连续图片赋值使临时 GC 根保留到整个函数返回，导致旧图
缓冲区无法回收。新 `backend/llvm/roots.c` 在已完成语句后清理临时根；
局部所有者、尚未完成的实参和外层迭代源继续存活。独立门禁关闭清理后，
实际观察到保留字节数超过上界；恢复后同一用例在宿主及来宾通过。
这不表示完成了任意变量的活跃性或借用分析。

验证记录均在 `build/a3-cutover/`：

- `typed-guest-image-gui-final/result.json`：完整 **222 次**原生来宾执行，
  调试／发布各一轮；包含六种图像格式的独立像素比较、路径和权限、
  临时根、窗口像素、可见文字、真实 q 键退出及原弹球示例。
- `typed-guest-image-pressure/result.json`：后来补充的两次内存压力执行。
  调用者持有 5 MiB 图片时，20 MiB 重试无法放入默认 24 MiB 进程堆；
  保持 MemoryError 类型，并在错误中包含路径及分配预算。
- `image-roots-full-regression.log`：完整宿主原生门禁通过，含 245 项
  基础检查和 118 组原始词法工具输出比对。新增 GUI/image 门禁分别见
  `image-gui-final-build.log`、`gui-image-native-host-fixed.log`。
  字段错位、文本长度、源容量、像素顺序、路径副本及临时根清理的反例
  均实际触发失败；不是只判断编译成功。
- `image-gui-source-migration/result.json`：剩余 **40 个 A2 源码**仍通过
  双编译器字节码比对和 4 次宿主执行。`image/gui` 的冻结缓存匹配原始
  `bcstable.sha256`，仅供尚未迁移的消费者使用，最终仍须整体删除。
- `a3-cutover-image-gui.json`：标准库 **16/17** 原生检查通过，唯一剩余
  A2 标准库是 `asc.as`；现有源码/API 清单仍有 **91 项**未完成要求。
  这个清单不代替 Studio 补全、快照、构建运行与调试工具的完整验收。

两处测试修正保留原因：Pillow 默认 JPEG 平滑色度采样与内核不同，改用
现有 `jpeg_test.c` 规定的 `djpeg -nosmooth -dct int`，最大差 3、RGB 平均
差 0.5，并应用原 EXIF 朝向；没有扩大容差。最初 GUI 私有盘未携带字体，
色块正确而文字不可见。现在字体和授权文件来自生产 Make 清单，像素检查
要求文字墨迹；当时真实截图保存为 `gui-library/no-font.png`，前置反例
必须拒绝它。实际文本内容另由 ABI 参数测试逐字节验证，截图检查不是 OCR。

以上没有完成删除旧引擎、自举编译器和所有工具接入；A3-only 目标继续保持
未完成，旧缓存也不属于最终允许保留的配置。

### 2026-09-16：sys 服务库与原始系统示例

`sys.as` 已原地重写为 A3，标准库由上一节记录的 13 个增加到 14 个，
剩余 `image/gui/asc`。保留文件／目录、进程、时钟、DNS 和 ping 的服务
接口；写入文本显式转换为 Bytes，nil 失败值用 Optional 表达。路径与
argv 字符串不能包含 NUL，参数副本和原始指针数组由同一对象持有。
该结构在 fork 前建立，子进程的负值 exec 失败或语言异常都退出 127。
同时修正旧 run 在 fork 失败后可能调用 waitpid(-1) 等待其他子进程的问题。

原始 `fsroot/as/examples/sysdemo.as` 同步原地改为 A3，继续实际写文件、
列目录、删除文件、运行 echo、读取时钟及工作目录。完整输出包含子进程
文本与退出码，保存在来宾报告中，不只检查最后一行标记。

`test-as-sys-lib` 验证类型错误、参数错误、确定性时钟／网络结果、失败 fork
及真实 argv 指针内容，在 O0/O2 下运行；argv 测试强制每次分配进行 GC。
前置负对照分别破坏参数内容、删除失败 pid 保护、让 exec 异常重新传播，
均实际观察到失败（`sys-child-error-gate.log`）。宿主的私有 syscall 捕获器
不表示宿主能够执行 LogitOS 系统调用或已经验证真实网络连接。

最终 `typed-guest-sys-final-complete/result.json` 的 passed/complete 都为
true，196 次原生运行通过。新用例在真实来宾验证文件大小上限、目录、cwd、
时钟、子进程参数（空串／空格／中文切片）、退出码 9 和 exec 失败的 127。
此前两次测试失败分别是错误地把整文件上限当作前缀读取，以及示例新增的
字节数断言手算错误；修正测试／示例后完整重跑，失败记录不算通过。

A3-only 清单目前还有 97 项未完成要求，见 `a3-cutover-sys-accepted.json`。
现有源码中仍有 43 个 A2 文件；旧 sys 缓存与原字节码基线完全一致，仅供
未迁移消费者与行为比对使用，最终必须删除。这个阶段不代表 image/gui、
自举、全部工具入口或 Studio 已迁完。证据均在 `build/a3-cutover/`。

最终完整宿主回归 `sys-native-full-regression.log` 通过全部功能门禁、
245 项基础检查及 118 组词法输出对照。`sys-final-source-migration/result.json`
通过上述 43 个 A2 源码的双编译器比对与 4 次宿主执行；接线、生成 ABI
一致性与发布符号检查记录在 `sys-final-wiring.log`。

### 2026-09-16：原路径 ABI 标准库原生化

上表“12 个 A3 库、剩余 5 个”是上一阶段的状态；现为 **13 个 A3 库、
剩余 asc/sys/gui/image 四个**。`fsroot/as/lib/abi.as` 已由原生成器重写为
A3，保留 31 个布局以及全部公开调用声明。生成器将参数整理为文本、只读
字节存储、可写字节存储、名义记录和 argv；描述仍来自同一份
`include/abi/logit_calls.abi`，内核的 packed 宏与 C 偏移断言内容未改变。
渲染逻辑独立在 `tools/abi_native.py`，解析与参数验证分为具名函数。

`ByteStorage/MutableByteStorage` 泛型在实例化前检查读写承诺，原生后端
继续使用具体 Bytes/Buffer/记录布局。字符串参数显式复制尾部 NUL 并保留
GC 根；输出禁止不可变 Bytes；负长度、超长请求、不完整或未终止的 argv
提前报错。原始指针的所指对象仍由调用者保持存活，不能把缓冲区验证当成
自动所有权，也不能据此宣称线程／外部调用的生命周期全部完成。

`test-as-abi` 验证 82 个包装的实际 syscall 编号、参数顺序、64 位字段和
文本复制，以及 21 个确定性等待场景，均在 O0/O2 运行。四个前置负对照
破坏高位打包、字符串复制、长度检查和超时比较，均实际观察到失败。
新增元数据反例拒绝未知长度、重复参数和非法位范围；结构化符号测试核对
协议名称及当前源文件字节范围。最终接口记录为 `abi-interface-final.log`。

真实来宾的 `typed-guest-abi-native/result.json` 中 `passed/complete` 都为
true：190 次原生执行通过，包括新 ABI 的文件读写、时钟、等待和参数检查。
磁盘无 VM。完整宿主门禁 `abi-full-native-regression.log` 通过 245 项基础
检查、各功能正反例以及原始词法工具的 118 组输出对照；旧 VM 的独立行为
oracle 为 `abi-legacy-oracle.log`，354 项通过。

剩余 45 个 A2 源码仍通过双编译器字节码比对和 4 次宿主执行，见
`abi-final-migration/result.json`。旧 ABI 缓存只供尚未迁移的消费者和行为
比对使用；最终仍必须随整个 compat2 目录删除。A3-only 审计还有 101 项
未完成要求，见 `a3-cutover-abi-final.json`；这批结果不表示旧引擎、自举或
Studio 全部迁完。构建与接线证据分别为 `abi-native-build-final.log` 和
`abi-final-wiring.log`，路径均在 `build/a3-cutover/`。

### 2026-09-16：系统调用与内存文本桥接

`syscall`、`mem2str`、`mem2cstr` 现在有静态检查、LLVM 代码生成和运行时实现，
`addr` 扩展到 Bytes。系统调用仍要求 unsafe/CAP_RAW，参数布局固定为一个
i64 调用号与至多三个 i64/u64 字，保留原始内核返回值。宿主明确产生
RuntimeError，不能将宿主测试中的私有参数捕获器算作系统调用支持。

内存桥接复制文本、验证 UTF-8 并检查 Buffer/Bytes 容量。旧版非法 UTF-8
文本和无 NUL 静默截断行为不再保留，具体迁移边界见语言规范。
原路径 `fsroot/as/examples/sys.as` 已使用显式 main、Bytes 和 unsafe，
来宾全输出仍要求精确为 `hello via syscall\n`；宿主则验证明确拒绝执行。
其余依赖布局声明、所有权和进程接口的系统库仍未迁完。

真实来宾通过 180 次原生执行，报告为
`build/a3-cutover/typed-guest-system/result.json`，覆盖文件描述符读写、
负值内核返回、六种实际授权、文本转换与原 sys 示例。宿主门禁验证 13 类
诊断、O0/O2、强制 GC、存储复制和平台拒绝；移除权限/边界检查及交换 ABI
参数的反例均被观察到失败。
剩余 46 个 A2 源码仍通过双编译器字节码比对和 4 次宿主执行；A3-only
审计为 104 项未完成要求（`a3-cutover-system.json`）。这些是当前迁移范围，
不表示系统标准库、旧缓存、自举或 Studio 已全部迁完。
完整宿主 `test-as-typed` 已通过（含 245 项基础检查及各功能正反例），
记录为 `system-regression.log`；原词法工具的 118 组输出仍与 C 前端一致。
原生 `as.aex` 构建、发布符号门禁和 Makefile 接入检查通过，分别见
`system-native-build.log` 与 `system-wiring.log`。

### 2026-09-16：编译期 ABI 布局记录

旧布局依赖动态 VM 的 layout/Buffer。现在同样的固定布局声明可由 A3
前端直接检查和原生编译：字段偏移、宽度、签名和边界在编译期确定。
非对齐整数读写使用 alignment 1；记录是共享引用，普通 struct 仍为值。
固定字节字段读取生成独立 Bytes，保留 NUL；写入超长时保持原内容，
合法短写入将剩余字节填零。原始 p 字段为 u64，不隐式持有其指向对象。

实现拆为 `frontend/layout.c`、`backend/llvm/layout.c`，复用已有 Buffer
存储、GC 和字节操作。布局类型可出现在模块导入、泛型、容器、Optional、
Any 与闭包中，结构化诊断/符号报告包含类型声明的源码位置。
记录打印保留布局名及字段值，固定字节字段使用 Bytes 表示；独立元数据
区分内联字节与 Bytes 引用，避免调试输出将字节内容误读成指针。
规则中的迁移边界包括：布局元数据必须是字面量、整数字段使用明确位宽、
文本到固定字节字段需要显式 Bytes 转换；动态布局工厂尚不支持。

`test-as-layout` 已检查 23 类非法程序，并将从真实 ABI 头文件枚举的
31 个结构体与独立 C 编译器产生的字节序列逐字节比较，O0/O2 均通过。
负对照先观察到错误偏移、错误字节宽度元数据、遗漏填零和删除 GC 根的
指定失败，再运行正例。首次来宾记录为 `typed-guest-layout-records`；补齐
字段打印与模块根后完整重跑，最终报告是
`build/a3-cutover/typed-guest-layout-complete/result.json`，其中
`passed/complete` 均为 true，184 次执行全部通过，含实际内核写入时间
结构体后由 A3 读取，以及两个构建模式的非对齐/重叠字段与 GC 验证。

这只消除了原生固定布局能力这一项阻塞。`abi.as` 的系统包装、其余
标准库、原生自举和 Studio 完整接入仍待迁移；A3-only 审计为 103 项
未完成要求，报告是 `build/a3-cutover/a3-cutover-layout-final.json`。
原先 104 项记录对应上一阶段，不能据此认为旧引擎已经删除。
旧源码维护检查仍有 46 个 A2 文件，双编译器产物一致，4 次宿主执行通过，
见 `layout-final-migration/result.json`。原生编译器与运行时重建、发布
符号检查及 Makefile 接入检查分别记录于 `layout-final-native-build.log`
与 `layout-final-wiring.log`。
最终完整宿主回归通过，含 245 项基础检查、所有功能门禁及 118 组词法
流对照，记录为 `build/a3-cutover/layout-final-regression.log`。上述来宾
报告对应最终字段打印及根处理实现，不沿用补齐之前的产物作为最终证明。

### 2026-09-16：原生作用域文件端口

上表“文件、进程、ports”记录的是最初状态。现在 `open`、文件读写/逐行
迭代和 `with` 各出口清理已由 A3 检查器及 LLVM 后端实现。运行时文件对象
不进入 VM Value，也不依赖 GC 关闭描述符。所有者局部不可复制或逃逸；
当前获取语法固定为 `with file = open(path, mode):`。

实现按职责拆到 `sema/port.c`、`backend/llvm/port.c`、
`backend/llvm/resource.c` 和 `runtime/port.c`。清理代码在调用失败、显式
异常、return、break、continue 及正常退出时按逆序释放；作用域内的处理器
保留外部所有者。关闭失败不会覆盖原始异常，也不会重试已消费的描述符。

原路径词法工具 `tests/unit/aslexdump.as` 使用这一所有权路径读取源码，
读取后关闭文件，词法处理只保留独立的 Bytes/str。它继续与 C 前端比较
完整词法流。`test-as-ports` 加入主门禁，覆盖 17 类静态诊断、O0/O2 的
真实文件运行及关闭顺序/失败注入；删除释放、吞掉关闭错误和截断写入的
私有反例必须先被观察到失败。

这不是完整 ports 迁移：`port(fd)` 借用、所有权移动/跨函数传递、进程管道、
重定向、Region 和资源标准库仍待实现。原 `ports.as` 包含这些能力，因此
保留其迁移阻塞记录，不能删掉未实现部分后宣称示例已经迁完。

来宾验收已通过 162 次原生执行，报告为
`build/a3-cutover/typed-guest-ports-grants/result.json`，包含六种真实内核
授权的调试/发布端口操作。词法工具的 118 组原生输出仍与 C 前端一致。
`port-guest-final.log` 记录的首次新增授权测试因测试变量类型冲突未能构建，
不计入通过结果；修正测试后从构建到来宾执行完整重跑得到上述报告。
A3-only 审计现为 109 项未完成要求（`a3-cutover-ports.json`），不是迁移
完成声明，也不意味着所有代码工具已经接入 A3。
完整宿主 `test-as-typed` 回归通过，含 245 项基础检查及各功能门禁；记录
为 `port-regression.log`。`as.aex` 重建、发布符号检查和 Makefile 接入检查
也通过。上述结果限定于这里实现的作用域文件所有权。

### 原生异常

```python
# aether: 3.0
def quotient(left: i64, right: i64) -> i64:
    return left / right

def main() -> None:
    try:
        quotient(1, 0)
    except ZeroDivisionError as error:
        print(error.message, error.file, error.line, error.column)
    print("继续执行")
```

- 内置种类包含 OverflowError、ZeroDivisionError、IndexError、ConversionError、ValueError、AssertionError、RuntimeError、IOError、MemoryError、TypeError、KeyError。`Error` 和无类型 `except:` 接收所有种类。
- `raise ValueError("说明")`、`raise error` 和处理器内的裸 `raise` 均有原生实现；正常调用跨模块/泛型也能传播。
- 异常值是不可变的固定布局记录：`code/message/file/line/column`。当前异常名称在值注解中均对应 `Error` 记录；处理器按 `code` 区分种类，尚没有自定义异常类或静态子类型层级。
- 创建异常值时位置为空，第一次抛出记录位置；捕获或重新抛出保留它。嵌套处理器各自保存副本，内层异常不会覆盖外层待重抛的值。
- 处理器清除待处理状态后才运行，恢复后的普通调用不会继续误判失败。处理器/else 内的新错误只交给外层处理器。
- 变量初始化分析从 try 之前的状态进入处理器；不会假定故障前已执行所有赋值。未匹配异常走显式传播路径并使进程返回非零。

这套实现没有使用 VM，也不依赖系统栈展开器。旧说明曾写“字符串存储来自不可变原生常量”；现在已有动态字符串，异常记录的文本字段、待处理异常、捕获记录和模块保存的异常均纳入精确 GC 扫描，不能沿用“常量永远存活”的假设。

### 文本和表达式

`yes if condition else no` 只执行选中分支，分支结果必须具有一致静态类型。字符串支持六种比较、`needle in text` 和 `text.find(needle)`；查找沿用 A2 的 UTF-8 **字节偏移**，空串返回 0，找不到返回 -1。比较/查找处理显式长度和内嵌 NUL，字符串 `\0` 转义也已修正。

`f"文字 {表达式}"`／`F'...'` 已支持原生插值，复用普通表达式的类型检查和 str 值格式化。支持转义、双花括号、嵌套插值、多行表达式、容器和泛型值；按从左到右顺序计算，某个插值抛异常后不继续计算后续部分。格式规格仍与原版本一样明确报错。插值 token 在临时词法缓冲释放前重定位到原始源码，中文编译诊断和运行异常保持原文件位置。

`Range` 可保存、传参、返回、装箱和放入容器，仍是惰性不可变对象；相等比较保留引用身份。支持 len、负索引、成员判断和原来的列表式格式化。直接 `for ... in range(...)` 保留不分配对象的数值循环；先保存的范围也只读取边界一次。计数使用无符号跨度除法，修正旧算法在接近整个 i64 域时先相加溢出的问题；真实长度超过 i64 上界时 len 抛 OverflowError，范围本身仍可索引和迭代。字符串 for 循环与原有索引一致，每步是一个 UTF-8 字节视图，视图保留原始文本所有者。

列表推导式 `[表达式 for 名称 in 可迭代值 if 条件]` 已使用共享原生循环生成 List[T]。可迭代值先在外部作用域解析，循环名称仅对过滤条件和元素表达式可见；允许嵌套推导式和遮蔽同名局部、模块、函数。过滤条件为 bool，满足条件才计算元素；Optional 判空可作用于当前元素，不能泄漏到推导式外。支持 Range、字符串、Array、Slice、List 和 Dict，结果中的借用仍受堆容器规则限制。

### 随机数与近似数学

`random` 保持原来的 31 位 LCG、种子归一化、包含上界的 randint，以及不修改输入的 shuffle/sample。它不是密码学随机数，也没有改变原来的取模分布。泛型列表操作按元素类型实例化；非法区间或空选择产生 ValueError，超出 i64 的区间运算产生 OverflowError。

`mathx` 的 square/cube/quad/clamp/dist2 保持数值类型；其余实数函数边界明确使用 f64，整数调用者需要显式转换。保留原有 Newton/Taylor/atanh 近似算法及次数，不声称是完整精确舍入 libm。`numeric-lib` 工程运行每个公开函数、全部常量、种子序列和异常分支。

`math` 的常用算术、容器归约按 Number 类型实例化；整数序列/素数接口使用 i64。`powi/pow` 的原有返回类型随指数取值变化，因此明确返回 Any：正指数保留原始数值类型，负指数为 f64，零指数为 i64 的 1。调用者通过 `is_type[T]` 或 `cast[T]` 处理结果；不会为了固定返回 f64 而丢失大整数精度。`**` 仍是已知整数类型的直接原生运算。

### 标量转换

`str` 支持数值、bool 和 str；浮点格式保留 17 位有效数字。`parse_int` 严格检查完整十进制/十六进制输入及 i64 边界；`parse_float` 同样要求消费整个字符串，非法输入产生 ConversionError。旧浮点解析曾接受有效前缀或把非法输入变成零，这一行为在 A3 中明确改为报错。`chr`/`ord` 保留字节语义（0…255/首个 UTF-8 字节），非法 chr 和空串 ord 抛 ValueError。`f64bits` 提供浮点位模式。

后续补齐：`str` 和 `print` 也接受 Any、List、Dict、Array、Slice、结构体、Optional 和 None。编译器生成目标布局描述，字段偏移由 LLVM 计算；格式化不读取填充字节。容器内的文本带引号，内嵌 NUL 保留长度，长结果按需增长；循环引用在深度上限显示省略标记。print 的全部实参先按从左到右顺序求值，再输出，保持原调用副作用顺序。

### 原生字典和集合

`Dict[K, V]` 分别存储键和值的具体原生布局。当前 Hashable 覆盖定宽整数、bool 和 str；浮点/Any 键尚未完成。支持字面量、索引、插入、原地更新、len、in、has、remove、keys、values、get(key, default)。索引缺失键抛 KeyError。keys/values 和字典循环产生独立快照；遍历时增删字典不使游标失效。顺序不作保证。单参数 get 的空值结果仍依赖 Optional，不能把当前子集写成完整字典兼容。

上述单参数 get 缺项已补齐：`get(key)` 返回 `Optional[V]`，缺键为空，有键时保留 V 的实际值。V 本身为 Optional 时，外层是否有键与内层是否为空分别保留，不能把“存在且为空”当成缺键。

### None 与可空值

`None` 是可存储的空值，`nil` 保留同义拼写；单独值类型名为 `NoneType`。函数返回注解 `-> None` 继续表示无返回结果，`return None` 可用于这种函数。`Optional[T]` 使用存在标记和内联 T 布局；T 或 None 在明确上下文中注入，普通赋值不会把任意类型自动变成 Optional。

局部变量经过 `!= None`、`is not None`、反向比较、assert 或相应短路条件后，可以作为 T 使用。分支合流只保留所有可达路径都成立的事实，重新赋值清除旧事实；循环和异常处理保守清除可能过期的结论。字段和模块变量先复制到局部再判空，避免别名修改。GC 只扫描存在的载荷；可空包装不能使借用逃逸。

Optional 在 T 满足 Equatable 时支持相等比较；空值不读取载荷。序列的 `in`／`not in` 使用相同规则。`is`／`is not` 当前限于 None 判断，其他身份判断尚不支持。

`sets` 的 16 个公开函数已在原路径重写为 Hashable 泛型函数。空集由调用处结果注解确定键类型，例如 `names: Dict[str, bool] = sets.empty()`；参数已推断出的类型不会被结果上下文改写。集合并交差集返回新字典，add/remove 保留原地修改语义。

### 原生类与构造器

`class` 字段必须声明，值是对独立原生载荷的引用；别名共享字段，组合字段保持各自原生布局。`init` 必须返回 None，并在每条正常退出路径初始化全部字段；不定义 init 时使用全部字段参数构造。构造完成前禁止读取未初始化字段、传出 self 或绑定其方法。异常出口不要求补齐未完成对象。

方法的 self 类型由所属类确定，其余公开参数和返回值显式声明。绑定方法使用代码地址和 receiver 环境，GC 扫描器保留对象；对象扫描器继续保留文本、容器和其他类字段，循环引用可回收。模块导入类、限定名构造、私有方法检查、方法异常传播已有宿主与来宾验证。继承、泛型方法、构造器作为函数值以及限定名类型注解仍未完成。

后续修正：单继承与方法重写现已实现原生路径。子类继承字段前缀，不能重新声明同名字段；重写方法的参数、返回类型必须与父方法一致，构造器可有不同参数。子类引用可隐式转为父类引用，分配仍使用具体子类的大小和扫描器；可变容器不因此协变，`List[Child]` 不能当作 `List[Base]`。

普通方法通过对象的方法表选择重写实现；父方法中的 `self.method()` 仍选择子类实现。`super.method` 选择词法父类的方法，可保存为绑定方法，也可在方法的闭包中使用。`super.init(...)` 只能在构造器中直接调用，不能保存为回调；成功调用后才建立父字段初始化事实。继承构造器的类如果增加字段，必须定义自己的构造器。

构造器仍在编译期检查其声明的全部字段。父构造器不能静态知道每个未来子类的状态，因此对象还有完整字段的初始化位图；父构造器在传出 self、建立绑定方法或捕获 self 之前检查位图。未完成的子类对象产生有源码位置的 RuntimeError，不能把零填充存储冒充已初始化值。C 与 LLVM 的对象头来自同一字段表。这里的头部是运行时实现细节，不是 C 布局结构体接口。

原 `classes.as` 示例已经原地改写为 A3，完整 stdout 与改写前的执行结果一致。`test-as-inheritance` 验证 14 类诊断及 O0/O2 的跨模块、三级继承、父类引用、Optional、绑定方法、闭包中的 super 和 GC。强制每次分配回收的负对照分别取消构造检查、把虚调用改成静态调用、去掉字段扫描，必须实际观察到断言失败或 ASan use-after-free。完整来宾验收另以运行报告为准。

边界复查将诊断用例增至 19 类：super 必须绑定词法方法的真实 self，不能把嵌套参数或推导式的同名变量当作接收者；闭包、解包和循环也不能重绑定方法 self。`super.init` 只建立实际被调用构造器所声明字段的事实，不能把更高层继承来的构造器当作初始化了新增字段。原检查器接受同名整数参数的失败记录保存在 `inheritance-boundary-before.log`；修正后的全部诊断与正例通过 O0/O2 宿主运行。

继承阶段最终完整回归通过 84 次真实来宾执行（`build/a3-cutover/typed-guest-inheritance-final/result.json`），以及全部宿主原生门禁和失败对照。剩余 51 个 A2 源码通过字节码比对与 8 次宿主运行；旧自举示例门禁只计 2 个旧路径程序，另外 7 个明确转交原生完整输出门禁。A3-only 审计仍有 131 项未满足：12/17 个库和 8/42 个示例已通过相应原生检查/生成门禁，不能把继承阶段通过说成完成全部迁移。

### 字节缓冲区

`buffer(n)` 原生返回 `Buffer`，也可写 `Buffer(n)`。长度范围沿用 0 到 64 MiB，实际分配失败产生 MemoryError；存储固定大小并初始化为零，不用 Value 容器存字节。索引和遍历返回 i64，负索引从末尾计算；写入值必须在 0 到 255 范围内。越界产生 IndexError，非法字节值产生 ValueError，失败的复合赋值不修改原字节。

Buffer 支持 len、身份相等、成员查询、for、推导式、严格长度解包、类字段与显式 Any；格式化保持 `<buffer N>`。原生 GC 保留返回值、中间实参和聚合内的缓冲区引用。`test-as-buffer` 的宿主正例和负对照已经执行：故意使用 i64 写一个字节必须发生 ASan 越界，删除缓冲区根必须发生 use-after-free，取消字节范围检查必须导致断言失败。只读借用视图、唯一 Region、Bytes 和原始指针互操作还需要继续迁移，不能用 Buffer 名称代替这些语义。

缓冲区阶段通过 86 次真实来宾执行（`build/a3-cutover/typed-guest-buffer/result.json`）、全部宿主语言门禁及剩余 51 个 A2 源码比对。该次完整命令最终因并行新增的 `tests/gpu/amd/polaris/native/native.mk` 未接入而在 `test-mk-wired` 失败，不能称为整条命令通过。

### 系统常量

全部 107 个既有系统常量已接入原生检查和 LLVM 生成。`system_constants.def` 统一名称与 C 符号映射，值取自 LogitOS ABI 和独立的语言能力位定义；旧注册代码和补全暂时共同消费这份表，删除旧引擎时不再需要复制常量。`SYS_*` 始终表示 LogitOS ABI，不使用宿主的系统调用编号；读取这些数字不授予系统权限。

常量表达式的类型是 i64；目标为 u8/u64 等类型时需要显式转换。用户声明、模块变量、参数和捕获变量优先，函数内同名赋值仍按整个函数决定局部绑定，不能在初始化前回退读取内建常量。宿主 O0/O2 和完全不链接旧 VM 的前端已逐个执行并与独立编译的 ABI 值比对；故意改错生成的整数、保持退出码为零时，完整输出比较必须失败。来宾验证仍以对应运行报告为准。

系统常量阶段完整回归通过 88 次真实来宾执行（`build/a3-cutover/typed-guest-constants/result.json`）。107 个常量的完整输出与独立 ABI 程序逐一一致；62 项补全测试、全部宿主语言门禁、剩余 51 个旧源码比对、自举迁移门禁和 Makefile 接入检查也通过。此时 A3-only 审计仍有 129 项未满足。

### 能力快照

`caps() -> Cap` 在原生运行时返回当前进程权限的不可变快照；`bits() -> i64`、`path() -> Optional[str]`、`without(mask) -> Cap`、`scope(path) -> Cap` 保留原接口。无路径限制时 path 返回 None，显式根路径仍返回 `/`。快照按身份比较，可放入容器、类、闭包或显式 Any；不能用普通整数构造 Cap。创建或收窄快照不会替换进程实际持有的权限。

scope 要求绝对路径，并先归一化重复分隔符、`.` 和 `..`，再验证路径分量边界；含 NUL、相对路径和过长路径产生 ValueError，扩大范围产生带源码位置的 PermissionError。旧实现曾直接比较未归一化文本，这里明确收紧其边界。路径结果保持所有者存活；文件系统的实际解析和权限检查仍属于后续资源获取接口，快照 API 通过不等于文件访问控制已经全部迁移。

宿主门禁验证 7 类诊断、O0/O2 执行、GC、收窄及失败后的继续运行；去掉路径范围检查必须断言失败，丢失启动路径却保留权限必须使 C 运行时测试失败，取消 Cap 扫描必须发生 ASan use-after-free。来宾门禁增加内核直接创建的无权限、文件、网络、raw、进程和 GUI 六种子进程，分别验证两套位定义的转换和路径传递；是否通过以实际运行报告为准。

能力快照阶段的完整回归通过 102 次真实来宾执行（`build/a3-cutover/typed-guest-capability/result.json`），其中六种内核授权分别通过调试/发布运行。全部宿主原生门禁、51 个剩余旧源码比对、旧自举迁移门禁和 Makefile 接入检查均通过。此时文件、进程和 raw 资源获取尚未接入，不能把权限快照验收扩大为这些访问接口已完成。

### unsafe 与标量内存访问

`addr(Buffer或str) -> usize`、`peek8/16/32/64(usize) -> i64` 和 `poke8/16/32/64(usize, i64)` 已接入原生生成。调用必须位于当前函数的 `unsafe:` 块中；嵌套函数和 lambda 不继承外层许可。usize 是目标指针宽度的无符号地址，旧 i64 地址变量需显式转换。每次操作仍检查当前 CAP_RAW，失败产生 PermissionError；旧地址不会保存过去的权限。

读写按目标的本机字节序进行，允许未对齐地址。peek8/16/32 返回零扩展的 i64，peek64 保留 64 位整数位模式；poke 写入参数的低位，延续旧 raw API 的截断行为。普通整数运算仍检查溢出。空指针访问产生 ValueError，其他地址及所有者生命周期由 unsafe 代码负责；整数地址不构成 GC 根。str 的地址也不保证当前视图末尾恰好有 NUL，不能直接冒充 C 字符串。安全 Buffer 索引继续独立检查边界和值范围。

宿主已验证 9 类诊断及 O0/O2 原生执行；故意把 32 位读取改为符号扩展会断言失败，撤销权限后忽略新权限状态也会断言失败，控制程序始终只使用自身仍存活的缓冲区。六种来宾授权用例继续加入实际 raw 读写和拒绝检查，结果以运行报告为准。typed pointer、alloc/dealloc、Region 和 C ABI 尚需继续实现。

### 函数值、序列和统计库

`Callable[[参数类型...], 返回类型]` 描述函数值的完整签名。函数可以传入、返回、存入模块变量、结构体、List、Dict 或显式 Any；间接调用也会检查参数、返回类型和异常状态。泛型函数值由注解或其他实参推断。普通函数使用原生代码地址和空环境；捕获变量的闭包及 lambda 尚未完成。

`Equatable` 约束相等比较，`Ordered` 约束排序比较；Number/Integer 可用于二者，Hashable 保证相等比较。List/Dict 保留引用身份相等语义；Callable 比较代码和环境。Array/Slice/List 的 in 比较各元素，已知元素类型直接生成原生比较。

`seq` 的复制、筛选、归约、短路查询、排序、切片、分块和回调接口均在原位置重写；排序保留稳定插入排序及不改变输入的行为。`zip`/`enumerate` 保留原 API 的两元素列表形状，异类型单元格改为显式 Any。`dicts.items`/`from_pairs` 使用同一形状；from_pairs 的结果注解确定键值类型，每个单元格转换检查类型，错误保留库源码位置。`group_by` 已原生调用回调并建立同类型分组。

`stats.median` 的奇数样本结果保留元素原始类型，偶数样本结果为 f64，因此返回显式 Any；奇数个 i64 样本不会被强制转浮点而丢失精度。方差、均值、移动平均先分别转换整数再做实数累加。统计库现在复用原生 seq 排序。

`test` 的全部公开函数已原地重写，保留记录失败、返回 bool、继续执行和累计报告的行为。模块计数显式使用 global；`assert_eq` 按 Equatable 实例化，`assert_same` 比较列表内容。库函数 `assert(condition, message)` 与语言 assert 语句分别解析。

### 显式 Any 边界

`Any(value)` 复制具体原生值到带类型身份和 GC 扫描器的盒中；List 等引用值保持引用语义。普通数值和结构体不会因此自动装箱。`cast[T](box)` 检查实际类型完全匹配，不匹配时在调用位置抛 TypeError；`is_type[T](box)` 返回 bool。需要数值转换时先取出实际类型，再做显式数值转换，例如 `f64(cast[i64](box))`。

装箱不能延长借用寿命：直接或嵌套的调用作用域 Slice 都被拒绝。Any 的相等比较要求相同具体类型，整数位宽、有无符号和整数/浮点之间不隐式混合。浮点按数值比较（正负零相等，NaN 不等于自身），结构体逐字段、数组逐元素比较，List/Dict 按引用身份比较。深层值记录使用显式工作栈，不消耗递归 C 栈。动态算术、排序、方法调用和反射仍未完成，不能把“已装箱”写成完整动态语言能力。

## 验证

```sh
make BUILD=build/a3-cutover test-as-native-parity
make BUILD=build/a3-cutover test-as-typed-guest
```

`test-as-native-parity` 已接入常规原生门禁，前置负对照撤销异常状态清除，必须观察到 `exception-recovery` 失败。正例覆盖跨模块/泛型、处理器次序、重抛、分支/循环出口、不可变异常、未初始化变量、字符串及条件表达式；O0/O2 均执行。

来宾门禁新增 `exceptions`、`text`、`uncaught` 三个工程。测试磁盘没有 AetherScript VM；保留产物/编译器哈希、源码快照、实际退出码和输出。`IOError` 的主动抛出通过，并不代表真实文件 I/O API 已经移植。

`test-as-managed` 验证原生 List、动态字符串、跨调用根及回收，包含删除临时根后实际触发 ASan use-after-free 的负对照。`test-as-globals` 验证菱形导入、模块根、显式全局绑定和初始化失败，包含删除模块根、把未初始化标记改为 true 的实际失败对照。来宾门禁执行同一组真实源码。

`test-as-dict` 验证字典及 sets 的宿主 O0/O2 行为，含强制哈希碰撞、删除后的探测链、扩容、快照、泛型和 GC 回收。前置负对照删除字典值扫描器，必须实际触发 ASan use-after-free。来宾门禁另执行 dict/sets 工程；宿主内存检查不代替来宾执行证据。

`test-as-callable` 的前置负对照关闭间接调用的异常检查，必须观察到原本禁止的后续副作用。`test-as-stats` 把中位数相加改回先做整数运算，必须观察到合法极值输入上的 OverflowError。`test-as-collections` 验证 seq/dicts 的完整公开函数集，并用反转排序比较器的实际失败验证断言有效。对应源码分别加入来宾门禁。

最终门禁 `test-as-a3-only` 对照固定的 17 个库、345 个公开函数、280 个模块值和 42 个示例，并要求原生/来宾门禁在前、旧引擎及缓存全部消失。它在迁移未完成时应当失败。原先“也不能移除 A2”的阶段约束现已由完整 A3 切换目标取代；删除旧实现必须伴随功能迁移，不能通过丢弃接口使清单变绿。

`test-as-format` 实际变异字段偏移和测试库失败计数，必须观察断言失败；`test-as-any` 除缺失扫描器外，还把浮点比较改成位比较以验证正负零/NaN。`test-as-optional` 检查分支、重新赋值、循环和异常路径的非法使用；删除 Optional 载荷扫描器后，ASan 必须观察到真实 use-after-free。

`test-as-class` 验证 18 种非法程序、构造器分支、对象别名、组合、方法值和循环 GC；删除对象字段扫描或绑定方法环境根，都必须观察到真实 use-after-free。类实现加入后，完整回归通过 50 次来宾执行（25 个工程各运行调试/发布版本）。

测试范围修正：过去仅传 `-fsanitize=address` 不会自动给手写 LLVM 函数添加检查属性，因此旧宿主测试主要检查 C 运行时。当前测试先给私有 IR 副本的所有原生函数添加 `sanitize_address`，再执行 ASan 编译。修正后完整宿主/类回归已重跑通过。UBSan 检查 C 运行时；语言整数溢出继续由编译器显式检查，不能把任意 LLVM 都称作有 UBSan 覆盖。

`aslex` 已在原路径用 A3 固定字段类和同类型状态重写；公开 token 继续是 `[种类, 文本, 行, 起始字节, 结束字节]`，混合字段显式使用 Any。词法错误改为 ValueError，保留 error_span。`test-as-lexer-lib` 用独立链接的 C 前端逐字节比较全部 59 个标准库/示例源码的 token；同时验证中文偏移、缩进、异常、绑定方法和 GC。错误地把字符串发成标识符的负对照必须实际失败。完整回归通过 52 次来宾执行，旧编译器的 41171 字节固定点仍一致；这不代表原生编译器已经自举。

`test-as-fstring` 覆盖非法表达式、原始位置、嵌套/多行插值和泛型，O0/O2 通过；删除中间表达式根后 ASan 必须观察到 use-after-free。解析器已拆成声明/语句、公共操作、表达式和字符串四个翻译单元。

格式化字符串与解析器拆分后的完整回归通过 54 次来宾执行。Range/文本遍历的宿主 O0/O2 用例覆盖极值、长度溢出、对象身份、容器保存和 GC；对应来宾用例已接入门禁，验收结果须以实际运行报告为准。

Range/文本遍历的后续完整回归通过 56 次来宾执行；把计数公式改回先相加的负对照实际触发断言失败。`test-as-comprehension` 的宿主 O0/O2 覆盖作用域、过滤顺序、Optional、泛型、嵌套结果及异常中止；删除结果构造根后 ASan 观察到 use-after-free。推导式来宾用例已接入门禁，结果以运行报告为准。

推导式的后续回归通过 58 次真实来宾执行，报告位于 `build/a3-cutover/typed-guest-comprehension/result.json`。该次仓库级 `test-mk-wired` 因并行新增的 Polaris 固件测试未接入而失败，不能把整条命令称为通过；语言测试和来宾运行已经完成。

### 多变量赋值与解包

`a, b = b, a` 先从左到右计算全部右侧，再从左到右写入目标。右侧计算抛异常时，不写入任何目标。单个 List、Array、Slice、Range 或 str 可以解包；长度必须等于目标数量，Array 的静态长度在检查期验证，动态长度不匹配抛 ValueError。str 延续字节视图语义。目标当前限于变量名，右侧不能隐式成为异构元组值。

旧实现曾忽略单序列解包的多余元素，并反向写入逗号右侧的目标；A3 明确修正这两个行为。重复目标 `x, x = 3, 4` 的最终值是 4。解包不会让未初始化变量在右侧提前可用，也不会使已有变量的类型发生改变。

`test-as-assignment` 已通过 9 类诊断及 O0/O2 原生执行；前置负对照去除先求值结果的根，实际触发 ASan use-after-free。解析、类型检查和 LLVM 解包分别位于独立文件，避免继续扩展语句主分发器。

复合赋值的后续修正：过去原生实现先计算右侧，导致 `total += change_total()` 在右侧重绑定 total 时读取错误旧值。现在先捕获目标所有者、下标和旧值，再计算右侧，最后重新定位存储写回。Class、List、Dict、Buffer 的返回值可作为目标所有者；临时值结构体、临时固定数组以及只读 Slice 继续拒绝。嵌套内联字段/数组保留父存储路径，因此右侧或外层下标函数扩容容器后也不会继续使用原元素地址。

该行为在 `backend/llvm/compound.c` 独立实现，共享 Buffer 的字节检查。`test-as-assignment` 扩展为 17 类诊断及 O0/O2 执行，包含字符串旧值 GC 根、所有五种复合运算、负下标、字典删键、失败顺序及普通赋值的嵌套目标。把右侧提前、继续写旧地址、去除旧字符串根的三种编译器变异均已观察到相应失败。`compound` 工程已接入调试/发布来宾门禁，最终验收以实际运行报告为准。

本阶段完整回归通过 106 次真实来宾运行，报告为 `build/a3-cutover/typed-guest-compound/result.json`；宿主包括 245 项基础类型/原生检查及各功能正反向门禁。剩余 51 个 A2 源码仍通过旧编译器字节码比对和 8 次宿主执行。A3-only 审计仍有 119 项未完成要求，因此这里的赋值语义验收不表示完整迁移完成。

### 不可变二进制数据

前文仍把 Bytes 列为缺项；现在已有原生不可变字节序列子集。
`Bytes(buffer)` 复制可变数据，`Bytes(text)` 复制当前字符串视图的字节；
之后修改源缓冲区不会改变结果。二进制数据不要求是有效 UTF-8，NUL 和
0x80–0xff 均原样保存。原来用 str 承载文件二进制的用法应在文件接口迁移时
改为 Bytes，并另行显式解码；当前尚未实现这个文件接口。

Bytes 支持长度、只读字节索引、迭代、成员检查、推导式、解包、内容相等、
Any 和 GC。格式化使用转义表示。底层复用固定 Buffer 的存储和只读辅助函数，
类型系统拒绝写入及转回 Buffer；这样不会通过重新命名可变对象冒充不可变性。
切片、拼接、哈希键、解码及资源读写仍需实现。

`test-as-bytes` 已接入完整宿主门禁及其必跑的故障对照；测试包含 11 类
静态诊断、O0/O2 ASan 执行，并实际观察到漏复制、地址比较和丢失 GC 根的
失败。`bytes` 工程同时加入调试/发布来宾门禁，来宾结论以运行报告为准。

本阶段结果为 `build/a3-cutover/typed-guest-bytes/result.json`：108 个程序、
108 项检查，passed 与 complete 均为 true。245 项基础宿主检查及全部功能
门禁通过；剩余 51 个 A2 源码仍通过字节码比对和 8 次宿主执行。
`as.aex`、`studio.aex` 重新构建及 shipped 编译器符号检查通过。
`build/a3-cutover/a3-cutover-bytes.json` 仍报告 119 项未完成要求；Bytes
基础类型的交付尚未替代余下标准库或允许删除旧引擎。

### 原生完整文件读写

旧 `file_read` 返回 str 或 None，旧 `file_write` 接收 str 并以 -1 表示失败；
这不能直接沿用为 A3 的静态二进制接口。现在原生 `file_read(path)` 返回
Bytes，`file_write(path, Bytes(text))` 成功返回实际字节数。失败通过
IOError、PermissionError、ValueError 或 MemoryError 传播到源码调用位置。
旧程序必须迁移失败处理和二进制/文本边界，不应把结果直接当作 str。

实现位于 `runtime/file.c`、`backend/llvm/file.c`；前者拥有打开到关闭的
资源生命周期，后者只处理静态参数、返回布局和错误位置。读写处理中断和
短传输，关闭失败不会伪装为成功。写入仍是普通创建/截断操作，发生错误时
可能留下部分内容。全文件读取上限与 Bytes 一致，为 64 MiB。

权限按实际持有的读/写位及目录范围检查。宿主受限路径逐级通过目录
描述符打开并拒绝符号链接，LogitOS 使用实际内核权限上限。`test-as-files`
覆盖真实文件、六类静态错误和故障注入，包括无进展写入、短读写、EINTR、
读写/分配/关闭失败及每次关闭次数。故意漏写、漏关闭、吞掉关闭错误、
移除权限位检查或允许符号链接跟随后，测试必须失败，且是正例的前置门禁。

来宾增加普通文件操作及 none/fs/net/raw/proc/gui 六种实际内核授予，
分别运行调试/发布构建。剩余标准库、文本解码、唯一资源对象及 with 仍须
继续迁移；新接口通过不能作为删除依赖它们的旧自举引擎的理由。

本阶段完整验收见 `build/a3-cutover/typed-guest-files/result.json`：122 个
程序、122 项检查全部通过，passed/complete 均为 true。宿主 245 项基础
检查、各功能正反向门禁及剩余 51 个 A2 源码比对和 8 次宿主运行通过。
`as.aex`、`studio.aex` 重新构建和 shipped 编译器检查通过。最终文件专项
门禁还验证了后续参数触发 GC 时临时路径的保活。
`build/a3-cutover/a3-cutover-files.json` 将 file_read/file_write 记为已有
原生实现，剩余要求从 119 降为 117；完整 A3-only 迁移仍未完成。

### 显式文本解码与完整二进制工程

现在旧程序读取 UTF-8 文件时可使用 `file_read(path).decode()`，返回 str；
读取任意二进制时继续保留 Bytes。解码严格拒绝无效 UTF-8 并抛 ConversionError，
不隐式替换或截掉坏字节。普通 `str(bytes)` 仍提供转义表示。语义检查、LLVM
调用生成和运行时验证分别位于 `sema/bytes.c`、`backend/llvm/bytes.c`、
`runtime/bytes.c`，不依赖 Python 或 VM。

`tests/fixtures/astyped/binary` 已从内存数组演示改为真实多文件工程：
生成器输出明确的小端格式，主程序读取文件，reader 检查版本、头部和载荷
长度。目录中的 README 提供可执行命令。测试使用独立编码器逐字节验证
生成结果，并覆盖空载荷、短头部、长度不符、版本错误、字节序错误、缺失
文件和 CLI 参数错误。

`test-as-binary` 同时检查全部 1,112,064 个 Unicode 标量及其编码的各处截断，
以及原生解码的 O0/O2 执行、Optional/容器和 GC 所有者保活。过长编码检查、
截断边界和解码根被故意移除时，必须观察到断言或 ASan 失败。
来宾使用相同正常/损坏输入，记录输入哈希，并增加来宾生成文件后再读取的
闭环。该工程完成不代替剩余标准库、唯一资源、Studio 和自举的独立验收。

最终来宾报告为 `build/a3-cutover/typed-guest-binary-final/result.json`：
138 个程序、138 项检查，passed/complete 均为 true。宿主各功能门禁通过；
初次完整命令因基础测试仍无参数运行 binary 而失败，同步真实文件参数后，
245 项基础检查在 `binary-base-recheck.log` 中单独重跑通过。
第一次来宾验收正确收到 `reader.as` 的异常位置，但旧脚本只接受 main.as；
脚本现核对预期完整路径、构建快照归属以及非零行列，随后来宾全量重跑通过。
`as.aex`、`studio.aex` 重建和 shipped 检查通过。A3-only 审计仍为 117 项。

首批原地改写的示例为 hello、fib、dict、strings、exc。`test-as-examples` 与来宾门禁共用迁移前捕获的完整输出；故意取消交换但保留成功标记的负对照必须失败。旧自举测试只对其余 VM 示例做字节码比较，不把这些原生示例计为旧编译器通过。

### 综合标准库与原始词法工具迁移

`checked_numeric`、`use_mod`、`stdlib` 已在原路径改为 A3，使原始示例的
原生输出门禁增加到 11 个。综合示例仍调用 11 个库并保留原有九条断言；
Range 到 List 的物化和实数边界转换显式书写。输出的明确变化包括：
溢出打印类型化异常名称、PI 使用原生 f64 的 17 位格式、集合输出先排序。
原始输出留在 `build/a3-cutover/original-examples.json`，这些变化不是功能删除。

实际组合暴露了旧 256 项类型表不足和同名本地文件遮蔽标准库的问题。
`std.*` 导入、别名及点号子目录解析现由 `frontend/import.c` 负责；普通导入
继续本地优先。`sema/types.c` 统一分配固定地址的 1024 项类型表，超限只报告
首个容量诊断，失败声明跳过整个缩进体，仍能报告后续独立错误。
`test-as-imports` 的两个负对照实际观察到错误的库查找和错误恢复失败。

`tests/unit/aslexdump.as` 也已改为 A3，显式解码文件并检查 Any token 字段。
原 `test-selfhost-lex` 现构建并运行这个原始工具，不再复制 A2 lexer 缓存。
59 个实际库/示例源码在调试和发布构建下共 118 条 token 流与 C 词法器一致。
额外 ASan 检查覆盖正常输入、用法错误、缺失文件和非法 UTF-8；故意改变
checksum 后，输出与独立 C oracle 不符。完整 token 字节检查继续由
`test-as-lexer-lib` 承担，不能把 checksum 相等当作无碰撞证明。

最终报告 `build/a3-cutover/typed-guest-tools/result.json` 通过 148 个原生程序，
包括实际 aslexdump 工具读取来宾文件的两种构建。该私有磁盘没有编译器或 VM。
完整宿主门禁及 245 项基础检查通过（`import-regression.log`）；后加入的工具
适配由 `lexer-tool-check.log` 和 `lexer-tool-control.log` 单独验收。
`as.aex`、`studio.aex` 重建和 shipped 检查通过；目录重构的并行音频门禁一度
尚未接线，接线完成后 `import-wiring-final.log` 验证 377 个片段均已处理。
剩余 47 个 A2 源码仍通过原有编译器比对与 4 次宿主执行。
A3-only 审计为 110 项（`a3-cutover-tools.json`）；系统接口、唯一资源、
剩余标准库、自举编译器和 Studio 全面适配仍须继续，不能据此删除其旧实现。

上述解包与五个示例的完整回归通过 70 次真实来宾执行（`typed-guest-unpack/result.json`）；剩余 A2 的 54 个源码仍通过两种编译器字节码比对及 14 次宿主运行。这里的 A2 计数是剩余迁移范围，不是最终保留范围。

随后 `gc` 示例也原地改写为 A3，六个示例通过宿主 O0/O2 完整输出比对。`test-as-process` 检验 args 的复制/参数分隔、初始化顺序、GC 对象计数与字节计数的区别；强制每次分配触发回收后，删除参数列表的构造根必须实际产生 ASan use-after-free。对应来宾回归仍须以运行报告为准。

原有迁移清单只固定了库 API 和示例，容易遗漏 VM 注册的内建接口。补充的 `migration-builtins.json` 固定了 45 个函数以及全部已注册常量；它是删除旧注册代码前的核对依据，不能代替接口的原生行为测试。

参数、GC 计数和模块级解包的最终回归通过 76 次真实来宾执行，报告为 `build/a3-cutover/typed-guest-process-final/result.json`。完整宿主门禁、剩余 53 个 A2 源码的字节码比对和 12 次宿主执行，以及 Makefile 接入检查通过。此前 `process-regression.log` 的来宾构建因测试源码与旧编译器不同步而失败，不计为通过；重建后完整重跑得出这里的结果。

原 closure 示例已在原路径改为 A3，保留共享计数器及两个 lambda 的输出。`test-as-closure` 已验证 10 类诊断和 O0/O2 执行，涵盖共享/独立捕获、重新赋值、递归、多级捕获、泛型、推导式的延迟绑定、self、Optional 返回和异常。强制每次分配触发 GC 后，去掉环境扫描或捕获单元根都必须观察到 ASan use-after-free。对应来宾用例已接入，验收以实际报告为准。

闭包阶段的完整回归已通过 80 次真实来宾执行：`build/a3-cutover/typed-guest-closure/result.json`。剩余 52 个 A2 源码仍通过两种旧编译器的字节码比对与 10 次宿主执行；这些数字只记录尚未迁走的范围。继续增加了条件表达式首次发现捕获变量的回归，修正恢复分支事实时读取尚未初始化的编译器栈槽的问题。
