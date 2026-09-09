# LogitOS bug 审查清单（2026-09-09）

目前收录 **13 项产品代码缺陷：6 项 P1、7 项 P2**，另列 1 项测试装置缺陷。首次报告为 11 项；按用户要求重新启动三个子代理后，受限功能复核补充 B12、B13，未重复计算同一根因。P1 表示优先修复的权限、内核生命周期或数据丢失问题；P2 表示明确的功能和兼容性错误。这是当前工作树的定向审查，不是全系统无遗漏证明，也不表示这些问题都是本轮首次发现。

审查基线：`fe748e31e5d236cd74c1ca49f896b18170c7d54d` **加当前未提交改动**。已阅读 AGENTS.md、CLAUDE.md，核查源码和调用链。未修改实现、未提交代码、未运行破坏性 guest 测试。原有 tracked 文件的 git 状态在审查前后相同；本轮新增此报告，探针和日志在 `/tmp/logitos-audit-20260909/`，隔离构建在 `build-audit-main-0909/`。

**证据边界：**2 项用实际源码进行了主机功能复现，1 项用真实头文件验证了接口矛盾，10 项为高置信静态调用链结论。没有在 guest 中复现本清单，也未运行全量 CI、完整 ISO/磁盘构建、真实硬件或真实站点矩阵。主机结果不能替代 guest 验收。涉及并行竞争的运行时后果尤其需要 guest 回归确认。

| ID | 优先级 | 缺陷 | 本轮证据 |
|---|---|---|---|
| B01 | P1 | 合成设备写入先执行命令、后检查权限 | 静态调用链 |
| B02 | P1 | 路径遍历忽略后端持久化目录权限 | 静态调用链 |
| B03 | P1 | SHM 的 map/close 不验证调用进程持有的句柄与权限 | 静态调用链 |
| B04 | P1 | 编辑器把读取失败当空文件，允许覆盖原文件 | 静态调用链 |
| B05 | P1 | CORS 预检缓存扩大授权范围，缓存通配符与即时检查不一致 | 静态调用链、官方规范核对 |
| B06 | P1 | 阻塞读写未持有独立文件引用，并发 close 可提前释放对象 | 静态调用链 |
| B07 | P2 | RAMFS 重命名目录不迁移子项 | 实际 RAMFS 源码主机复现 |
| B08 | P2 | Studio 忽略保存失败并继续运行磁盘旧版本 | 静态调用链 |
| B09 | P2 | waitpid 返回编码与自带 wait 宏冲突 | 实际头文件探针、静态调用链 |
| B10 | P2 | popen 在 stdout 已关闭时误关子进程的输出管道 | 实际 popen.c 主机复现 |
| B11 | P2 | errno 是全局变量，与已实现的多线程运行时冲突 | 静态调用链 |
| B12 | P2 | libc 错误适配未统一返回 -1，并丢失具体错误原因 | 子代理与主代理静态复核 |
| B13 | P2 | 后端删除/迁移失败后不恢复已经变更的链接元数据 | 子代理与主代理静态复核 |

## B01 — 合成设备写入在权限检查之前产生副作用

位置：[vfs.c:777](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:777)、[kdiag.c:480](/Users/wangzhe/system/LogitOS/c/kernel/core/kdiag.c:480)、[syscall.c:506](/Users/wangzhe/system/LogitOS/c/kernel/exec/syscall.c:506)。

`vfs_write()` 先调用 `k_write()`，确认该设备由 kdiag 处理后才调用 `check_file(..., MAY_WRITE)`。`k_write()` 并非无副作用的识别函数，而是直接执行 `kdiag_write()`；后者可修改 profiler 状态，也含内核诊断中止操作。`SYS_WRITE_FILE` 直接到达这一路径，没有先执行文件写权限检查。

因此，对于具有 CAP_FS、但没有该节点写权限的普通进程，事后返回权限错误无法撤销已经执行的命令；若命令不返回，权限检查根本不会发生。没有声称不持有 CAP_FS 的进程也可通过这一路径。

修复方向：先用无副作用的节点识别/属性查询确定提供者，再检查权限，最后执行写入。回归应验证被拒写入时提供者调用次数为零。**本轮未向 guest 发送诊断中止命令。**

