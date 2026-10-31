# LogitOS 新调度器与新内存管理：两份论文式设计文档（计划）

存放位置（superpowers 约定）：
- 本计划批准后存为 `docs/superpowers/plans/2026-11-01-sched-mm-design-docs.md`
- 调度设计文档：`docs/superpowers/specs/2026-11-01-hetero-interactive-sched-design.md`
- mm 设计文档：`docs/superpowers/specs/2026-11-0X-<mm主题>-design.md`（调度文档审完后再写）

# 第一部分：调度

## Context
用户要按自己的思路重做 LogitOS 两个最大的子系统，先调度、后内存管理（mm）。这一轮**只写设计文档**：现代、偏实验、
从论文里提炼再加上用户自己的东西。目标机器以 i7-14700KF（8P+12E/28T，Z790）为主，X79（20 线程同构 Xeon）是
同一设计的退化情况。实现以后交给 Sol（只写实现）、Astra（只读审查，含风格），Claude 跑门和提交。

用户已定（2026-11-01）：
- 主线：大小核异构感知（HFI 能力表、Thread Director 分类、能耗/HWP-EPP）
- 三个实验性创新点（全文核心）：**N1 帧截止时间交互链**、**N2 容量归一化的 EEVDF**、**N3 带类型的核心租约**
- 配套章节（不算创新点）：**调度器–mm 迁移代价契约**（为 mm 重写预留窄接口）、**证据门控**（每个启发式带计数器、开关、撤回标准）
- 注意力思想（Vaswani 2017）：用户决定**暂时搁置**，本文不涉及
- 形式：论文式，中文正文、英文标识符，`docs/superpowers/specs/2026-11-01-hetero-interactive-sched-design.md`

## 现状基线（已从代码查明，起草前在 HEAD 复核 file:line）
- `c/kernel/sched/sched.c`：一个全局就绪环 `g_ring` + 一把锁 `g_sched_lock`；`pick_min_locked()` 线性扫描最小加权 vtime
  （CFS 规则无红黑树）；`g_vnow` 下限；nice 权重 4096..274；无亲和性/调度类/截止时间/优先级继承/负载统计
- 抢占：向量 32 只在 `in_kernel==0` 时 `schedule()`，内核线程（wm、wm-band）实际不被定时器抢占；唤醒无 IPI（等下个节拍）
- 定时器：BSP PIT 100 Hz，AP 周期 LAPIC（1.6 ms+0.2 ms×序号）；全局 ktimer 堆只在 BSP 到期（约 10 ms 精度）；无 TSC-deadline/NOHZ
- 拓扑：P/E 类型已识别（`c/kernel/cpu/cpu_platform.c`，`smp_topology.h` `core_class`）只用于日志；无 E 核 L2 分组；HFI 只清点未启用
- 电源：调频只认 0x2D/0x3E，0xB7 上无 HWP
- 历史：`docs/superpowers/specs/2026-06-10-m25-p4-percpu-sched.md` 每核运行队列实验因数据撤回（偷取泵、BKL 阀门、BSP 节拍门控死锁、
  同相节拍）；其重启条件现已大体满足（BKL 已除、M27 等待队列、真机）
- 负载：wm 一线程包办输入/网络轮询/渲染；wm-band×(ncpu−1)；kaudio 约 21 ms 周期靠 63 ms 余量；输入路径不唤醒任何线程
- uthread（M30）是一对一（每个用户线程一个内核 `struct thread`），租约需要新的上调（upcall）路径

## 文档结构（论文式）
0 标题/日期/HEAD/TRANSPARENCY（未在本机跑的都标「未验证」，CPUID/MSR 事实引 SDM 卷章修订）
1 摘要  2 动机（现状数字 + 目标与证据标准：输入→上屏 p99、帧截止错过率、≥30 fps 拖动/打字/滚动、kaudio 余量、smptest 不劣于全局环、能耗）
3 背景（14700KF 拓扑、HFI/ITD、HWP/EPP、TSC-deadline；X79 对照）
4 基线与教训（现 sched.c + M25 P4 六条教训 L1–L6，后文每个机制必须引用它遵守的教训；全局环是要打败的基线，写明撤回标准）
5 相关工作（每组末尾写「我们拿什么 / 不拿什么」）
6 设计总览（分层图、不变量、锁模型）
7 机制：拓扑与容量模型（SMT / L2 模块 / 核类型 / L3 四级域，HFI 刷新）；每核运行队列 + EEVDF（N2）；放置（容量适配×意图−迁移代价−SMT 惩罚）；交互链（N1：输入中断生成令牌，沿唤醒/端口/futex/等待队列/锁继承传递，CBS 式预算）；核心租约（N3：带核类型与 L2 模块，
  链可在有界时间收回，上调路径）；无节拍时间（每核时间轮 + TSC-deadline，退役全局 ktimer）；重调度 IPI（合并）与内核抢占点；电源（HWP EPP 按层设定，X79 保留 EIST）
