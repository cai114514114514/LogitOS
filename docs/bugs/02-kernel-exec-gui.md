# c/kernel/exec + c/kernel/gui 缺陷猎取报告（分区 02：进程/ELF/AEX/信号/coredump/ptrace/syscall/PTY/poll + 窗口管理器/fb 合成/文本/剪贴板/通知/IME）

- 日期：2026-09-16
- 分区规模：`c/kernel/exec` 34 个 .c/.h 约 12,300 行，`c/kernel/gui` 22 个 .c/.h 约 12,000 行，逐行通读。

## 方法与覆盖

先读 `docs/CODE_AUDIT.md`（2026-08-04 审计全文及各修复批）并 grep `CLAUDE.md` 的 OPEN BUG/known 条目，确认已知问题清单（见文末"未重复上报"）；本分区 2026-08-04 审计中的 S1/S2/H-2/H-3/H-5 与 gui 各中低危项**在 HEAD 已逐一到源码核实为已修**（elf.c PASS 0 的 VA 界限与减法形式溢出检查、exec.c 的事务化 execve 与 `file_write` 偏移守卫、SYS_PIPE 的 hold-ref 回滚、SYS_GUI_BLIT 的 sw/sh/w/h 校验与 `fb_blit_user_rgba` 逐行 pin、wm_launch 的三处失败路径回收、reap() 的 dragging/mouse_capture/rz_win 复位、SYS_GUI_RECT/FLUSH_RECT 的 clamp、px≤512 clamp 均已在代码中确认），不再重复上报。

随后通读全部文件，重点深读：`elf.c`（两遍）/`aex.c`/`exec.c` 的边界与校验、`usercopy.c` 的 pin-and-copy 契约、`ksigframe.c` 的信号栈帧构造（plan_frame 的逐级回绕检查、MXCSR 掩码、RFLAGS 掩码）、`coredump.c` 的 emit 边界、`ptrace.c` 的 SETREGS cs/ss 剥离、`wm.c`（6668 行，两遍）的 damage/焦点/拖拽/dock-fly/Expose 状态机、`fb.c` 的目标/clip/IO 锁与 `fb_blit_user_rgba` 的行循环、`clipboard.c` 的 UTF-8 出入两侧不变量、`ime_ui.c`/`ime_learn.c` 的 Arena 与码点拒绝。

本仓注释文化极强，以下每条发现都先对照过所在文件的自述注释再定性；标 CONFIRMED 的均为"注释声称已防住/未声明而实际不防"的代码级事实，不涉及对 c/lib（OpenLogit/gfx/pinyin）内部循环的猜测——那部分属于 07 分区。

## 发现

### [medium] [CONFIRMED] execve 的 CLI 栈放置从不检查与已加载镜像的重叠 —— 跨越 entry+64 MiB 的 ET_EXEC 镜像被 eager 栈页盲目重映射：帧泄漏 + 镜像内容被栈顶掉，而加载器自己明文放行 ≤256 MiB 的镜像

