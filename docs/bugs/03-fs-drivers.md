# c/fs 与 c/drivers 缺陷猎取报告（分区 03：VFS / LogitFS / fsck / lfsro / procfs / ramfs / 块层 / DMA 驱动 / 网卡 / USB / 音频）

- 日期：2026-09-16
- 审计基线：当前工作树（`8f6e83c42` 之后，工作树 clean）
- 方法：先读 `docs/CODE_AUDIT.md`（2026-08-04，含修复状态）、`docs/BUG_BACKLOG.md`（P1/P2 已修）、`docs/BUG_REVIEW_2026-09-09.md`（B01–B13/T01）与 CLAUDE.md 的 OPEN BUG 段，全部已知项不重复上报。然后按指令的优先级深读：`logitfs.c` 全文（log_commit 三道屏障、bfree 延迟释放、alloc_hint 不变量、崩溃恢复）、`fsck.c`（superblock 校验 + 日志重放 + 修复收敛）、`lfsro.c`、`vfs.c`/`vfs_path.c`/`vfs_meta.c`、`bcache.c`、`blkdev.c`（flush/barrier 契约与 bounce）、`nvme.c`（PRP/超时/4Kn 部分块）、`ahci.c`（PRDT/重试/DMA 所有权）、`ata.c`、`virtio.c`/`virtio_blk.c`/`virtio_net.c`、`dma.c`、`hda.c`（CORB/RIRB/BDL）、`xhci.c`+`xhci_ring.c`、`ehci.c`、`e1000.c`/`rtl8139.c`/`netdev.c`（e1000e/pcnet/rtl8169 经环 helper 抽查）、`part.c`（MBR/GPT）、`usb_bot.c`/`usb_storage.c`/`usb_desc.c`/`hid_report.c`（设备提供数据的边界）、`procfs.c`、`ramfs.c`、`fsbench.c`/`vfsctl.c`；`char/`、`gpu/`、`platform/`、`power/`、`timer/`、`core/irq|device` 按风险仅做了模式级略读。
- 定性标准：全部条目均给出 file:line；静态推理可闭环的标 CONFIRMED，需要运行时窗口或硬件行为才能坐实的标 SUSPECTED。未改任何源码，只写了本文件。

## 发现（按严重度）

### [medium] [CONFIRMED] lfsro 的目录遍历完全没有 size 上界，伪造/损坏镜像可让挂载锁内的设备扫描达百万次命令级

- 位置：`c/fs/logitfs/lfsro.c:100-115`（`dir_lookup`）、`c/fs/logitfs/lfsro.c:147-165`（`lr_mount_locked` 的校验范围）、对照 `c/fs/logitfs/logitfs.c:619`（`size_ok`）

```c
static uint32_t dir_lookup(struct lro *L, const struct lfs_dinode *dino, const char *name)
{
    uint32_t nblk = (dino->size + LFS_BS - 1) / LFS_BS;     /* size 完全未校验 */
    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t phys = bmap(L, dino, b);
        if (!phys || rd(L, phys, L->dblk) < 0) continue;    /* 是 continue，不是 break */
```

- 触发与后果：`lr_mount_locked` 只查 magic、`block_size`、`total_blocks != 0`、`inode_count != 0`，不做 `fsck_super_valid` 的几何校验（`logitfs.c` 里同类扫描有 `size_ok(sz)` 把 size 钳到镜像大小、遇 0 指针 `break`）。lfsro 里 `dino->size` 是盘上数据：一个声称 4 GiB 的目录 + 双间接链里填上设备内合法块号，`dir_lookup`/`lr_count_locked`/`nth_ent` 会循环约 100 万次，每次一次 4 KiB 设备命令或一次越界拒绝；空洞走 `continue` 一直扫到底。整个过程在 `LRO_OP` 的 `spin_lock_irqsave(&L->lock)`（每挂载实例锁、irqsave）下执行——持自旋锁跑数十万条设备命令，同时阻塞同挂载的每一个读者。`total_blocks` 本身也未与设备容量核对（`rd()` 只查 `>= total_blocks`；越设备部分由 `blkdev.c:111` `in_bounds` 兜住，所以无内存不安全，剩下的就是拒绝失败的扫描本身）。
- 修复建议：`lr_mount_locked` 直接复用 `fsck_super_valid`（该函数本就为“挂载与离线一致”而写在共享头里）；`dir_lookup`/`lr_count_locked`/`nth_ent` 进入前对 `size` 做 `size <= total_blocks*BS` 的钳制，`!phys || rd<0` 改为 `break`（与 logitfs 同语义）。