8 与 mm 的接口（N4：`struct sched_mm_ops`——迁移代价、地址空间 CPU 掩码、带宽压力；回调 on_switch_as/on_migrate/on_cpu_idle；现 mm 只给桩实现）
9 新颖贡献（N1/N2/N3，与先前工作逐条区分；mm 契约与证据门控作为工程配套）
10 评测计划（每核飞行记录器；QEMU 只测逻辑并注入假 HFI 表；性能结论只在真机；先写撤回规则）
11 分阶段实施 S0–S7（入口门、出口门、撤回触发）  12 风险与缓解  13 未决问题  14 参考文献（只收已核实条目）

## 执行步骤
1. **文献调研（Workflow，并行）**：6 个 Sonnet 调研代理只用网页检索——R1 非对称多核放置与公平、R2 Intel HFI/ITD/HWP/TSC-deadline 与
   Alder/Raptor Lake 实测研究、R3 Linux EEVDF/EAS/PELT/uclamp/ITMT/sched_ext、R4 交互与优先级继承（Apple QoS/turnstile、Android ADPF、
   Fuchsia、MMCSS、PI、proxy execution、CBS）、R5 核心分配与用户态线程（scheduler activations、Arachne、Shenango、Caladan、Shinjuku、
   Concord、ghOSt、UMCG）、R6 陷阱/定时器/方法（Decade of Wasted Cores、timing wheels、NOHZ）。每条产出账本行
   （标题、作者、会议、年份、URL/DOI、支撑原文片段、用在哪节）。
2. **对抗式核实**：独立核查代理逐条按标题重新查找，核对出版方/DBLP/官方文档，二手来源不收；状态 已核实/已修正/剔除。
   N1/N2/N3 各派一个代理找最接近的先前工作反驳新颖性，站不住就弱化表述。
3. **基线复核**：经 `bin/mac` 在 HEAD 复核文中所有 file:line。
4. **起草**：Claude 按账本写全文；自检（每个机制对应目标和教训、每个目标有证据标准、每句事实有引用/实测/未验证、参考文献全部已核实）。
5. **Astra 只读审查**（Mac 上 `codex exec -m gpt-6-astra -s read-only`）：技术正确性（IPI/等待路径丢唤醒、链继承成环、租约收回竞态）、
   file:line 是否属实、X79 退化、新颖性是否夸大、评测能否证伪、文风；Claude 逐条修复或说明理由，文末附修改记录。
6. **交用户审阅**，未决问题由用户拍板后写回；提交到主仓库当前分支（推送等用户说）。

## 验证（"完成"的定义）
- 文件在上述路径，14 节齐全；参考文献账本 100% 已核实，正文无剔除条目
- 文中 file:line 全部能在 HEAD 用 grep 对上
- Astra 的阻断项全部关闭；用户签字确认未决问题；已提交
- 之后再把 S0–S7 拆成实现计划交给 Sol/Astra

# 第二部分：内存管理（mm）

## Context
用户要按自己的思路重做 mm，形式和调度一样：**一份独立的论文式设计文档**，中文正文、英文标识符，放 `docs/superpowers/specs/`；
**调度文档审完以后再写**（顺序进行，复用同一套文献核实与审查流程）。和调度文档之间的接口（迁移代价、回调、配额）在两份文档里写成
**同一份契约**，互相引用。

用户选定的研究方向（四个都要）：
- **大页与大块内存（folio）**：可变大小内存块 + 2 MB / 1 GB 大页为一等公民，减少 TLB 未命中和页表开销，配合碎片整理
- **可扩展的分配器与地址空间**：每核分配器、细粒度/无锁地址空间、高效 TLB 击落（PCID、批量/延迟击落），28 线程下 mmap/缺页不打架
- **现代回收策略**：多代 LRU、按访问热度分级、压缩内存、更聪明的 OOM，内存紧张时桌面不卡
- **与调度器协同**：对接调度文档的 mm 契约——按 L2 分组/大小核放置、迁移代价估计、交互链线程的内存保障、租约进程的内存配额

## 现状基线（已从代码查明，起草前在 HEAD 复核 file:line）
`c/kernel/mm` 共 29 个文件、约 1.2 万行；教学级设计、按 512 MiB 机器调参，插桩与主机测试扎实（`mmhost.h` 的 `MM_HOSTTEST` 接缝必须保留）。
- **物理内存**（`phys/pmm.c`）：每 4 KiB 帧 1 位的位图 + 平铺的 refcnt/pincnt 数组，无伙伴系统、无空闲链表、无每核缓存，一把 `pmm_lock`；
  低区（<1 GiB，恒等映射）/高区（直接映射）；连续分配是线性首次适配；**所有元数据（pmm、rmap、pcache、swap 表）必须物理连续且在 1 GiB 以下**，
  这是真正的内存上限（16 GiB / 32 GiB 机器上是硬伤）