- 位置：`c/kernel/exec/load/exec.c:291-299`（放置）、`exec.c:346-350`（无在场检查的映射）；对照 `c/kernel/exec/load/elf.h:349`（`ELF_MAX_IMAGE_BYTES = 256 MiB`）、`elf.h:366-369`（入口谓词只要求 64 MiB 余量）；正确写法的对照物在同文件另一条启动路径 `c/kernel/gui/wm.c:2075-2077`
- 代码：
```c
/* exec.c setup_user_stack() */
    uint64_t base = entry & ~(uint64_t)0xFFFFF;
    uint64_t top = base + 0x4000000;                 /* 64 MiB above base */
    /* Retain legacy stack placement; a large PIE BSS needs its stack above
     * the entire image, rather than blindly 64 MiB above the entry. */
    if ((img->load_bias || img->interp_base) && top < img->top + (uint64_t)stack_pages * 0x1000)
        top = (img->top + (uint64_t)stack_pages * 0x1000 + 0xFFF) & ~0xFFFull;
    uint64_t bottom = top - (uint64_t)stack_pages * 0x1000;
    if (!mm_user_range(bottom, top - bottom)) return 0;
```
```c
/* exec.c:346-350 —— 无 vmm_pte() 在场检查，对照加载器自己的 place_page()（elf.c:540-551 有）*/
    for (int i = 1; i <= eager; i++) {
        uint64_t frame = pmm_alloc_any();
        if (!frame) return 0;
        vmm_map_page(top - (uint64_t)i * 0x1000, frame, stack_flags);
    }
```
- 触发与后果：PASS 0 只要求（a）每个段落在用户窗口内、（b）`entry + 64 MiB` 在用户窗口内、（c）总映射字节 ≤ 256 MiB——一个 ET_EXEC 完全合法地拥有一个末尾在 `entry_base + 64 MiB` 之上的段（例如 entry=0x50000000、单 PT_LOAD memsz≈0x0F100000，恰在 256 MiB 帽内）。此时 `top = 0x54000000` 落在镜像内部：`bottom = 0x53F00000` 起的 eager 栈页把 PASS 1 已映射好的 BSS 页的 PTE **无条件覆盖**——被顶掉的帧永久泄漏（pmm 无 free），栈写进镜像的 BSS；若镜像的 file-backed text run 恰好延伸到该区间，`vma_reserve_fixed` 返回失败 → 走 `eager = stack_pages` 分支，整段 run 的 PTE 被栈页成片替换。仅涉及用户地址空间（不触及内核内存安全），但 loader 的 256 MiB 帽是明文邀请这种镜像的，且同一个内核里 GUI 侧启动器（wm_launch）专门为此写了 `if (img_top && need > ustack_top) ustack_top = need;`——exec.c 只对 PIE（`load_bias||interp_base`）做了同样的事，对恰好是"大 BSS"最常见形态的 legacy ET_EXEC 漏了。
- 修复建议：去掉 `load_bias||interp_base` 前置条件，对 `img->top` 一律抬高 `top`（照抄 wm.c:2076-2077 的三行）；并在 eager 循环里照抄 `place_page()` 的 `vmm_pte` 在场检查作为纵深。

### [low] [CONFIRMED] 管道两端的方向从不校验 —— read() 可以排空写端、write() 可以灌入读端，与 file_poll 自身的行为不一致

- 位置：`c/kernel/exec/fd/file.c:207-231`（`pipe_read`）、`file.c:232-267`（`pipe_write`）；对照 `file.c:526-549`（`file_poll` 明确按 `f->is_write` 分方向回答）与 `c/kernel/exec/fd/file.h:44-46`（`is_write` 字段注释："F_PIPE: 1 = write end, 0 = read end"）
- 代码：
```c
static long pipe_read(struct file *f, void *vbuf, long len)
{
    if (!len) return 0;
    struct pipe *p = f->backing;          /* 不问 f->is_write */
    ...
static long pipe_write(struct file *f, const void *vbuf, long len)
{
    struct pipe *p = f->backing;          /* 不问 f->is_write */
```
- 触发与后果：任何进程对 `fds[1]`（写端）调 read() 会与读端消费者竞争排空环；对 `fds[0]`（读端）调 write() 会向环内注入字节并置 `writers` 语义错乱（EOF 判定依赖 `p->writers`）。POSIX 要求 EBADF。无内存安全问题（`is_write` 刻意不进 `flags` 正是防止 SYS_SETNB 翻转它，file.h 注释自证方向是有意维护的状态），但同文件内 poll 尊重方向而 read/write 不尊重，是同一契约的两个门。
- 修复建议：`pipe_read` 入口 `if (f->is_write) return -1;`、`pipe_write` 入口 `if (!f->is_write) return -1;`（或按本仓风格拒绝并留一行注释）。