## B02 — 目录 search 权限读取了错误的元数据来源

位置：[vfs.c:294](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:294)、[vfs.c:354](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:354)、[vfs.c:1102](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:1102)、[vfs_meta.c:75](/Users/wangzhe/system/LogitOS/c/fs/vfs_meta.c:75)。

路径解析的 `visit()` 只查询 `vmeta_lookup()`；查询不到就按默认可遍历处理。但 LogitFS 的 `chmod` 经后端 `setattr` 保存，`attrs_of()` 也优先读后端，根本不保证 RAM 元数据表中有对应记录。

一个只在后端记录为 0700 的目录，路径遍历可能不检查其执行/search 权限。若目录内某个文件本身允许读，知道完整路径的其他用户可能穿过本应不可遍历的祖先目录访问它。最终文件权限仍有检查，不能笼统写成所有文件权限失效。

修复方向：让组件遍历与 stat/最终节点检查共享权威属性来源，避免递归进入路径解析。应覆盖 chmod 后与重启后的两种状态，而不只测试 RAM 元数据后端。

## B03 — SHM 权限只在 open 检查，map/close 没有句柄约束

位置：[mmsys.c:303](/Users/wangzhe/system/LogitOS/c/kernel/mm/mmsys.c:303)、[mmsys.c:339](/Users/wangzhe/system/LogitOS/c/kernel/mm/mmsys.c:339)、[vma.c:349](/Users/wangzhe/system/LogitOS/c/kernel/mm/vma.c:349)、[shm.c:288](/Users/wangzhe/system/LogitOS/c/kernel/mm/shm.c:288)。

`SYS_SHM_OPEN` 传 uid 并检查权限；`SYS_SHM_MAP` 直接把全局 segment 编号和请求的读写权限交给 `vma_reserve_shm()`，后者只检查 segment 存活、大小和范围，然后增加引用。没有进程句柄表，也没有再次检查 uid 或该句柄获准的访问模式。`SYS_SHM_CLOSE` 同样直接对全局编号 `shm_put()`。

这使 open 的权限拒绝无法约束另一条独立的 map 入口，也无法保证 close 只释放调用者持有的引用。只读打开与可写映射的权限关系同样没有被保存。可能造成跨进程共享数据访问和引用生命周期破坏。

**保留并纠正旧说法：**mmsys.c:316 的注释认为此前的 open 检查足以保护 0600 segment；当前调用链没有强制 map 之前必须成功 open，这个理由不成立。修复应建立进程句柄、访问模式和引用所有权，并明确 fork/exit/close 的转移规则。未做 guest 越权或内存破坏复现。

## B04 — 大文件读取失败后，TextEdit/Studio 允许保存空内容

位置：[textedit.c:533](/Users/wangzhe/system/LogitOS/c/apps/gui/textedit.c:533)、[textedit.c:561](/Users/wangzhe/system/LogitOS/c/apps/gui/textedit.c:561)、[studio.c:300](/Users/wangzhe/system/LogitOS/c/apps/gui/studio.c:300)、[logitfs.c:837](/Users/wangzhe/system/LogitOS/c/fs/logitfs.c:837)。

TextEdit 的缓冲上限为 8,000 字节，Studio 为 65,536 字节。LogitFS 的全文件读取在 `size > max` 时返回 -1。两个编辑器仅处理 `r > 0`，没有将加载失败与新建空文件区分；TextEdit 还设置 `saved = 1`。

因此，在可写的现有大文件上，打开后可显示空白，随后 TextEdit 的保存或 Studio 的运行前保存会按空缓冲/新输入覆盖原文件。读取错误的其他原因也会走同一分支。在 RAMFS 上读取语义不同，只返回前缀，保存则可能截断尾部。

**本轮对初始候选的纠正：**不是“LogitFS 只读取前 8 KB”，而是“LogitFS 拒绝读取，编辑器吞掉失败”；这比显示截断内容更容易误导用户。

修复方向：先判断文件大小或使用可增长缓冲；加载失败须保留错误状态并禁止覆盖保存，除非用户明确选择另存为。小型编辑器可以拒绝大文件，不能把拒绝变成成功打开空文件。