### [medium] [CONFIRMED] logitfs 后端变更成功但 flush 失败时，RAM 与盘静默分叉（delete/mkdir/write 建新/rename 四条路径）

- 位置：`c/fs/logitfs/logitfs.c:1424-1425`（write 建新）、`:1447-1448`（mkdir）、`:1463-1466`（delete）、`:1711-1712`（rename）；对照失败即回滚的范本 `:1634-1644`（setattr）

```c
    tx_begin();
    if (dir_remove(parent, leaf) < 0) { log_abort(); return -1; }
    inode_trunc(in);
    in->type = T_FREE;
    if (flush_inode(ino) || flush_inode(parent) || flush_bitmap()) { log_abort(); return -1; }
```

- 触发与后果：`dir_remove`/`dir_add`/`inode_write` 一旦成功，**RAM 里的 inode 表、目录树与 alloc_hint 已经前进**；随后 `flush_inode||flush_bitmap` 失败（log 满：默认几何 log_max=63，目录重写超过约 31 块即触发；或 bcache/设备 IO 错误）走 `log_abort`——只把 `tx_count` 和 deferred frees 清零，**不回滚 RAM**。后果按路径分别是：
  - delete（上面代码）：调用方拿到 -1 以为没删成，但 RAM 目录项已消失、`in->type=T_FREE`、指针已清零；文件对应盘块的 deferred free 被丢弃 → RAM 位图里“已用但无引用”的泄漏；盘上一切未动。更糟的组合是会话内 `ialloc` 复用这个 RAM 已自由的 ino 并提交——盘上旧目录项（仍在）就此指向新文件，下次 fsck 报 multi-link。
  - mkdir / write 建新：新条目在 RAM 可见、盘上不存在，重启后“消失”；`ialloc` 消耗的 `next_id` 不回退（flush_bitmap 里根块的 staging 一并被 abort 丢弃，RAM 计数已加）。
  - rename：RAM 已改名、盘未动，调用方得到 -1 却看到“已改名”的会话视图；两条目录的旧块同因 deferred-free 丢弃而泄漏。
  这与代码已文档化的“rename 提交期 IO 错误仅告警”（CODE_AUDIT 遗留清单）是两个不同窗口：本条是**提交之前的 flush 失败不回滚 RAM**，setattr 路径（:1634-1644）对同一形态做了完整回滚，四个落下的路径没有。
- 修复建议：把 flush 失败的处理统一成 setattr 的形状——失败即把本事务改过的 RAM inode/目录恢复成事务前快照（`dir_add/dir_remove/inode_write` 成功时各留一份旧 inode 内容与受影响目录 inode 的副本），或最低成本先补 delete：失败时把 `in->type`/`in->size`/指针与父目录条目恢复。并给“目录重写 + flush”的组合留出 log 预算检查（事务开始时按 `nblk+2` 预检 `log_max`），把“大目录必失败”从深处的 `log_add` 失败提前成可预期的 `ENOSPC`。

### [low] [CONFIRMED] resolve_parent 把父路径静默截断到 127 字节，而 resolve() 无长度上限——create/mkdir/delete/rename 可能落在另一个（前缀）目录里

- 位置：`c/fs/logitfs/logitfs.c:1180-1186`（`resolve_parent`），对照 `:1133-1164`（`resolve` 无路径长度限制）、`c/fs/vfs/vfs_path.h:62`（`VFS_PATH_MAX 256`）

```c
    char dirpath[MAX_PATH];                    /* MAX_PATH = 128 */
    int dl = 0;
    for (int i = 0; i < s && dl < MAX_PATH - 1; i++) dirpath[dl++] = path[i];
    dirpath[dl] = 0;
```

- 触发与后果：VFS 允许最长 255 字节的绝对路径进入后端。`resolve()` 全程走到 NUL，所以**已存在**路径的读写不受影响；但创建/删除/改名走的 `resolve_parent` 把父路径部分截到 127 字节且不报错。截断通常落在分量中间（查找失败、返回 -1，无害）；可构造的是截断恰好落在 `/` 边界且该 127 字节前缀**恰好是已存在目录**：`write "/<60字目录>/<60字目录>/<3字目录>/file"`（父路径 129 字节）会把 file 建进被截断出的祖先目录，静默错位。叶子名超长已有显式拒绝（`:1176`），父路径没有对称处理。
- 修复建议：`resolve_parent` 里 `if (s > MAX_PATH - 1) return NOINO;`——与叶子超长的既有处理对齐；或把 `dirpath` 提到与 VFS 路径同长。