### [low] [CONFIRMED] wm.c 内死亡副本的 SYS_FILE_NAME 缺索引上界且 scopy 不查 NULL —— 今日不可达（syscall.c 先接管），但一旦两个门其一被挪动即是内核 NULL 解引用

- 位置：`c/kernel/gui/wm.c:2825-2834`（死亡副本）、对照活副本 `c/kernel/exec/syscall.c:621-629`（有 `i >= vfs_count("/")` 检查）与同文件有检查的兄弟 `wm.c:2863-2873`（SYS_DIR_NAME）；`wm.c:1185` 的 `scopy` 不判 `s == NULL`
- 代码：
```c
/* wm.c —— 无 i 的任何界限检查 */
    case SYS_FILE_NAME: {
        int i = (int)a;
        if ((int)c <= 0 || !user_range_ok((void *)b, (uint64_t)(int)c, 1)) return -1;
        char name[256];
        int cap = (int)c < (int)sizeof name ? (int)c : (int)sizeof name;
        scopy(name, vfs_ent_name("/", i), cap);          /* i 越界 => NULL => 解引用 */
```
```c
/* syscall.c 的活副本 —— 同一号码的另一个门已经检查了 */
        if (i < 0 || i >= vfs_count("/") || max <= 0 || !user_range_ok(...)) { ... return -1; }
```
- 触发与后果：`SYS_FILE_NAME` 在 `syscall.c` 的 switch 中先被接管（621 行），wm.c 这份当前不可达；但 `vfs_ent_name` 越界返回 NULL 是 syscall.c 副本用 `nm && nm[j]` 明文防御过的事实。本文件 wm.c:1185 的 `scopy` 对 NULL 无守卫（syscall.c:129 的同名函数同样无守卫，但它的调用点都先查了）。这是"一个号码两扇门、只有一扇上了闩"的潜伏形状。
- 修复建议：删除 wm_gui_dispatch 中已被 syscall.c 接管的文件语义死分支（SYS_FILE_NAME/SYS_READ_FILE/SYS_WRITE_FILE/SYS_DIR_NAME/SYS_DELETE_FILE/SYS_MKDIR/SYS_DIR_COUNT/SYS_GET_TIME），或在 scopy 前补 `i >= 0 && i < vfs_count("/")`；一行注释写明哪扇门是活的。

### [low] [CONFIRMED] PTRACE_ATTACH 到僵尸进程：proc_exists 把 PROC_ZOMBIE 当"存在"，tracer 白等满 2 秒超时并得到 PT_E_TIMEOUT 而非 PT_E_SRCH

- 位置：`c/kernel/exec/ptrace.c:262`（`if (!proc_exists(pid)) return PT_E_SRCH;`）、`ptrace.c:281-301`（200 tick 等待环）；根因在 `c/kernel/exec/proc.c:183`（`proc_snapshot` 接受 `PROC_RUNNING || PROC_ZOMBIE`）
- 代码：
```c
    if (!proc_exists(pid)) return PT_E_SRCH;      /* 僵尸也通过 */
    ...
    for (int i = 0; i < ATTACH_TICKS; i++) {      /* 200 tick = 2 s */
        ...
        if (!proc_exists(pid)) { ptrace_proc_free(pid); return PT_E_SRCH; }
        sched_poll_wait();
    }
    /* Did not stop. ... */
    return PT_E_TIMEOUT;
```
- 触发与后果：目标刚退出（zombie 未被 reap）时 ATTACH：`ksig_post` 因 `ksig_proc_free` 已把信号槽复位而报 SRCH（SIGSTOP 从未挂上），等待环里的 `l2->stopped` 永远为假、`proc_exists` 恒真，于是 tracer（若在持锁上下文之外只是普通线程则阻塞 2 秒）超时，返回 `PT_E_TIMEOUT`——一个语义上指"它活着但停不下来"的码，而不是"没有这个进程"。另外超时路径还会对一具僵尸补发 SIGCONT（同样 SRCH，无害但语义混乱）。
- 修复建议：ATTACH 的存在性检查与等待环退出条件改用"RUNNING 才算"（`proc_snapshot` 后查 `t.state == PROC_RUNNING`），或在 `l2` 缺失/僵尸时立即返回 `PT_E_SRCH`。