## B05 — CORS 预检缓存键不完整，且命中规则扩大 header 授权

位置：[js_webapi.c:668](/Users/wangzhe/system/LogitOS/c/apps/browser/js_webapi.c:668)、[js_webapi.c:699](/Users/wangzhe/system/LogitOS/c/apps/browser/js_webapi.c:699)、[js_webapi.c:1154](/Users/wangzhe/system/LogitOS/c/apps/browser/js_webapi.c:1154)、[js_webapi.c:1652](/Users/wangzhe/system/LogitOS/c/apps/browser/js_webapi.c:1652)。

缓存只存目标 origin、method、credentials 和 header 列表。存入的 origin 来自 `f->url`，不是发起页面的 origin；目标 URL 的路径和查询也不在键中。静态缓存 `g_pfc` 在页面 install/close 时未清除。于是一个页面/端点得到的预检结果可能被其他页面或同源不同端点复用，跳过原本需要的 OPTIONS。

另外，即时预检检查只在 `!creds` 时把 `Access-Control-Allow-Headers: *` 当通配符，缓存命中却在 `list_has(..., "*")` 时直接放行所有 author headers。缓存状态改变了同一权限规则的答案。

影响是未经该请求上下文授权的实际请求可能被发送；实际响应仍有 CORS 检查，**本轮不据此声称任意跨域响应都能读取**。[Fetch 标准的预检缓存规则](https://fetch.spec.whatwg.org/#cors-preflight-cache)要求匹配请求 origin、当前 URL 和 network partition key 等字段。

修复方向：按规范补全缓存键，并统一缓存与即时预检的 header 判定。回归应覆盖两个发起 origin、同一目标 origin 下两个路径，以及有/无缓存和有/无 credentials 的相同输入。未做真实页面网络复现。

## B06 — 阻塞 I/O 借用文件指针，没有保护等待期间的生命周期

位置：[syscall.c:374](/Users/wangzhe/system/LogitOS/c/kernel/exec/syscall.c:374)、[proc.c:239](/Users/wangzhe/system/LogitOS/c/kernel/exec/proc.c:239)、[file.c:374](/Users/wangzhe/system/LogitOS/c/kernel/exec/file.c:374)、[file.c:1241](/Users/wangzhe/system/LogitOS/c/kernel/exec/file.c:1241)、[sched.c:1123](/Users/wangzhe/system/LogitOS/c/kernel/sched/sched.c:1123)。

`SYS_READ/WRITE` 从 `proc_fd_get()` 获得借用指针，未增加文件引用便进入可能阻塞的处理。wait_event 在调度时释放 BKL，同一进程另一线程能够 close 同一 fd。eventfd 的最后 close 会唤醒等待者后立即释放 `eventobj`；等待者恢复时还会访问 waitq 和 `e->val`。唤醒并不等于等待函数已经返回。

file.c:1248 的注释称阻塞读者持有自己的 fd 引用，但当前 syscall/get 路径没有创建这份独立引用。线程共享 fd 表，不能把“描述符还存在时的一份引用”当成“每个进行中的调用各有一份引用”。

影响为并发 close 时的内核对象 use-after-free 风险；pipe 路径也使用相同的借用方式。修复方向：查找文件时持有独立引用，贯穿整个阻塞调用；最后关闭仅在引用清零后销毁等待队列和后端对象。本轮确认了源码缺口，未做 guest 竞争复现，未断言具体可利用结果。

## B07 — RAMFS 重命名非空目录，子文件仍挂在旧路径

位置：[ramfs.c:226](/Users/wangzhe/system/LogitOS/c/fs/ramfs.c:226)、[vfs.c:955](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:955)。

RAMFS 将完整路径存入每个 entry。`rf_rename()` 只修改目录自身的 `path`，没有更新子孙 entry；VFS 调用后只迁移 VFS 元数据，不会修正后端 entry。

实际 `ramfs.c + vfs_path.c` 的主机探针：创建 `/old/child` 并写入 2 字节，重命名 `/old` 为 `/new`，得到：

```text
mkdir=0 write=2 before=2 rename=0 new_count=0 new_child=-1 old_child=2
```

重命名报告成功，但新目录为空，新路径读取失败，子文件还留在已不存在的旧父路径下。这不是数据字节已被擦除，而是命名空间关系损坏。

修复方向：以组件边界匹配并原子迁移子孙路径，预先检查全部路径容量、冲突及自包含移动；若尚不支持，应拒绝非空目录重命名。主机探针没有证明 guest 挂载/GUI 行为。

## B08 — Studio 保存失败仍清除 modified 并运行旧代码

位置：[studio.c:135](/Users/wangzhe/system/LogitOS/c/apps/gui/studio.c:135)。

`run_file()` 无条件调用 `write_file()`、忽略返回值、设置 `modified = 0`，随后执行磁盘路径。磁盘空间不足、只读文件或其他写入错误时，编辑器可能执行上一次保存的脚本，同时不再标识当前编辑尚未保存。这与 B04 不同：即使小文件成功打开，也会触发。

修复方向：确认全量写成功才清除 modified 并启动运行；失败时保留编辑内容和未保存状态，显示错误。需要以确定性写失败覆盖运行前保存分支。

重新启动后的独立复核确认：普通 Ctrl+S 在 TextEdit:561 和 Studio:339 已检查负返回值，保存失败会保留原来的未保存标记。B08 仅针对 Run 路径，不能扩大为所有保存入口都忽略失败。

## B09 — waitpid 与 WEXITSTATUS/WIFEXITED 的编码矛盾

位置：[io.c:165](/Users/wangzhe/system/LogitOS/c/apps/libc/src/io.c:165)、[proc.c:563](/Users/wangzhe/system/LogitOS/c/kernel/exec/proc.c:563)、[sys/wait.h:13](/Users/wangzhe/system/LogitOS/c/apps/libc/include/sys/wait.h:13)。

内核直接写入 `exit_code`；libc waitpid 保留原值。但自带宏按信号低位、退出码高位解码。直接用实际头文件验证内核会返回的值 7：

```text
kernel_exit_code=7 WIFEXITED=0 WEXITSTATUS=0 WIFSIGNALED=1
```

普通退出会被移植程序误判为信号终止；非零退出码不能通过公开宏正确获取。源码已有注释解释为保留旧测试而不转换；这是现存的接口冲突，不因有注释就变为一致。system/pclose 的局部左移也没有修复 wait/waitpid。

修复方向：保留原始 syscall ABI 时，在 libc 边界转换，并同步修改依赖旧编码的测试；若要支持正确的信号退出判断，还需避免把 signal termination 与普通 `exit(128+sig)` 混为一谈。此探针只验证头文件与源码返回格式的矛盾，没有启动 guest 子进程。

独立复核补充：`wait()` 直接包装 `waitpid()`，只计同一项；`system()` 和 `pclose()` 已自行左移普通退出码，若统一 waitpid 的编码，必须移除它们的重复转换。`sigtest_main.c` 已测试退出 7，却断言原值等于 7；另一些宏测试只用退出 0，恰好无法区分两种编码。不能将这些测试通过作为接口一致性的证据。

## B10 — popen 在 stdout 已关闭时关闭了刚重定向的输出

位置：[popen.c:69](/Users/wangzhe/system/LogitOS/c/apps/libc/src/popen.c:69)、[proc.c:231](/Users/wangzhe/system/LogitOS/c/kernel/exec/proc.c:231)。

fd 分配从最小空槽开始。父进程原来的 stdout 关闭后，`popen(..., "r")` 的 parent_end 可能就是 1。子进程先把 child_end dup2 到 1，随后无条件 `close(parent_end)`，把新的 stdout 管道也关闭了。

编译仓库原始 popen.c，仅替换 FILE 适配层；fork/pipe/dup2/close/exec 使用主机实现，得到：

```text
stdout_initially_open: bytes=2 content=ok
stdout_initially_closed: bytes=0 content=
```

正常对照退出 0，触发情况退出 1。缺陷位于 popen.c 的 fd 操作顺序。guest 的控制台回退可能使文本跑到控制台而非管道，因此不据主机结果断言 guest 的全部输出位置或退出状态。

修复方向：先关闭不需要的 parent_end，再重定向 child_end，并检查 dup2 失败。验证 0/1/2 原先不同开闭组合。

## B11 — errno 尚未随 pthread 支持迁移为线程局部状态

位置：[errno.h:9](/Users/wangzhe/system/LogitOS/c/apps/libc/include/errno.h:9)、[io.c:25](/Users/wangzhe/system/LogitOS/c/apps/libc/src/io.c:25)、[pthread.c:869](/Users/wangzhe/system/LogitOS/c/apps/libc/src/pthread.c:869)。

errno 声明和定义仍为单个 `int`，而 pthread/TLS 已有实现。线程 A 的失败调用设置错误码后，线程 B 的 open/sem_trywait 等失败会覆盖同一个变量；A 在调用返回后判断 errno 可能读到 B 的错误，错误处理和重试分支不再可靠。

`libc_host_errno_shim.c` 的旧注释仍基于“每进程一个线程、无 TLS”，与当前 pthread.c 不符。没有发现调度器保存/恢复 errno 的替代机制。

修复方向：使用与当前 TLS 方案一致的线程局部 errno 访问器，并覆盖主线程、工作线程和 host shim。测试应安排两个线程顺序交错写不同错误码，再分别读取，避免只寄希望于随机竞争。未进行 guest pthread 复现。

## B12 — libc 未把内核错误转换成一致的失败返回值与 errno

位置：[io.c:28](/Users/wangzhe/system/LogitOS/c/apps/libc/src/io.c:28)、[io.c:86](/Users/wangzhe/system/LogitOS/c/apps/libc/src/io.c:86)、[io.c:183](/Users/wangzhe/system/LogitOS/c/apps/libc/src/io.c:183)、[proc.c:543](/Users/wangzhe/system/LogitOS/c/kernel/exec/proc.c:543)、[vfs.c:952](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:952)。

共享辅助函数 `fail(r, e)` 只在 `r < 0` 时设置固定的 errno，然后原样返回 r。当前内核不止返回 -1：不支持的 waitpid 选项返回 `SIG_E_NOSYS=-5`；跨挂载 rename 返回 `VFS_EXDEV=-18`。结果分别成为 `waitpid -> -5 / ECHILD` 和 `rename -> -18 / ENOENT`。

调用者使用 `== -1` 检查失败时会漏判；根据 EXDEV 选择复制后删除的移植程序也进不了正确分支。这不是“缺少 WUNTRACED 功能”或“内核吞掉错误”，而是已经传到 libc 的错误没有正确适配。waitpid、rename、unlink 的表现来自同一辅助函数和调用方式，合并为一项，不按 API 数量计数。

修复方向：在 libc 边界统一失败返回 -1，并按各 syscall 的实际错误空间映射 errno。不能一律使用 `errno = -r`：部分 syscall 的 -2/-5 是本项目自定义状态码，含义需要分别处理。io.c 文件头仍称“内核失败只有 -1、LogitFS 没有权限”，与当前实现不符，修复时应保留历史解释并补上现状。当前证据为静态调用链，未运行 guest syscall 探针。

## B13 — 删除失败后，VFS 链接元数据已经不可逆地前进

位置：[vfs.c:873](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:873)、[vfs.c:886](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:886)、[vfs_meta.c:222](/Users/wangzhe/system/LogitOS/c/fs/vfs_meta.c:222)、[vfs.c:1182](/Users/wangzhe/system/LogitOS/c/fs/vfs.c:1182)。

`vfs_delete()` 先执行 `vmeta_unlink()` 删除原记录、更新链接组并提升 survivor，随后才让后端删除数据或迁移 canonical 文件。若后端返回错误，VFS 原样返回错误，却没有恢复刚才的元数据。文件操作失败并不意味着原来的链接关系仍然成立。

一个静态可达的例子是 RAMFS 硬链接路径长度：VFS 元数据允许的路径容量大于 RAMFS 的 96 字节 entry 容量；创建别名仅登记 VFS 元数据。删除 canonical 名称时，需要把后端文件迁移到 survivor；RAMFS 可因目标路径超长拒绝迁移，但 survivor 已被当作独立/canonical 名称，不再解析到仍位于旧路径的文件。结果为删除报错后别名读取失败；不是已证明数据字节被擦除。

修复方向：在后端操作成功后提交元数据变化，或在失败时可靠回滚；同时在需要未来后端迁移的路径上校验后端限制。此项针对失败原子性，区别于 B07 的成功 rename 漏迁移子项。仅静态复核，未注入设备故障或写入用户文件。

## T01 — memstream 测试的主机参考程序自身崩溃

位置：[libc_memstream_test.c:282](/Users/wangzhe/system/LogitOS/tests/unit/libc_memstream_test.c:282)、[tests/libc.mk:98](/Users/wangzhe/system/LogitOS/tests/libc.mk:98)。

本轮 `make BUILD=build-audit-main-0909 test-libc-memstream` 失败于命名为 `memstream_glibc` 的参考程序，不是 LogitOS 的实现。此主机为 macOS，参考程序实际链接 Darwin libc。size=0 的 fmemopen 返回 NULL 后，测试仍调用 fread。

单独以 ASan 编译该参考测试，定位为 `libc_memstream_test.c:285` 的 NULL FILE 读取，经主机 `flockfile` 崩溃。该测试没有在此平台完成差分比较，不能把错误 139 当成 LogitOS memstream 缺陷。

修复方向：检查参考平台和返回值；不支持该参考语义时明确 SKIP 并给出支持 glibc 的验证环境，或提供规范明确的可移植对照。不要让参考装置先崩掉。

## 本轮验证与未纳入的旧问题

- `test-mk-wired` 通过：149 fragments，148 reachable，1 declared（schedneg 的自递归例外）。
- `check-abi` 通过：29 kernel structs，79 calls，81 SYS_* names bound。
- B07、B10 功能探针和 B09 头文件探针重复执行得到相同结果；全部退出状态在 [probe-results.json](/tmp/logitos-audit-20260909/probe-results.json)。
- [源码 SHA-256 和 mtime 清单](/tmp/logitos-audit-20260909/source-manifest.json)用于识别共享工作树之后的漂移；[门禁日志](/tmp/logitos-audit-20260909/gates.log)、[参考测试 ASan 日志](/tmp/logitos-audit-20260909/memstream-reference-asan.log)保留了本轮证据。
- CLAUDE.md 中“munmap 完全不做跨核 shootdown”已过时：[vmm.c:562](/Users/wangzhe/system/LogitOS/c/kernel/mm/vmm.c:562) 已有释放 frame 前的批量刷新。没有把旧句子重报为 bug，也不据此宣称全部 TLB 行为已经通过 guest 验证。
- “close 永远返回成功”“sh 不支持 -c”也不再符合当前源码，未列入。缺失的动态链接、完整 POSIX 功能、真实硬件覆盖不作为本轮 bug 凑数。
- 阻塞 I/O 中用户缓冲区在等待期间被另一线程撤销映射的风险值得继续核查，本轮没有把它额外计算为已确认条目。

建议首先处理 B01/B02/B03/B06 的权限与生命周期边界，以及 B04 的编辑器数据保护；随后修 B05，最后统一处理 B07–B13 的功能回归。每项修复都应增加能够在修复前失败、修复后通过的回归，guest 性质在 guest 中验收。

并行审查尝试覆盖内核、存储网络和浏览器应用。三个子进程均被平台安全检查中止，未交付完整审查报告；其候选仅在主进程重新阅读实际源码并核实后才纳入。本报告不宣称完成整个网络、驱动、密码学和媒体解码器的深度审计。

**后续状态（同日，按用户要求重新启动）：**三个子代理改为明确受限的普通功能复核，分别检查退出状态、RAMFS 命名空间、编辑器加载与保存。三者均正常完成，没有再次被拦截。未运行越权、网络攻击、内核竞争或崩溃触发测试，也未改写实现；存储代理只复跑了现有主机 RAMFS 探针。它们确认 B04/B07/B08/B09，补充了上述 B12/B13；主代理再次核对源码后更新报告。