### [low] [CONFIRMED] lfsro 的 size 接口把 ≥2 GiB 的 uint32 size 截断成负 int（logitfs 已修的同类，lfsro 漏了）

- 位置：`c/fs/logitfs/lfsro.c:171-177`（`lr_size_locked`）、`:260-267`（`lr_ent_size_locked`）

```c
    return ino.type == LFS_T_FILE ? (int)ino.size : -1;
...
    if (read_inode(L, de.ino, &ino) < 0) return 0;
    return (int)ino.size;
```

- 触发与后果：`logitfs_size_locked`（logitfs.c:1374-1377）对同一问题显式加了 `in->size > (uint32_t)INT32_MAX → -1` 的守卫；lfsro 的两个入口没有。size 在 [2^31, 2^32) 时返回负数：`vfs_size` 的调用方把 -1 当“不是文件/出错”，`ent_size` 返回 0。无内存安全问题，属接口正确性/一致性问题，且 lfsro 是唯一暴露面时更隐蔽。
- 修复建议：与 logitfs 相同的 `> INT32_MAX` 拒绝。

### [low] [CONFIRMED] vfs_mounts_render 在输出恰好填满 max 时不写 NUL；vfsctl 的 vfsmeta 节点 size 与 read 在截断边界不一致

- 位置：`c/fs/vfs/vfs.c:346-362`（`:360` 的 `if (n < max) buf[n] = 0;`）；`c/fs/ctl/vfsctl.c:344-353`（size）对照 `:365-371`（read）

```c
    if (n < max) buf[n] = 0;                    /* vfs.c:360 —— n == max 时无终止符 */
...
    if (w == 1) return vfs_mounts_render(tmp, (int)sizeof tmp)
                  : vmeta_render(tmp, (int)sizeof tmp) + vfs_cred_render(tmp, (int)sizeof tmp);
```

- 触发与后果：两点。(1) `vfs_mounts_render`/`vmeta_render` 的 `put()` 在 `n == max` 时返回恰好 max 且不写终止符；vfsctl 把调用方缓冲区原样传入，当前调用链用返回值计长所以未爆，但这是埋给下一个“当成 C 串用”的调用方的。(2) vfsmeta 节点的 **size 是两个 render 的长度相加**（第二个 render 覆写同一个 scratch，只贡献长度），而 read 路径是先 render vmeta、`n < max` 才追加 vfs_cred：当 vmeta 渲染恰好填满 max（元数据表满、VMETA_N 条长路径可达）时 size 报 `max + cred_len`，read 只返回 `max` 且第二段被丢弃——stat 与实际可读字节数矛盾，读方按 size 读必得短读。
- 修复建议：`vfs_mounts_render` 收尾无条件保留一字节终止符（`max` 语义改为“含 NUL”）或文档化“返回值为长度、不保证 NUL”；vfsmeta 的 size 改为“渲染进 scratch 时用与 read 相同的追加逻辑”，两者共用同一个渲染函数。

### [low] [CONFIRMED] vfs_getdents 的 name/type/size 三次独立后端查询之间会释放并重取挂载属主——目录并发变更时同一 merged 索引可能拼出“张三的名字 + 李四的类型/大小”

- 位置：`c/fs/vfs/vfs.c:661-712`（`:675` `vfs_ent_name`、`:681` `vfs_ent_is_dir`、`:705` `vfs_ent_size` 三次独立调用）

```c
        const char *nm = vfs_ent_name(abs, i);      /* 各自 mount_pin → io_domain_enter/leave */
        ...
        e->type = vfs_ent_is_dir(abs, i) ? VT_DIR : VT_REG;
        ...
            int sz = vfs_ent_size(abs, i);
```

- 触发与后果：每次 `vfs_ent_*` 内部各自 `mount_pin(..., owned=1)` 进入 per-mount `io_domain` 又立即退出。于是本轮 getdents 的三次后端枚举之间，另一核的 create/delete 可以推进同一目录——同一索引 `i` 的名字来自变更前的第 i 项、类型/大小来自变更后的第 i 项。结果只到用户态可见的数据错配（`ls`/getdents 显示错配的名字/类型/大小），无内存安全问题；目录是 logitfs 时后端每次枚举还会重读目录块，放大窗口。
- 修复建议：`vfs_getdents` 在整个循环外持一次 owned `mount_pin`（三次 `vfs_ent_*` 改走已 pin 的 `fs_ent_*` 直调），或后端提供“一次快照枚举”的批量接口。