### [low] [CONFIRMED] SYS_READ_FILE 在知道文件大小之前按用户给的 max 原样 kmalloc —— 无 SYSCALL_IO_MAX 类上限，用户可反复迫使内核发起直至 ~2 GiB 的连续物理分配尝试

- 位置：`c/kernel/exec/syscall.c:558-570`；对照同文件 SYS_READ（456-469 行）与 SYS_SOCK_SEND/RECV（956-979 行）都有 `> SYSCALL_IO_MAX` 截断
- 代码：
```c
    case SYS_READ_FILE: {
        char name[128], abs[128]; int max = (int)r->rdx;
        if (!p || max < 0 || user_copy_string(...) < 0) { ... }
        if (max > 0 && !user_range_ok((void *)r->rsi, (uint64_t)max, 1)) { ... }
        proc_resolve(p, name, abs, sizeof abs);
        SYSCALL_BUF(tmp, max);            /* kmalloc(max)，max 可达 INT_MAX */
        if (!tmp) { r->rax = (uint64_t)-1; return; }
        long got = vfs_read(abs, tmp, max);
```
- 触发与后果：`max` 只要求非负且用户缓冲可写；一个程序可传 `max = 0x7FFFFFFF` 使内核在读第一个字节之前尝试约 2 GiB 的分配。kheap 会走 grow→`pmm_alloc_contig` 的整段线性扫描后拒绝（有日志、优雅失败，无越界），但每次调用都是一次全物理空闲链扫描 + 一条 `[oom]` 串口行，可被循环放大成低成本的内核侧压力源。同函数随后 `vfs_read` 反正会被文件真实尺寸封顶，先分配 max 属于纯浪费。
- 修复建议：先 `vfs_size(abs)` 再按 `min(max, size)` 分配（与 wm.c 死副本 2794-2807 行的顺序一致——那份反而做对了）；或与其他 I/O 门一致地加 `SYSCALL_IO_MAX` 截断。

### [low] [SUSPECTED] SYS_WRITE 在 fd 1/2 已关闭时静默改写串口控制台 —— 与 SYS_READ 无此回退不对称，且无注释声明这是刻意的

- 位置：`c/kernel/exec/syscall.c:434-455`
- 代码：
```c
    case SYS_WRITE: {
        ...
        struct file *f FILE_REF = p ? proc_fd_acquire(p, fd) : NULL;
        if (!f && fd != 1 && fd != 2) { r->rax = (uint64_t)-1; return; }
        ...
            if (f) got = file_write(f, tmp, n);
            else for (long i = 0; i < n; i++) serial_putc(tmp[i]);   /* fd 关着也写控制台 */
```
- 触发与后果：进程 `close(1)` 后再 write(1, ...) 不返回 EBADF，而是把字节送到串口——与 fd 0 的读路径（无任何回退，`SYS_READ` 对关着的 fd 一律 -1）不对称，也让"输出重定向已被拆掉"的程序输出不可追踪地出现在控制台上。读代码找不到声明这是刻意的注释；proc_spawn/wm_launch 都会给 fd 0/1/2 装上 tty，唯一受益者是"没有 fd 表的进程"，而内核线程不进 int 0x80。标 SUSPECTED：可能是控制台便利语义的遗留而非缺陷，但按 POSIX 与本仓"拒绝而非半实现"的惯例应显式声明或去掉。
- 修复建议：若为刻意行为，在 case 上方补一段注释说明"fd 1/2 恒为控制台"的契约；否则删掉回退，让 `!f` 一律 -1。