- **内核虚拟内存**：低端恒等映射前 1 GiB（2 MiB 叶）+ 高半区 64 TiB 直接映射；各地址空间共享内核顶层项、私有 PDPT[1]；
  **用户内存不用大页**（fork 拒绝用户大页）；PAT PA1=WC
- **kheap**（`phys/kheap.c`）：dlmalloc 式边界标记、9 个二次幂桶、4 MiB arena 只增不缩、一把 `kheap_lock`；每核 magazine（6 个尺寸类、32 深）
  把锁获取从 3070 万降到 112；内核栈 32 KiB 无保护页
- **用户地址空间**：静态表 64 个空间 × 32 个 VMA，线性查找、一把全局 `vma_lock`；缺页分 COW/ANON/SWAP/FILE/SHM；mmap 文件映射只读；
  无 brk、无 ASLR、**无 PCID**；TLB 击落（向量 240）一次只能一个发送者、接收方整页表重载、超时直接停机；全局 `g_tlb_gen` 惰性代数
- **页缓存/回收**：页缓存只读、无写回、最多 32 个文件、自带时钟指针；回收是基于物理帧号的 CLOCK + rmap（rmap 池耗尽的帧本次开机永不回收）、
  只做直接回收（无 kswapd）；swap 无 swap cache、不跨重启；OOM 按最大 RSS 选
- **DMA**：无 IOMMU，`dma_alloc_coherent` 走直接映射 WB；一把全局锁
- **锁**：pmm/kheap/vma/rmap/pcache/swap/dma 各一把全局自旋锁 + 256 个按 CR3 哈希的地址空间睡眠锁（会撞桶）；`tlb_service` 被自旋锁等待循环轮询，
  把锁与 TLB 代码耦在一起
- **已知问题**（`docs/bugs/01-kernel-mm-cpu-sched.md`、架构页）：TLB 停机与「有界放弃 + 代数」设计矛盾、mprotect 丢失换出页的修改、fork 改父进程 PTE
  不广播击落、堆碎片导致有空闲仍失败、回收无进程间公平
- **与调度器的现有接缝**：CR3 切换全走 `vmm_switch`（sched.c 多处调用）；每次切换后 `tlb_gen_sync`、系统调用门 `sched_tlb_gen_check`；
  `kheap_cpu_index` → `this_cpu()->index` 选 magazine；mm 锁用 waitq 睡眠；缺页入口 interrupts.c → `mm_fault`

## 文档结构（与调度文档同构）
0 标题/日期/HEAD/TRANSPARENCY  1 摘要  2 动机（现状数字 + 目标与证据标准：缺页延迟、mmap/munmap 在 28 线程下的扩展性、TLB 未命中率、
内存压力下的帧截止错过率、大页覆盖率、回收开销）  3 背景（x86-64 分页、PCID/INVPCID、大页、14700KF 的 TLB 与缓存层级；X79 对照）
4 基线与教训  5 相关工作（每组末尾写「拿什么 / 不拿什么」）  6 设计总览  7 机制（物理内存与 folio、大页与整理、每核分配器与 kheap、
地址空间与锁、TLB 击落、页缓存与写回、回收与分级、压缩、OOM、DMA）  8 与调度器的契约（与调度文档第 8 节同一份）
9 新颖贡献候选（文献调研后提出若干个，**由用户挑选**后再起草）  10 评测计划  11 分阶段实施  12 风险与缓解  13 未决问题  14 参考文献

## 执行步骤（调度文档完成并经用户审阅后开始）
1. **文献调研（Workflow 并行）**：M1 大页与 folio（Ingens、HawkEye、Trident、Linux THP/mTHP/folio）；M2 可扩展地址空间与分配器
   （RadixVM、Bonsai、maple tree、per-VMA lock、mimalloc/snmalloc、slab/magazine）；M3 TLB 击落（LATR、ABIS、Linux 批量/延迟刷新、PCID）；
   M4 回收与分级（MGLRU、TPP、Memtis、zswap/zram、PSI、OOM 策略）；M5 调度–mm 协同（缓存感知放置、带宽争用、内存 QoS）。账本格式同调度。
2. **对抗式核实**参考文献；把候选新颖贡献（附先前工作对比）交用户挑选。
3. **基线复核**（HEAD 上的 file:line），起草，自检。
4. **Astra 只读审查**（技术正确性：TLB 击落与 PCID 的竞态、COW/fork 与大页的交互、回收与 DMA 钉住页、锁顺序；新颖性；可证伪性）；修复并附修改记录。
5. 交用户审阅、拍板未决问题，提交（推送等用户说）。

## 验证
同调度文档：14 节齐全、账本 100% 已核实、file:line 在 HEAD 可对上、Astra 阻断项关闭、用户签字、已提交；
另加一条：两份文档里的调度–mm 契约逐字段一致。