### [low] [SUSPECTED] log_commit 在日志体 bwrite 已失败的情况下仍写入提交记录——恢复会因 bcrc 不符丢弃整个事务，而检查点可能已部分落盘

- 位置：`c/fs/logitfs/logitfs.c:366-401`（`:374` 体写入失败只记 `rc`，`:387-388` 照常写头）

```c
    for (int i = 0; i < n; i++) {
        if (bwrite(sb.log_start + 1 + (uint32_t)i, tx_bufs[i])) rc = -1;   /* 失败不 abort */
        ...
    if (bwrite(sb.log_start, tx_hdr)) rc = -1;
    if (bcache_sync()) rc = -1;                            /* B2 -- the commit point */
    for (int i = 0; i < n; i++)
        if (bwrite(tx_targets[i], tx_bufs[i])) rc = -1;    /* 检查点照常进行 */
```

- 触发与后果：注释（:362-364）只论证了“barrier 失败不 abort 是安全的”，但**日志体写入失败**是另一类：头仍会带着对这个体的 `bcrc` 提交，随后检查点把新内容装到目标块。崩溃后恢复（fsck.c `log_validate`）因盘上体与 `bcrc` 不符把事务整体丢弃、回到“pre-Tk+1”——而目标块上已经落了 post-Tk+1 的字节，恰好制造日志要防的“既非旧也非新”状态。触发条件苛刻（`bwrite` 失败 = bcache_write 失败 = 设备 IO 错误；此时检查点大概率同样失败，且 `rc` 会传给调用方），所以标 SUSPECTED、定 low。
- 修复建议：体写入失败时走与 `log_abort` 等价的收尾（清头、跳过 B2/B3、返回 -1），把“提交记录只为完整落盘的体作保”变成代码事实而不是概率。

### [low] [CONFIRMED] xHCI per-endpoint 事件队列满时丢弃的是“最新”事件（注释写的是“最旧”），一个被丢的 int-in 完成事件会把该端点的上报路径楔住

- 位置：`c/drivers/usb/xhci.c:334-348`（`ep_push_event`，`XHCI_EVQ = 8`，`xhci.h:126`）；下游 `:1256-1273`（`xhci_int_in_arm` 的 `if (ep->inflight) return 0;`）与 `:1275-1302`（`xhci_int_in_poll`）

```c
static void ep_push_event(struct xhci_ep *ep, const struct trb *e)
{
    uint8_t nt = (uint8_t)((ep->ev_tail + 1) % XHCI_EVQ);
    if (nt == ep->ev_head) return;      /* 注释：drop the oldest news —— 实际丢的是最新 */
    ep->ev[ep->ev_tail] = *e;
    ep->ev_tail = nt;
}
```

- 触发与后果：队列满时直接 return，被丢的是**刚到的这条**；而对应传输环的 TRB 已被 `xhci_events` 里 `xring_complete(&ep->ring, 1)` 退役。若丢的是 int-in 的完成事件：`ep->inflight` 恒为 1，`xhci_int_in_arm` 永远提前返回 0，`xhci_int_in_poll` 永远 `-1`——该端点从此不再产生任何上报，直到设备重插（`xhci_free_slot`）。单端点积到 8 条未取事件需要 `inflight` 门失效或错误风暴，触发面窄，故整体 low；其中“丢最新 + 注释写反”是 CONFIRMED，楔死链是 SUSPECTED 级的运行时后果。
- 修复建议：队列满时改为覆盖并前移 `ev_head`（真丢最旧），或在丢弃路径上同步 `ep->inflight = 0`/触发 `recover_endpoint`，保证被丢事件的 TD 不会把 arm/poll 状态机卡死；并修正注释。

### [low] [CONFIRMED] virtio_net 的 vq_publish 在 dma_buffer_submit 失败时静默不发布描述符，而 TX 路径已先消费槽位——每次失败永久漏一个 TX 槽（RX 侧重发同理缩圈）