## 已知问题（未重复上报）

以下为本分区（或与本分区直接相关）已记录在案、经核实 HEAD 现状后不重复上报的条目：

- **`munmap` 缺跨核 TLB shootdown**（CLAUDE.md:445，2026-08-28 OPEN BUG，属 c/kernel/mm 分区）：本分区核对了 `usercopy.c:77-85` 的 2026-09-10 更正与 `syscall_entry_checks()` 的 `sched_tlb_gen_check()` 门——两侧均已把"复制时经 pin/alias 而非裸解引用"落地；`vmm_unmap_range_in` 本体的 shootdown 缺口仍按原记录开放，属 01 分区范围。
- **execve 后不清 TLB（旧 S1）与 ELF 无 VA 界限（旧 S2）**：CODE_AUDIT 2026-08-04 修复批已修。HEAD 核实：elf.c PASS 0 有 `mm_user_range(start, end - start)`（elf.c:1024）与减法形式溢出检查（elf.c:1000-1007，含注释里记名的历史 fuzzer 用例）；exec.c 的 execve 已事务化（`proc_exec_space` + 失败保留旧镜像，exec.c:517-581）。
- **SYS_PIPE 悬挂 fd（旧 H-3）/ BLIT 尺寸冻结（旧 H-5）/ file_write 偏移溢出（旧 H-2）/ SYS_GUI_RECT 65535² 循环（旧中危）/ px 无上限（旧中危）/ wm_launch 失败路径泄漏（旧中危）/ thread_create_user 失败不检查（旧中危）/ 拖拽悬引（旧中危）/ aex_info 64 字节越界读（旧低危）**：逐一在 HEAD 核实已修（`proc_fd_pair`+hold-ref 回滚 syscall.c:845-860；`bl.sw/sh≤4096`+`bl.w/h>0`+`fb_blit_user_rgba` 逐行 pin，wm.c:2990-3013 与 fb.c:579-604；`f->off > LONG_MAX - len` file.c:1051；RECT/FLUSH_RECT surface-intersect clamp wm.c:2660-2664/2768-2776；`px<1||px>512` wm.c:2707/2944 与 text_measure_dispatch.inc；wm.c:2004/2140/2158/2175/2200-2210 各失败路径均有回收；reap() 复位 dragging/mouse_capture/rz_win wm.c:3273-3275；scan_apps 改 `vfs_pread` 64 字节 wm.c:6139-6142）。
- **`enter_user.asm` 不初始化 FP/SSE 状态、fork 不继承 fenv**（CODE_AUDIT 构建节中危 + 遗留清单）：属 c/boot 分区，本分区未发现加重它的 exec 路径变化。
- **`logitfs_rename` 回滚失败仅告警、`tls_close` 不发 close_notify、`pci_find` 只扫 bus0、`.la` 栈效应不可静态校验**等"遗留风险清单"条目：均不在本分区文件内。
- **wm.c 文件头/CLAUDE.md 明文记录的既定取舍**（不作为缺陷上报）：`SYS_GUI_FLUSH` 无矩形是 ABI 上限；`fb_blur_rect`/`fb_liquid_glass` 不可裁剪故 `dmg_expand` 整面板扩张；greeter 在 g_locked 分支以 (0,0,W,H) 合成导致 `dirty_win_content` 被迫全屏上报（wm.c:966-991 已自述"真正的修复属于该分支的主人"）；"fd 0/1/2 = 串口"与串口控制台 claim-on-read 的前台模型（file.c/ksignal.c/abi 三处一致声明）；`.aex` 签名 log-but-not-enforce（aex.c 长注释明文声明这是策略缺口而非本行能关闭的）；`SYS_SPAWN`(63) 刻意永不实现（abi 注释 + exec.c 论证）；proc_waitpid 返回原始 exit code 而非 POSIX 编码（proc.c:738-756 的已记录取舍）。