- 位置：`c/drivers/virtio/virtio_net.c:147-160`（`vq_publish` 首行 `if (!dma_buffer_submit(mem)) return;`）、`:186-192`（`vnet_tx` 先 `tx_free[d] = 0; tx_cur = next;` 再 publish）、`:237`（RX 重发）

```c
    tx_free[d] = 0;
    tx_cur = (uint16_t)ring_next(d, TX_BUFS);
    vq_publish(&txq, d, tx_mem[d], VNET_HDR_LEN + len, 0);   /* 失败 = 该描述符不上环 */
```

- 触发与后果：`dma_buffer_submit` 只在设备 blocked 或缓冲不处于 safe 状态时失败（设备被 virtio_stop/隔离之后）。此时 `vnet_tx` 已把 `tx_free[d]` 清零、`tx_cur` 前移，但设备永远看不到这个描述符、也永远不会回它的完成——`tx_reclaim` 不会释放它；TX 只有 16 个槽，16 次失败后 `g_tx_qfull` 永久增长、发送全停且无诊断。RX 侧 `vq_publish` 失败同样让该 buffer 永久退出接收环（budget 循环里它已被消费）。无内存安全问题，是“静默资源退役”。
- 修复建议：`vq_publish` 返回 int；TX 失败时回滚 `tx_free[d]=1`、`tx_cur` 不动并返回 -1；RX 失败时至少计数并尝试重发，或把失败升级为 `virtio_stop`（与传输超时同等级处置）。

### [low] [CONFIRMED] fsbench 的 num() 对任意长数字串无界累积——有符号 long 溢出（UB），产物经 (int)/(uint32_t) 截断后进入分配与设备命令

- 位置：`c/fs/ctl/fsbench.c:95-100`（`num`），消费点 `:121-122`（`bench_blk` 的 `sectors * 512u`、`kmalloc(bytes)`）、`:487-499`（命令分发）

```c
static long num(const char *s)
{
    long v = 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return v;
}
```

- 触发与后果：向 `/dev/fsbench` 写 `blk 99999999999999999999 1` 即有符号溢出（UB）；截断后 `bytes = sectors*512` 可回绕为 0/小值，`kmalloc(0)`、`blk_dev_read(..., count, buf)` 的后续都建立在这个未定义值上。该节点写路径有 CAP 门（vfs_write 的 check_file），触发者是持 CAP 的本地进程，且 kheap 对巨分配会失败——所以是 UB/健壮性缺陷而非可利用原语。
- 修复建议：`num()` 加饱和（`if (v > (LONG_MAX - d) / 10) return LONG_MAX;`），`bench_blk` 入口对 `sectors` 按 `d->nsectors` 与 `BENCH` 上限钳制。

### [low] [SUSPECTED] HDA codec_cmd 在一次超时之后继续复用 RIRB 读指针——CORB/RIRB 无关联标记，下一条 verb 可能收到上一条迟到的响应并当作答案

- 位置：`c/drivers/audio/hda.c:316-388`（`codec_cmd`：`:357-387` 超时只打印并返回 -1，`rirb_rp` 不前进而下一次调用照常消费下一个响应）

```c
    deadline = hda_ms() + 50;
    for (unsigned long spins = 0; spins < 200000000ul; spins++) {
        unsigned rwp = (r16(h, RIRBWP) & 0xFF) % h->rirb_entries;
        if (rwp != h->rirb_rp) {
            h->rirb_rp = (h->rirb_rp + 1) % h->rirb_entries;
            if (resp) *resp = (uint32_t)(h->rirb[h->rirb_rp] & 0xFFFFFFFFu);
```

- 触发与后果：verb 超时后控制器稍后完成并写入 RIRB；下一个 `codec_cmd` 读到的第一个响应其实是上一条的。HDA 响应本身不带命令关联字段（协议层面无法甄别），代码也没有在超时后做“清空/重同步 RIRB”的处理。后果是被污染的 `codec_param` 值流入 widget 枚举与增益/路由设置——最坏是接了错误路由、静默无声或增益写错 widget，属“看起来在工作的错误配置”而非内存问题。需要真实硬件上一次 50 ms 级超时才可坐实，故 SUSPECTED。
- 修复建议：`codec_cmd` 超时路径上做一次 RIRB 重同步（读 `RIRBWP` 并把 `rirb_rp` 直接追平、ack `RIRBSTS`），后续命令从干净状态开始；或在超时后对控制器跑一次 CORB/RIRB 重置序列。

## 已知问题（未重复上报）

以下是本次通读中再次遇到、但已被树内文档或前次报告记录在案的问题，遵守不重复上报的约定：

- `logitfs_rename` 提交期（log_commit 内）IO 错误后仅告警、可能留下盘上 UAF——CODE_AUDIT.md「遗留风险清单」原文记录；本报告 medium 第 2 条覆盖的是它之外的“flush 失败不回滚 RAM”窗口。
- `ent_name` 返回 `namebuf` 内部指针、锁释放后由调用方读取（logitfs.c 并发块 hazard 1、lfsro.c 同型、vfs.h `const char *(*ent_name)` 的 ABI 问题）——文件内已完整论证，修复在 VFS ABI（`vfs_ent_name_copy` 已存在）。
- LogitFS lazytime：atime 只写 RAM、崩溃即失——`logitfs.c` 时间戳注释明确为设计取舍（Linux `lazytime` 同型）。
- BUG_REVIEW 2026-09-09 的 B01（kdiag 写先于权限检查）、B02（目录 search 权限读 vmeta 而非后端）、B04（编辑器把读取失败当空文件）、B07（ramfs 改名不迁移子项）、B13（vfs_delete 元数据先行不可回滚）——本轮复核仍在，均为已报告项。
- ramfs arena 的 bump 分配器不回收改写文件的旧空间、容量固定——`ramfs.c` 文件头明示的设计取舍（“这就是它是 /tmp 不是 /home”）。
- procfs size() 与首读不共享快照是刻意的（negative control `PROCFS_SNAPSHOT_AT_OPEN`）；`/proc` 大文件截断带显式 marker——文件内已文档化。
- e1000 TX 依赖“调用方必持 net_lock”的隐式约定与 TX 自旋 100 万次的上限——e1000.c:538-539 注释与 CODE_AUDIT 驱动节均记录。
- rtl8139 拒绝在 rx_poll 中 ack ISR（实测会把卡搞坏，rx->ack 尾延迟 ~10 ms）——rtl8139.c:211-235 大段测量记录，属已知受控缺陷。
- xHCI 不支持 isochronous（`xhci_configure_ep` 的 `default: continue`）、EHCI 根口不支持 LS/FS（需 companion，`root_reset` 明说）、无带宽仲裁之外的周期调度——两文件头部的 scope 声明。
- ata PIO：SECCOUNT=0 语义与 255 扇区上限、`ata_flush()` 依赖每次 write 自带 FLUSH CACHE——ata.c:82、:92-95 的注释与 CODE_AUDIT 低危项。
- P1（用户地址直通 DMA）已由 `blkdev.c` 的 per-call bounce 修复（2026-09-09 修正注记），本轮复核 `blk_rw`/`blk_submit` 的 `user_buffer` 拒绝路径完好，未再报。
- CODE_AUDIT 已修项（NVMe cid 校验、virtio 超时 desync、e1000 RX 长度、logitfs bfree/inode_read/dir_add 盘上数据校验、`streq` 限长、H-9 大目录扫描）本轮逐一复核仍在位，未发现回归。

## 覆盖度声明

- 逐行深读：`c/fs` 的 logitfs.c、fsck.c、lfsro.c、logitfs_fmt.h、logitfs_identity.inc、vfs.c、vfs_path.c、vfs_meta.c、ramfs.c、procfs.c、bcache.c、fsbench.c；`c/drivers` 的 blkdev.c、nvme.c、ahci.c、ata.c、part.c、dma.c、virtio.c、virtio_blk.c、virtio_net.c、hda.c、xhci.c、xhci_ring.c、ehci.c、e1000.c、rtl8139.c、netdev.c、usb_bot.c、usb_storage.c、hid_report.c（前半解析器）。
- 抽查/模式级：vfs_cred.c、vfsctl.c、e1000e.c、pcnet.c（含 pcnet_ring.h/e1000e_ring.h 的长度校验 helper）、usb_core.c、usb_desc.c、virtio_gpu.c、es1370/ac97 引擎（环算术抽查）、io_domain.h/io_lock.h。`gpu/`（AMD/NVIDIA 后端）、`power/`、`platform/`、`char/`、`timer/` 未深读，本报告不对这些目录下结论。
- 未运行 guest 复现：所有 CONFIRMED 均为静态调用链可闭环的结论；涉及运行时窗口的（xHCI 事件楔死、HDA 迟到响应、log_commit 体失败）按约定标 SUSPECTED。
