# Aether OS 代码审计报告

- 审计日期：2026-08-04
- 审计范围：`c/`（内核、驱动、文件系统、网络/TLS、密码学、共享库、用户态应用）、`rust/`、`tools/`、`Makefile`/`linker.ld`/`grub.cfg`、`tests/`，共 15 个模块
- 审计方法：15 个并行子代理逐行通读各模块源码并产出分级发现；本报告对全部「严重」与「高」级发现逐条回到源码复核（文件、行号、逻辑吻合度），中/低级发现原样保留。与 `docs/BUG_BACKLOG.md`（13 项已知未修 bug）逐条比对，重复项标注 [已知]。

## 统计

| 级别 | 条数 | 已核实 | 说明 |
|------|------|--------|------|
| 严重 | 8 | 8 | 全部复核通过 |
| 高 | 23 | 23 | 全部复核通过（含 2 条 backlog 已知项）；另有 1 条原高级核实后降级为中 |
| 中 | 51 | 抽核 | 跨子代理重复项已合并（wm_launch 泄漏、e1000 RX 长度、genroots 路径） |
| 低 | 约 96 | 抽核 | 同上合并（RUST_BIN 等三处重复） |
| 误报剔除 | 0 | — | 无整条剔除；1 条降级（e1000 TX volatile）、2 处行号修正 |

涉及模块数：15。与 backlog 重复的发现：13 项已知全部在审计中再次出现（标注 [已知]），其余均为新增。

---

## 严重问题（8 条，全部 [已核实]）

### S1. execve 换地址空间不清 TLB —— ring-3 可经陈旧 TLB 项读写已释放物理帧 [已核实]

- 位置：`c/kernel/mm/vmm.c:150-168`（`vmm_free_user`），调用点 `c/kernel/exec/exec.c:111`
- 问题：`proc_execve` 在当前活跃的同一 CR3 上调用 `vmm_free_user`：释放全部用户帧、清零 `PDPT[1]`，但全程无 `invlpg`、无 CR3 reload。`boot.asm` 只启用了 PAE，未启用 PCID，TLB 项只能靠 CR3 写入或 invlpg 失效。新映像由 `aex_load` 经 `vmm_map_page` 逐页 invlpg（`vmm.c:47`）——只有被新映像覆盖的 VA 被失效；旧程序独有 VA 的陈旧 TLB 项仍指向已归还 PMM 的物理帧，这些帧可能随后被分配给内核堆 arena 或其他进程的页表。
- 触发条件：本地 app 调用 execve 后探测旧映像曾映射的地址。
- 后果：ring-3 任意内核物理内存读写（UAF 原语）。
- 修复建议：`vmm_free_user` 末尾或 `proc_execve` 步骤 3 之后执行一次本核 CR3 reload（`vmm_switch(cr3)`，同值重载即 flush）；未来出现同 CR3 多线程进程时需接 `tlb_flush_all`（当前其实现会死锁，见 `vmm.c:181-187` 注释）。

### S2. ELF 加载器缺失用户区 VA 界限检查 —— 恶意 .aex 任意写内核内存 [已核实]

- 位置：`c/kernel/exec/elf.c:50-57`（`elf_load`）
- 问题：`elf.c:50-51` 注释声称 "the target VA range must lie wholly inside the private user region"，但代码只检查 `filesz<=memsz`、文件内偏移和 `end<start` 溢出，**没有任何对 `p_vaddr` 范围的检查**。`vmm_map_page` 会照常映射任意 VA，且 `next_table`（`vmm.c:31`）对已存在的中间页表项执行 `table[idx] |= USER`——若 `p_vaddr` 指向恒等映射的内核低内存（共享页表子树），会把共享内核 PD/PT 项标记为 USER 并覆盖其叶项；随后 `elf.c:67` 的 `memcpy((void*)p_vaddr, image+p_offset, p_filesz)` 把文件可控字节直接写进内核代码/数据。
- 触发条件：`SYS_EXECVE`、`proc_spawn`、`wm_launch` 三条加载路径均可达；一个伪造的可执行文件即可。
- 后果：内核任意地址写 → 内核代码执行（本地提权级）。
- 修复建议：`elf.c:57` 后加 `if (start < 0x40000000 || end > 0x80000000) return 0;`（USER_PDPT_IDX=1 覆盖的 1 GiB），并同样校验 `e_entry`；`next_table` 的 `|= USER` 应限制在用户子树内。

### S3. AetherFS `bfree()` 对盘上块号无边界校验 —— 内核堆越界写 [已核实]

- 位置：`c/fs/aetherfs.c:77`（`bfree`），触发点 `aetherfs.c:148-170`（`inode_trunc`）
- 问题：`bfree` 仅查 `b != 0` 即 `bit_clear(b)` → `bitmap[b >> 3]`（`aetherfs.c:69`）。bitmap 缓冲区只有 `bitmap_blocks * BS`（合法镜像 4 KiB），而 `inode_trunc` 把 inode 的 `direct[i]`、间接/双间接块表项（全是可被篡改的盘上数据）直接传入，`b` 可达 0xFFFFFFFF → 攻击者可控偏移的堆越界写。mount 只校验了 superblock 计数（`aetherfs.c:390-398`），从未校验 inode 内块号 `< total_blocks`。
- 触发条件：挂载伪造/损坏的 AetherFS 镜像后，删除或覆写一个 direct 指针被篡改的文件。
- 后果：内核堆越界写，偏移与位值部分可控。
- 修复建议：`bfree`/`bit_clear` 内加 `if (b == 0 || b >= sb.total_blocks) return;`；更彻底是在 `imap()`/`inode_trunc` 使用任何盘上块号前统一校验范围。

### S4. AetherFS `inode_read()` 有符号比较绕过 —— 大长度缓冲区溢出 [已核实]

- 位置：`c/fs/aetherfs.c:134`：`if ((int)size > max) return -1;`
- 问题：`in->size` 是不可信的盘上 uint32。`size ≥ 0x80000000` 时 `(int)size` 为负，恒不大于非负 `max`，检查被绕过；随后循环按 `size` 字节写入调用方按真实文件大小分配的缓冲区。`nblk = (size + BS - 1)/BS`（`aetherfs.c:136`）在 `size > 0xFFFFF000` 时还会回绕。`imap` 支持到双间接（约 4 GiB），可写体量远超缓冲区。
- 触发条件：伪造镜像中一个 `size` 巨大的文件 + 任意读路径。`vfs_size` 返回 `(int)in->size`（`aetherfs.c:414`）此时为负，多数"先 size 后 read"的调用方会拒绝，但 `SYS_READ_FILE` 允许用户直接指定 `max`，是最直接的触发面。
- 后果：内核堆/用户缓冲区溢出，长度攻击者可控。
- 修复建议：改用无符号比较并封顶：`if (size > (uint32_t)max || size > MAX_FILE_SZ) return -1;`，`MAX_FILE_SZ` 不超过 `imap` 可达范围；`vfs_size`/`inode_read` 对 `size > INT_MAX` 直接报错。

### S5. TLS 证书链不校验 BasicConstraints / CA 属性 —— 任何持证者可 MITM 任意域名 [已核实]

- 位置：`c/net/tls/x509.c:358-376`（`x509_verify_chain`；原文引用 366-375，已修正为函数完整范围）；`x509_parse` 全文从未解析 OID 2.5.29.19（已用 grep 确认无 basicConstraints 解析）
- 问题：链校验只做"第 i 张由第 i+1 张的公钥签名 + 顶层由内置 root 签名 + 叶子域名匹配"。攻击者用自己合法申请的 `evil.com` 终端实体证书（非 CA）作为 `certs[1]`，再用它的私钥签一张伪造的 `victim.com` 叶子作为 `certs[0]`：`signed_by_root(certs[1])` 通过（真 CA 签的）、`x509_verify_signed_by(certs[0], certs[1])` 通过（攻击者自己签的）、`name_ok(certs[0])` 通过（伪造证书写 victim.com）、`tls.c:331-354` 的 CertVerify 用伪造叶子公钥验也过。
- 触发条件：攻击者持有任一合法域名证书 + 在径（或诱导浏览器访问其服务器）。
- 后果：对任意域名的完整远程 MITM，整套 TLS 认证保证失效。
- 修复建议：解析 extensions 中的 BasicConstraints，要求链中每个非叶子证书 `cA=TRUE`（并检查 pathLenConstraint）；同时比较 `certs[i].issuer == certs[i+1].subject` 的 DER 字节（当前只验签名、不比名字）。

### S6. TLS Certificate 消息解析不以消息长度为界 —— 远程内核 OOB 读/崩溃 [已核实]

- 位置：`c/net/tls/tls.c:307-315`（原文引用 306-315，已修正）
- 问题：`cp = 1 + mb[0]`、`listlen`/`cend`/`clen`/`extl` 全部取自对端字节，唯一循环条件是 `cp + 3 <= cend && ncert < 8`，从未检查 `cend <= ml` 或 `mb+cp` 是否还在 `flight[16384]`（`tls.c:272`）内。对端发 `listlen=0xFFFFFF`、首项 `clen=0xFFFF00`：`cp += clen` 后 `mb[cp]` 在 `flight+约16MB` 处野读，几乎必然内核页 fault；即便不 fault，`x509_parse(mb+cp, clen, ...)`（312 行）会按攻击者给的长度把 DER reader 端到 `mb+cp+clen`，越过 BSS 任意读。
- 触发条件：恶意/被劫持的 TLS 服务器在握手中发畸形 Certificate；浏览器打开任一 https 页面即可触发。
- 后果：远程触发的内核 OOB 读/崩溃。
- 修复建议：解析前 `if (ml < 4) 拒绝`；`if (1 + mb[0] + 3 > ml) 拒绝`；`cend = cp + listlen; if (cend > ml) 拒绝`；循环内 `if (cp + 3 + clen + 2 > cend) 拒绝`，`extl` 同样以 `cend` 为界。

### S7. TLS flight 遍历循环缺 `q + 4 + ml <= flen` 检查 —— 尾部垃圾字节致远程 OOB 读 [已核实]

- 位置：`c/net/tls/tls.c:299-365`（对比 287-292 的扫描循环在 289 行有此检查）
- 问题：扫描循环在 Finished 处即退出，允许 Finished 后存在不完整尾部；walk 循环对尾部 4 字节重新解释 `mt/ml`：若 `mt` 恰为 EE/Certificate/CertVerify/Finished，则 303/305/355/362 行 `sha256_update(&th, flight+q, 4+ml)` 按攻击者控制的 `ml`（最大 16 MB）越界读 `flight[]` → 内核页 fault；`mt==HS_FINISHED && ml==32` 时 361 行 `memcmp(expect, mb, 32)` 在 `flen` 接近 16384 时也会越出 `flight`。
- 触发条件：对端在 Finished 后同记录附加 ≥4 字节垃圾（合法服务器不会，恶意服务器会）。
- 后果：远程内核 OOB 读/崩溃。
- 修复建议：walk 循环顶部加 `if (q + 4 + ml > flen) break;`（与 289 行一致），或直接要求 `q == flen` 恰好收尾。

### S8. `rsa_public` 未校验 `elen` —— 远端可触发的内核栈溢出 [已核实]

- 位置：`c/crypto/pubkey/rsa.c:130-137`（溢出点在 `rb_from_be`，`rsa.c:36-40`）
- 问题：`rsa_public` 只校验 `nlen`（≤ RL*4=520）和 `siglen ≤ nlen`，`elen` 完全未校验就传给 `rb_from_be(E, e, elen)`，后者执行 `o[bit/32] |= ...`，`bit` 最大为 `(elen-1)*8`。`elen` 来自 X.509 证书 RSAPublicKey 的 exponent INTEGER（`x509.c:172-174`，仅剥离前导零，无长度上限），经 `verify_with_key`（`x509.c:254-255`）和 CertVerify（`tls.c:351`）两条路径直达。
- 触发条件：恶意 TLS 服务器构造带超长指数（600 字节以上，DER 合法）的证书。
- 后果：向内核栈上的 `rbn E`（520 字节）之后写入攻击者可控的 uint32，写偏移可达数 KB——远端触发的内核栈溢出，潜在代码执行。
- 修复建议：`rsa_public` 开头加 `if (elen < 1 || elen > RL*4) return -1;`；测试钩子 `rsa_modexp_be`（`rsa.c:107-112`）的 `bl`/`el` 同样未校验，一并补上。

---

## 高危问题（23 条，全部 [已核实]）

### H-1. `ap_entry` 持 IF=1 裸调 `spin_lock(&g_bkl)` —— 定时器 IRQ 落入窗口即 BKL 自死锁 [已核实]

- 位置：`c/kernel/cpu/smp.c:174`（前置：`smp.c:170-171` 的 `sti; hlt` 使 IF=1，`smp.c:163` 已 armed 周期性 LAPIC 定时器）
- 问题：`spin_lock`（`c/kernel/cpu/spinlock.c:17-23`）在拿到 ticket 之后才写 `g_bkl_owner`，这中间 IF=1。AP 的 LAPIC 定时器 IRQ 若落入该窗口，`interrupt_handler` 读到 `g_bkl_owner != me` → 在 IRQ 里 `spin_lock_irqsave(&g_bkl)`，而 BKL 正被本核持有 → ticket 锁永不前进 → 该核死锁、BKL 永不释放 → 全系统挂起。`spinlock.c:9-14` 注释声称 "bare re-acquire sites cli around themselves"，但 `smp.c:174` 这个 bare site 前后没有 `cli`。
- 后果：全系统挂死（启动期竞态）。
- 修复建议：`smp.c:173-175` 改为 `cli` 后再 `spin_lock(&g_bkl)`（或直接用 `spin_lock_irqsave`），与注释承诺的不变量对齐。

### H-2. `file_write` 偏移整数溢出 → 野指针写 / BKL 死循环 [已核实]

- 位置：`c/kernel/exec/file.c:222`（`vfs_ensure_cap(f, f->off + len)`）+ `file.c:234-247`（`file_lseek`）+ `file.c:163-167`（`vfs_ensure_cap` 加倍循环）
- 问题：`file_lseek` 允许 `f->off` 为任意非负 long。之后 `SYS_WRITE` 时 `f->off + len` 有符号溢出回绕为负 → `vfs_ensure_cap` 中 `need <= f->cap` 直接放行 → `file.c:223` `memcpy((char*)f->backing + f->off, buf, len)` 以用户可控偏移、可控内容写内核内存（至少稳定触发内核 #PF panic）。变体：`need` 为巨大正值时 `while (ncap < need) ncap *= 2` 溢出后陷入死循环——持 BKL，整机冻结。
- 触发条件：任何 ring-3 程序 open+lseek+write。
- 后果：内核内存破坏或整机 DoS。
- 修复建议：`file_write` 开头加 `if (f->off > LONG_MAX - len) return -1;`；`vfs_ensure_cap` 加倍循环防溢出（`if (ncap > LONG_MAX/2) return -1;`）；`file_lseek` 顺带限制最大偏移。

### H-3. `SYS_PIPE` 部分失败遗留悬挂 fd → 文件槽复用后 UAF [已核实]

- 位置：`c/kernel/exec/syscall.c:245-247`
- 问题：`rfd = proc_fd_alloc(p, rf)` 成功、`wfd` 失败（fd 表只剩 1 个空槽即可构造）时，失败分支 `file_close(rf)` 使 `rf` refcount→0、`type=F_NONE`、槽位可被 `file_alloc` 复用，但 `p->fd[rfd]` 仍指向它。该槽被新文件复用后，`proc_exit`/`SYS_CLOSE` 会对新主人的文件再 `file_close` → 引用计数错乱、提前释放（UAF/双重释放）。
- 修复建议：失败路径先 `if (rfd >= 0) p->fd[rfd] = NULL;` 再 close；或先分配齐两个 fd 再安装。

### H-4. `file_close` 锁外拆毁与槽位复用竞态（潜伏，BKL-free 化即引爆） [已核实]

- 位置：`c/kernel/exec/file.c:272-301`
- 问题：槽位在锁内就已 `refcount=0`（279 行，即可被另一核 `file_alloc` 认领），但拆毁（283-299 的 vfs flush、`kfree(f->backing)`、`f->backing=0; f->type=F_NONE;`）在锁外进行。并发复用时旧主人的 `f->backing=0; f->type=F_NONE` 会覆盖新主人已初始化的字段（泄漏 backing、文件变哑），或 `kfree` 错指。当前被 BKL 串行化掩盖，但 `file.c` 注释明确这套细粒度锁是为 BKL-free 准备的——按该设计它就是不正确的。
- 修复建议：拆毁完成前不释放槽位（引入 freeing 状态，teardown 完再置 `refcount=0`/`F_NONE`），或最后引用的整个拆毁都在锁内标记、锁外只做无副作用的 kfree。

### H-5. `SYS_GUI_BLIT` 目标尺寸 dw/dh 完全未校验 → 永久系统冻结 [已核实]

- 位置：`c/kernel/gui/wm.c:551-561` + `c/kernel/gui/fb.c:552-569`
- 问题：`bl.w`/`bl.h` 来自用户态 `struct aether_blit`，只校验了 `bl.sw/bl.sh ≤ 4096`（`wm.c:556`），随后直接 `fb_blit_rgba(bl.x, bl.y, bl.w, bl.h, ...)`。`fb_blit_rgba` 按 `dw*dh` 双重循环，每次 fb_put 越界即返回——不写内存，但循环次数可达 (2³¹-1)² ≈ 4.6e18。该循环在 int 0x80 上下文执行（IF=0、持有 BKL），时钟中断无法抢占。
- 触发条件：任意 ring-3 app 发一次 SYS_GUI_BLIT 并设 `bl.w=bl.h=INT_MAX`。
- 后果：整机永久挂死。
- 修复建议：循环前将 `dw/dh` clamp 到目标 surface（`if (dx+dw > w->surf.w) dw = w->surf.w - dx;` 等），或硬上限 4096。

### H-6. NVMe 超时后完成队列永久错位，且不校验 cid —— 数据完整性 [已核实]

- 位置：`c/drivers/block/nvme.c:106-136`
- 问题：`nvme_submit` 超时（200M 次轮询）后返回 -1，但 SQ 项已提交、doorbell 已响：设备稍后完成该命令并写入 CQE。此时 CQE phase 与 `cq_phase` 匹配，下一次 `nvme_submit` 的轮询立刻看到这条陈旧 CQE 并当作新命令完成——代码完全不检查 `e->cid`（124-128 行）。从此队列错位一拍：读命令可能在设备尚未写入数据时就"完成"返回 0。
- 触发条件：SMP 争用下一次轮询超时（代码注释自承 TCG SMP 下延迟很大）。
- 后果：调用方拿到旧/垃圾数据且无法察觉（静默数据损坏）。
- 修复建议：轮询中校验 `e->cid == cid`，不匹配则跳过该 CQE 继续等；或超时后 queue reset 重新同步。

### H-7. virtio_request 同样的超时 desync，可致静默数据损坏 [已核实]

- 位置：`c/drivers/virtio/virtio.c:156-164`
- 问题：轮询只比较 `used->idx != last_used`，不校验 `e->id == head`。超时返回 -1 后迟到的完成留在 used ring；下一个请求发出后 poll 立即消费这条陈旧 entry，在该请求数据真正 DMA 完成前就返回。virtio-blk 读路径的 `status` 是共享 static，迟到的写会把它置 0，导致后续读取以"成功"返回未经设备填充的缓冲区。
- 后果：静默数据损坏（比 NVMe 更直接）。
- 修复建议：校验 `e->id`，或对超时做设备/队列 reset；blk_rw 至少交叉验证返回 len `>= count*512+1`。

### H-8. AetherFS `dir_add()` 分配尺寸整数回绕 → 堆溢出 [已核实]

- 位置：`c/fs/aetherfs.c:276-280`：`uint32_t old = d->size, cap = old + DIRENT_SZ; kmalloc(cap); inode_read(d, buf, (int)old)`
- 问题：`old` 来自盘上。`old > 0xFFFFFFBF` 时 `cap` 回绕为小值，`kmalloc` 小缓冲区，随后 `inode_read`（配合 S4 的检查绕过）把最多 `old` 字节写进去 → 堆溢出。
- 触发条件：`SYS_MKDIR`/`SYS_WRITE_FILE`/`SYS_RENAME` 作用于一个 size 被篡改的目录。
- 修复建议：`if (old > UINT32_MAX - DIRENT_SZ || old > MAX_FILE_SZ) return -1;`，并使用 S4 修复后的安全 `inode_read`。

### H-9. 超大目录 size 导致 BKL 下的长时间/无限扫描（系统级 DoS） [已核实]

- 位置：`c/fs/aetherfs.c:210-211, 228-230, 260-262`（`dir_lookup`/`dir_nth`/`dir_is_empty`）
- 问题：`nblk = (sz + BS - 1)/BS` 中 `sz` 未校验，伪造目录 size 接近 4 GiB 时，`resolve()` → `dir_lookup` 会对约 100 万个块逐个 `imap`+`bread`（每个 4 KiB 磁盘读）。FS 操作在 BKL 下执行，期间全系统停摆数分钟。`dir_count_live`（`aetherfs.c:251-256`）逐次调用 `dir_nth`，本身是 O(n²) 重扫。另 `imap` 返回 0 时循环是 `continue` 而非 `break`（213/232 行），稀疏空洞会一直扫到底。
- 修复建议：目录遍历前校验 `sz` 上限（≤ 单间接可达且为 DIRENT_SZ 整数倍）；`imap` 返回 0 时 `break`。

### H-10. 块号未校验 + ATA 28 位 LBA 静默截断 → 读写错误扇区 [已核实]

- 位置：`c/fs/aetherfs.c:64-65`（`bread`/`bwrite`：`blk * SPB`，uint32 可回绕）+ `c/drivers/block/ata.c:52`（`(lba >> 24) & 0x0F`，仅 28 位 LBA）
- 问题：inode 块号、间接表项、superblock 的 `bitmap_start`/`inode_start` 均未与 `total_blocks` 或设备容量核对。伪造镜像可让 `bread` 读任意扇区（经 `aetherfs_read` 把任意磁盘内容返回用户态），让 `flush_bitmap`/`flush_inode` 写到攻击者选定扇区；ATA 下 lba ≥ 2²⁸ 静默回绕，掩盖越界。
- 后果：磁盘破坏/信息泄露面（内存安全之外）。
- 修复建议：`bread`/`bwrite` 入口校验 `blk < sb.total_blocks`；mount 时校验 sb 各区域 start+count ≤ total_blocks 且互不重叠；blkdev 层按设备容量拒绝越界 LBA。

### H-11. 入站 TCP 段从不校验 TCP 校验和 [已核实]

- 位置：`c/net/transport/tcp.c:229-246`
- 问题：`tcp_input` 只做长度和 4 元组匹配就进入数据处理。发送端算了 `tcp_checksum` 但接收端完全不验证。链路损坏或注入的段会被当作有效数据写入 64 KiB 接收环并交给 TLS/HTTP 层（TLS 的 AEAD 能兜住加密流量，但握手前的明文 TCP 行为和 HTTP 明文直接受影响）。与 [已知 H5]（IP 头校验和缺失）同类但独立。
- 修复建议：`hlen` 校验后加伪头+段校验和验证，非零丢弃。

### H-12. [已知 C2] `tls_recv` 解密缓冲溢出 —— 远程静态缓冲区溢出，仍未修 [已核实][已知]

- 位置：`c/net/tls/tls.c:421`（`aead_open(..., s->app, ...)`；`s->app[16640]` 见 `tls.c:52`，`body[20000]` 见 `tls.c:416`）
- 问题：`blen` 可达 19995，写入 `plen = blen-16` ≤ 19979 字节进 `app[16640]`。backlog 给的修复（`if (blen - 16 > (int)sizeof s->app) return -1;`）至今未打上。
- 触发条件：对端在应用数据记录中发超长密文。
- 后果：远程触发的内核 BSS 溢出写。backlog 标记 "the one urgent" 已逾 8 周未修。
- 修复建议：按 backlog C2 方案，在 `tls.c:421` 前加长度拒绝。

### H-13. [已知 M2] CertVerify 未检 `ml>=4`、`siglen` 无界 [已核实][已知]

- 位置：`c/net/tls/tls.c:319-321`
- 问题：`mb[0..3]` 直接读、`sig = mb+4` 后按 `siglen` 使用，均未以 `ml` 为界。
- 修复建议：分支顶部 `if (ml < 4) { s->used=0; return TLS_E_PROTO; }`，读 siglen 后 `if (4 + siglen > ml) { ... }`。

### H-14. `mont_mul` 拷贝结果 off-by-one 栈越界写 4 字节（远端可触发） [已核实]

- 位置：`c/crypto/pubkey/rsa.c:81`：`rbn tt; ... for (int i = 0; i <= s; i++) tt[i] = t[i];`
- 问题：`s = rb_nwords(n)` 最大可为 `RL`（130），此时 `tt[130]` 越出 `rbn`（合法下标 0..129），向栈上多写 4 字节。证书模数 `nlen == 520` 且剥离前导零后最高字非零（合法 DER 即可构造）即可达，因为 `rsa_public` 显式放行 `nlen == RL*4`。
- 后果：RSA 验签路径栈上 4 字节部分受攻击者影响的越界写。
- 修复建议：拷贝循环改为 `i < s && i < RL`（结果只取 `t[0..s-1]`，`t[s]` 视为进位参与条件减）；或把 `RL` 加大 1，或收紧 `nlen` 上限。

### H-15. `ecdsa_verify` API 无长度参数，CertVerify 调用点读越界（远端 OOB 读） [已核实]

- 位置：`c/crypto/pubkey/ecdsa.c:330-331`（读 `pub[0..2*fl-1]`），触发点 `c/net/tls/tls.c:340-342`
- 问题：`ecdsa_verify` 按曲线固定读 `2*fl` 字节公钥，但 CertVerify 路径只检查 `chain[0].pub[0]==0x04`，没有检查 `publen >= 1+2*flen`（`x509.c:261` 的链上路径有此检查，CertVerify 路径漏了）。证书 BIT STRING 可短至 2 字节（`x509.c:162` 只查 `bl < 2`）。
- 触发条件：恶意服务器发送 P-256 但公钥仅 1 字节的证书 + ECDSA CertVerify，即越出证书缓冲区读 64 字节（P-384 为 96 字节）参与点乘。
- 后果：内核 OOB 读；读取落在 flight 缓冲区内时造成错误验签结果。
- 修复建议：`ecdsa_verify` 增加 `publen` 参数并校验 `== 2*fl`，或在 `tls.c:340` 处补 `chain[0].publen == 1 + 2*flen2` 检查。

### H-16. TTF 解析器在 16 KiB 内核栈上开高达 ~43 KiB 的 VLA —— 恶意字体溢穿内核栈 [已核实]

- 位置：`c/lib/text/ttf.c:214`（`uint8_t flags[gpts > 0 ? gpts : 1];`，在 `emit()` 内）
- 证据链：`gpts` 由字体数据控制（`ttf.c:207`），上限被 `ttf.c:212` 钳到 `capPts`；`capPts = (scratchlen - 256)/6`，唯一调用方 `c/kernel/gui/raster.c:15` 传 `OUTLINE_SCRATCH = 1<<18` → `capPts ≈ 43648`。ring-0 线程栈只有 `STACK_SIZE 16384`（`c/kernel/sched/sched.c:11`，WM/渲染跑在 ring-0 线程），ring-3 线程 kstack 也只有 32 KiB（`KSTACK_SIZE`，`sched.c:12`）。
- 触发条件：构造 simple glyph 的 endPts 使 gpts ≈ 3 万+，`emit()` 在内核栈上分配 ~43 KiB VLA → 栈越界写，踩坏相邻内核内存。字体文件可达（`fsroot/fonts` 或任何被加载的 TTF）。
- 后果：内核栈破坏 → #PF/panic，潜在控制流劫持。
- 修复建议：`flags` 改用 scratch 尾部空间（scratch 本就按 6 字节/点预留），或给 `gpts` 加硬上限（如 4096-8192）。

### H-17. libc malloc 堆初始化差一个 HDR：end sentinel 永不可达，扫描到尾块时越界读 16 字节 [已核实]

- 位置：`c/apps/libc/src/malloc.c:18`（`h->size = ARENA_SIZE - HDR`）
- 问题：首块 `next = arena + HDR + size = arena + ARENA_SIZE = aend`，而 sentinel 放在 `aend - HDR`（`malloc.c:20`）永远踩不到。堆守恒（split `malloc.c:51-56` 与 coalesce `malloc.c:44-49`）意味着任何时刻尾块的 `next` 都是 `aend`。于是 `malloc.c:35` 的 `for (…; h->size; )` 在 `h = aend` 时读取 arena 末尾之后 16 字节；`malloc.c:40` 的防御检查 `np > aend` 用的是 `>` 而非 `>=`，恰好放行 `np == aend` 这一步。
- 触发条件：每次扫描走到尾块且尾块不可用时（所有 OOM、尾块已占用），确定性 OOB 读。24 MiB arena 在 BSS 中段时无害，若 arena 恰好结束于映射边界则 ring-3 缺页 fault——与文件内注释 "an allocator must never fault" 自相矛盾。
- 修复建议：`heap_init` 改为 `h->size = ARENA_SIZE - 2 * HDR;`，同时把 `malloc.c:40` 收紧为 `np + HDR > aend`。

### H-18. AetherScript GC 标记阶段 OOM → 存活对象被误释放（UAF），代码意图与实现相反 [已核实]

- 位置：`c/apps/as/object.c:327-338`（`gc_mark_obj`）+ `object.c:411-414`（sweep）
- 问题：gray 工作list realloc 失败时执行 `o->marked = 0; return;`（335 行），注释声称 "un-mark o so sweep keeps it conservatively LIVE"，但 sweep 的逻辑是 `marked==0 → free_object(o)`。即：一个刚被证实可达的根对象被主动改回未标记，本轮 sweep 会把它（及仅经它可达的所有对象）全部释放，而 VM 值栈、帧、`g_exc` 仍指向它们 → 立即 UAF。
- 触发条件：堆压力恰好在 gray 扩容时 malloc 失败（mini-libc 下可达）。
- 修复建议：realloc 失败时保持 `o->marked = 1`（已标记即保守存活），仅设 g_oom 返回即可。一行修正。

### H-19. `.la` 字节码文件完全不做校验即执行 → 恶意文件全内存破坏 [已核实]

- 位置：`c/apps/as/as_bc.c:151-238`（`load_fn` 只验证短读/常量 tag/magic，不验证字节码语义）；`c/apps/as/vm.c:525`（`READ_CONST()` = `consts[READ_SHORT()]`，索引无界）、`vm.c:699-701`（跳转偏移使 `ip` 越出 code 缓冲区）、`vm.c:566-567`（`frame->slots[s]` 相对帧基址无声明局部数界限）
- 问题：加载器只检查文件结构完整性，不检查常量索引 < kcount、跳转目标在 [0,count)、槽位在帧窗口内。READ_CONST 越界读可伪造 `Value`（类型混淆 → 假 Obj 指针 → 任意解引用）；跳转越界可把堆数据当字节码执行。
- 触发条件：`as -run evil.la` 或脚本 `import` 一个被替换的 `NAME.la`（`.la` 优先于 `.as`）。
- 后果：ring-3 进程内任意内存读写/代码执行。
- 修复建议：as_load 后增加 verifier 遍（常量索引、跳转目标、槽位、upvalue 索引、操作数字节数）；或将 `.la` 明确声明为"仅信任自产"并文档化。

### H-20. QuickJS Element class 只在第一个 JSRuntime 上注册，第二次执行 JS 即堆越界读/写 [已核实]

- 位置：`c/apps/browser/js_dom.c:186-189` + `c/apps/browser/browser.c:62`（`run_js()` 每次加载页面都 `JS_NewRuntime()`）
- 问题：`static JSClassID elem_cid; if (!elem_cid) { JS_NewClassID(&elem_cid); JS_NewClass(rt, ...); }` —— `elem_cid` 是进程级静态变量，runtime 却每页新建。第二页起 `js_dom_init` 跳过 `JS_NewClass`，随后 `JS_SetClassProto(ctx, elem_cid, proto)`（`js_dom.c:189`）按子代理对 vendored QuickJS 的核对：首个用户 class id 恰等于新 runtime 的 `rt->class_count`（越界一位），`JS_SetClassProto`/`JS_NewObjectClass` 对 `ctx->class_proto[class_id]` 无边界检查 → 堆 OOB 写 8 字节 JSValue / 越界读垃圾 proto。注：QuickJS 内部数组边界细节引自子代理对 `third_party/quickjs/quickjs.c` 的核读，本报告独立确认了仓库侧事实（静态 cid + 每页新 runtime + 无重新注册守卫）。
- 触发条件：浏览器进程内第二个带内联 `<script>` 的页面加载。远端网页可触发（连续两次导航即可）。
- 后果：ring-3 堆越界读/写，远端页面可触发。
- 修复建议：`js_dom_init` 去掉 `if (!elem_cid)` 守卫，每个新 runtime 都执行 `JS_NewClassID + JS_NewClass`；或按 runtime 缓存注册标记。

### H-21. Element wrapper 持有裸 `struct node*`，`textContent` 赋值后旧 wrapper 全部悬垂（UAF/双重释放） [已核实]

- 位置：`c/apps/browser/js_dom.c:34-43, 53-69`
- 问题：`wrap()` 用 `JS_SetOpaque(o, n)` 直接绑活 DOM 节点，`elem_class` 无 finalizer、无失效机制。`el_set_text` → `set_text(n)` → `free_children(n)`（62 行）`dom_free` 掉全部子树；而脚本此前拿到的指向这些子节点的 wrapper 仍存活。之后对该 wrapper 的任何访问——`el_get_text` 读已释放内存、`el_setattr` → `set_attr` 对已释放节点 `free(n->attrs)` → UAF 写/双重释放。
- 触发条件：4 行 JS 即可（`var e=document.getElementById("x"); document.body.textContent="hi"; e.setAttribute("a","b")`），远在 2048 字节脚本上限内，远端页面可控。
- 修复建议：节点加代数/存活标记（wrapper 存 {node, generation}，访问前校验），或 class finalizer + 节点端反向注册表在 `dom_free` 时把所有 wrapper 的 opaque 置 NULL。

### H-22. Terminal Tab 处理在 `ccol == COLS-1` 时死循环，终端永久卡死 [已核实]

- 位置：`c/apps/gui/terminal.c:51`：`if (c == '\t') { do { if (ccol < COLS - 1) scr[crow][ccol++] = ' '; } while (ccol % 4); return; }`
- 问题：`ccol` 达 55（COLS=56）时 `ccol < COLS-1` 恒假，循环体不再推进 `ccol`，而 `55 % 4 == 3 ≠ 0`，do-while 变成无 yield 无限循环。`ccol` 可达 55：第 54 列插入字符后 `ccol=55`（`feed` 只在插入前检查 `ccol >= COLS-1`，`terminal.c:52`）。
- 触发条件：shell 输出一行满 55 列后紧跟一个 `\t`（`printf` 即可构造），Terminal busy-loop 挂死且不再响应 EV_CLOSE。
- 修复建议：`while` 条件加 `ccol < COLS-1 && ccol % 4`，或 `ccol` 到界时直接 `nl()`。

### H-23. 文件管理器多选删除时目录索引漂移，删错文件 [已核实]

- 位置：`c/apps/gui/files.c:197-205`（`do_delete`）
- 问题：`sel[]` 存的是删除前的行索引，但循环中每删一项，后续条目在目录枚举里整体前移一位，后续 `row_path(sel[i])` 取到的已是另一个文件。例：选中相邻两行 {2,3}，删 2 后原第 4 项变成 3，于是删掉的是原第 4 项、原第 3 项残留。
- 后果：误删用户文件（数据损坏类）。
- 修复建议：先把所有待删路径收集到数组（删除不改变字符串路径），再统一执行 `delete_tree`。

### H-24. `Makefile:37` — `find C include -type d` 大小写错误，Linux 上整个构建直接崩 [已核实]

- 位置：`Makefile:37`
- 问题：目录实际是小写 `c/`，这里写的是大写 `C`。macOS/Windows 大小写不敏感能跑；在大小敏感文件系统上 GNU find 对 `C` 报错、只返回 `include` 的结果，`INCDIRS` 只剩 `-Iinclude/abi`，而项目头文件全部 colocate 在 `c/` 各目录 → 所有内核 `.c` 编译失败。`$(shell ...)` 吞掉非零退出码，报错表现为满屏 "header not found"，极难定位。佐证：`tools/gen_compile_commands.py:18` 用的是小写 `find("c", "include", ...)`，两处自相矛盾。
- 修复建议：`find c include -type d`。（agent-12 与 agent-14 重复报告，已合并。）

### H-25. Rust inflate stored 块字节对齐错误丢弃整个已预取字节（移植引入的回归） [已核实]

- 位置：`rust/src/inflate.rs:155`：`b.nbits = 0; b.bits = 0;`（对齐时丢弃整个位缓冲）
- 问题：`huff_decode`（`inflate.rs:90-91`）每次解码前 `refill(MAXLEN-1)` 激进预取，符号解码后缓冲中最多可剩约 21 位（含最多 2 个完整字节），`b.pos` 已越过这些字节。stored 块对齐时把整个位缓冲清零而不回退 `b.pos` → LEN/NLEN 从错误偏移读取，stored 数据错位复制。
- 触发条件：任何"Huffman 块后接 stored 块"的合法 DEFLATE 流（`Z_SYNC_FLUSH`、含不可压缩尾段的流）。PNG IDAT 可远程（浏览器）触达。后果是合法图片解码失败/花屏；边界检查仍兜住，无内存不安全。原 puff 式 C 按需取位（余量 <8 位）无此问题，系移植时引入。
- 修复建议：对齐时只丢弃不足一字节的零头，回退整字节：`b.pos -= (b.nbits / 8) as usize; b.nbits = 0; b.bits = 0;`（或 `huff_decode` 改按需 refill）。

---

## 中/低危问题（按模块分节）

### 内核核心（core / cpu / mm）

**中**
- `c/kernel/cpu/smp.c:213-226` — 超过 `PERCPU_MAXCPU`(8) 的 CPU 仍被启动并计入 `g_online`：`percpu_register_id` 只在 `g_online < 8` 时注册（219 行）但 AP 照样 SIPI，找不到 slot 的 AP 在 park 分支仍 `g_online++`（147 行）。`smp_cpu_count()` 高估；`tlb_flush_all` 按 `g_online` 遍历 `g_cpus[8]` 会越界读（该函数目前无调用方，属待发引信）。修：SIPI 循环加 `if (g_online >= PERCPU_MAXCPU) break;`；park 分支不增加 `g_online`。
- `c/kernel/mm/vmm.c:21-34` — `next_table` 不检查大页 PS 位：`boot.asm` 用 2 MiB 大页恒等映射 0–1 GiB，若 `vmm_map_page` 被用于 <1 GiB 地址，PD 项（PS=1）会被当 PT 指针，页表项直接写进该 2 MiB 物理页。当前调用方均 ≥1 GiB（潜在炸弹，LAPIC/IOAPIC 基址来自 ACPI、无防线保证 ≥1 GiB）。修：`next_table` 检测 PS 位拒绝/拆分；`vmm_map_page` 入口断言 `virt >= 0x40000000`。
- `c/kernel/core/rng.c:84-89` — RNG 熵源静默退化为 rdtsc：`rdseed`/`rdrand` 缺失或单次失败即退回 `rdtsc() ^ timer_ticks`，无告警；无 RDRAND 的 CPU 上 TLS 密钥材料可被远端预测。另 `rng_state` 等全局无锁（当前均在 BKL 下，潜在竞态）。修：rdseed 加重试、无硬件熵源时 kprintf 告警、加 irqsave 锁。

**低**
- `c/kernel/mm/kheap.c:23,64-65,104-106` — `kmalloc`/`grow` 缺尺寸溢出防护：`ALIGN16(SIZE_MAX-3)` 回绕为 0 冒充成功；`frames*FRAME_SIZE` 在 `need ≥ 2^63` 时回绕为 0 → 持锁死循环。调用方均为内核受信尺寸。
- `c/kernel/cpu/acpi.c:94,108,116` — ACPI 表解析信任假设：MADT type-5 的 64 位 LAPIC 地址截断为 uint32；`xsdt->length`/`rsdt->length` 未校验 `< sizeof(header)` 即相减，无符号回绕 → 越界遍历。固件受信路径。
- `c/kernel/cpu/lapic.c:42` — `ipi_wait` 无超时：delivery-status 卡住即永久自旋（持 BKL/IF=0 时拖死全机）。与 [已知 M20] 同类的新位置。
- `c/kernel/cpu/ioapic.c:33-42` — `ioapic_route` 不校验 GSI 上限（只查 `gsi < gsi_base`），超出重定向表写越界 MMIO。
- `c/kernel/mm/pmm.c:90,96` — `pmm_init` 信任 MB2 tag 尺寸：`tag->size==0` 或 `entry_size==0` 时 `p` 不前进而死循环（固件受信）。另 `pmm_alloc_contig`（202-217）逐位 O(total_frames) 扫描且持锁 IF=0，大内存机器上中断延迟可观（性能）。
- `c/kernel/cpu/smp.c:226` — [已知 H11] AP 启动失败泄漏内核栈（缺 `kfree(stk)`）。

### 调度与进程（sched / exec / syscall）

**中**
- `c/kernel/sched/sched.c:102-103,49,69-70` — `thread_create`/`sched_init` 的 kmalloc 未检查返回值：`thread_create_user`/`thread_fork` 都检查了，`thread_create` 反而两次未查 → OOM 时 NULL 解引用。
- `c/kernel/gui/wm.c:176,213-214` — `wm_launch` 失败路径资源泄漏：`vmm_new_space()` 失败时直接 return（`img` 泄漏）；app 槽位耗尽（`ai < 0`）时 `kfree(img)` 后返回但漏 `vmm_free_space(space)`——每次泄漏 PML4/PDPT + 全部用户页帧（4-8 MiB），16 个窗口槽满时反复 launch 可耗尽 PMM。（agent-1 与 agent-2 重复报告，已合并。）
- `c/kernel/exec/exec.c:164-165`、`c/kernel/gui/wm.c:233` — `thread_create_user` 失败不检查：OOM 返回 -1 无人检查，proc 永远 `PROC_RUNNING` 但没有线程（NPROC 槽 + cr3 空间永久泄漏）；`proc_spawn` 还照常 `return p->pid`。

**低**
- `c/kernel/exec/exec.c:50-51`、`c/kernel/gui/wm.c:195` — 用户栈顶地址无界限：`top = (entry & ~0xFFFFF) + 0x4000000`，entry 未校验可越出 PDPT[1]（与 S2 同源，修了 ELF 校验即收敛）。
- `c/kernel/exec/syscall.c:208-214` — `SYS_FILE_NAME` 缺索引上界检查（对照 `SYS_DIR_NAME` 199 行检查了 `i >= vfs_count(abs)`），仅靠 `dir_nth` 兜底，行为不一致。
- `c/kernel/mm/vmm.c:210` — `vmm_user_range_ok` 对 `len==0, ptr==NULL` 返回 0：`write(fd, NULL, 0)` 返回 -1 而非 0（POSIX 兼容性）。
- `c/kernel/exec/exec.c:102`、`c/kernel/gui/wm.c:150` — `sz+511` int 溢出：文件接近 2 GiB 时 `bytes` 变负 → 巨分配失败（健壮性）。

### 图形与窗口系统 / PCI（gui / pci）

**中**
- `c/kernel/gui/wm.c:361-367` — SYS_GUI_RECT 16 位宽高未 clamp：`rw/rh` 掩码到 0..65535 后直接进 `fb_fill_rect`，最坏 65535² ≈ 4.3e9 次 fb_put 空调用，IF=0+BKL 持有期间执行，单次冻结数秒、可重复。修：rect 与 surface 求交后再循环。
- `c/kernel/gui/wm.c:515,526-531` + `c/kernel/gui/raster.c:136-146` — 用户控制的 px 无上限：`SYS_TEXT_MEASURE`/`SYS_GUI_TEXT_RUN` 把任意 int32 字号传入 `text_raster`；`raster.c:146` 的 `w*h` int 乘法可有符号溢出回绕绕过 covcap 检查，随后 `fill_edges` 越过 `text.c:51` 的静态 `rastbuf[200*200]` OOB 写内核 BSS（回绕窗口窄，可利用性牵强但缺口真实）。修：syscall 入口将 px 限到 1..512。
- `c/lib/text/ttf.c:214` — 见 H-16（agent-2 亦独立报告，已合并）。
- `c/kernel/gui/wm.c:1015,1042` — 拖拽中的窗口被 reap 后 `dragging` 悬索引：槽位复用后鼠标事件会把无关新窗口拖走（无内存破坏，状态管理缺陷）。修：`reap()` 置 `dragging = -1` 或拖拽前查 `used`。

**低**
- `c/kernel/gui/wm.c:1067` → `c/apps/coreutils/.../aex.c:20-28` — `aex_info` 无 size 参数直接读 64 字节头：一个 <64 字节的 "x.aex" 导致最多 60 字节堆越界读（启动期 scan_apps 路径）。
- `c/kernel/gui/text.c:120-130` — `text_measure`/`measure` 缺 font_ok 守卫：字体加载失败时 `glyph_get` 除零 #DE panic（打包缺陷触发）。
- `c/kernel/gui/wm.c:1091-1093` — `wm_init` 的 `pmm_alloc_contig` 返回值未检查：OOM 时 `fb_set_backbuffer(NULL)` 后 wm_render NULL 解引用（与 [已知 H9] 同类）。
- `c/kernel/gui/fb.c:79-88` — `fb_init` Multiboot2 tag 遍历无 size==0 守卫 → 死循环（GRUB 受信）。
- `c/kernel/pci/pci.c:37-56` — `pci_find` 只扫 bus 0 / func 0：不遍历 secondary bus、不查 multifunction、未区分 32/64 位 BAR 与 I/O BAR；CF8/CFC 无锁（启动期尚无竞态）。
- `c/kernel/gui/wm.c:452-471` — SYS_WRITE_FILE/DELETE/MKDIR 无权限模型（设计备忘）：任何 ring-3 app 可覆写 `/` 下任意文件，"app 沙箱"仅及于内存不及于磁盘，值得文档明示。
- `c/kernel/gui/wm.c:484-512` — SYS_HTTP_GET/BODY 全局状态无归属：`g_net_busy` 仅在单次调用内置位，两个 app 的 HTTP 调用可交错，B 读到 A 的 body（数据混淆，非内存问题）。

### 设备驱动（drivers）

**中**
- `c/drivers/net/e1000.c:196-198` — TX 描述符自旋读的 DMA 环形内存未声明 volatile（由高级降级，见文末「已剔除的误报/降级」）：`tx_ring` 是普通指针（`e1000.c:91`），`status` 由 NIC DMA 写入，编译器有权把加载提升出循环。原报告称"永久自旋"不准确：`++spins > 1000000` 的兜底即使加载被提升也仍然生效，真实后果是条件恒假时每TX等待都空转 1M 次后误报 -1（功能受损+短暂卡顿），不是死锁。修：`status` 字段加 volatile 或循环内加编译屏障。
- `c/drivers/net/e1000.c:213-215` — RX 描述符 length 未做上限检查：length 来自设备可达 0xFFFF，缓冲只有 4 KiB 帧，上层信任该 len 会越界读到帧外；`RXD_STA_EOP`/errors 位也不检查。host 可控（VM 内 host 通常可信，定中）。（agent-3 与 agent-5 重复报告，已合并。）
- `c/drivers/block/nvme.c:181` — `g_max_sectors` 移位 UB/溢出：`mdts ≥ 32` 时 `1u << mdts` 是 UB；28–31 时乘 8 溢出回绕，可致 `n` 被 clamp 到 0、`cdw12 = 0xFFFFFFFF`、死循环。修：`if (mdts > 7) mdts = 7;`。

**低**
- `c/drivers/block/ata.c:52-58,63-68` — `count==0` 使控制器状态机卡死：ATA 语义 SECCOUNT=0 表示 256 扇区，传输循环按 C 语义执行 0 次（当前调用方不会传 0，潜伏）。
- `c/drivers/block/nvme.c:152-227`、`c/drivers/virtio/virtio_gpu.c:84-101`、`c/drivers/virtio/virtio.c:110-113` — init 失败路径泄漏 PMM 帧（一次性，init 期）。
- `c/drivers/block/nvme.c:74-82`、`c/drivers/virtio/virtio.c:42-50,66-68` — BAR 映射未验证有效性：不查 base==0/I/O 位，固定映射 0x8000 字节，virtio 能力结构 offset 未校验。
- `c/drivers/block/nvme.c:149,159,189` — init 硬件健壮性缺口：清 EN 后等待 RDY 不检查结果；AQ depth 固定 64 未读 CAP.MQES；Active NS List 失败静默回退 nsid=1。
- `c/drivers/timer/pit.c:13`、`c/drivers/char/serial.c:25`、`c/drivers/timer/rtc.c:26,36` — 无界等待与参数校验：hz==0 除零；serial TX ready 无超时；RTC update-in-progress 无超时。
- `c/drivers/net/e1000.c:190-206` — TX 依赖"调用方必持 net_lock"的隐式约定：已核实当前所有路径确实都在 IF=0 段内（今天不是 bug），建议头文件写明约定或加断言。
- `c/drivers/net/e1000.c:104,108,123,127` — [已知 H9] `rx_init/tx_init` 的 `pmm_alloc` 返回值未检查。
- `c/drivers/virtio/virtio.c:81` — [已知 M20] virtio reset 等待无超时。

### 文件系统（fs）补充中/低

**中**
- `c/fs/aetherfs.c:58-62`（触发点 217/309）— `streq()` 对非 NUL 终止的盘上目录名越界读：`de[j].name` 是 60 字节定长字段，伪造镜像可不写 NUL → 一直读到 `blk_buf` 之外。修：`strncmp` 限定 60 字节。
- `c/fs/aetherfs.c:582` — `aetherfs_rename` 回滚不完整：回滚也失败（IO 错误）时同一 inode 链接在两个目录项下，之后删除任一路径会释放该 inode，另一目录项指向已释放 inode（UAF-on-disk，无 fsck 则永久损坏）。
- `c/fs/aetherfs.c:354-371,426-473` — `resolve_parent` 允许 leaf 为 "." / ".."：现有防线碰巧都靠上游规范化挡住，aetherfs 自身没有任何 leaf 合法性检查；内核内部调用方传未规范化路径即可在盘上创建名为 `..` 的目录项。

**低**
- `c/fs/aetherfs.c:399-405` — mount 校验不完整 + 错误路径泄漏：任一 `bread` 失败直接 return（`bitmap`/`inodes` 泄漏）；无防重入；`root_ino` 未校验。
- `c/fs/aetherfs.c:200-204` — `inode_write` 失败路径遗留悬空 direct 指针（trunc 后 nblk==0 不再清 direct[]；bitmap 一致，不会双重释放，属悬空状态）。
- `c/fs/aetherfs.c:333-335` — 超长路径分量被静默截断后参与查找：70 字符名的查找可能命中 59 字符前缀的同名文件。修：分量超限直接返回 NOINO。
- `c/fs/aetherfs.c:452` — mkdir 里 `flush_inode` 失败时新 inode 保持已分配但无目录项 → inode 泄漏（与 dir_add 失败分支的正确清理不一致）。
- `c/fs/aetherfs.c:414,521` — `aetherfs_size`/`aetherfs_ent_size` 把 uint32 size 截断为 int；且不区分目录与文件，`file_open_vfs` 能把目录"打开"为空文件。
- `c/fs/aetherfs.c:243` — `dir_nth` 可能返回 ino=0 的伪造目录项，`dir_count_live` 遇 0 提前停止 → 枚举计数错误（仅伪造镜像）。
- 整个 FS 依赖 BKL 串行化（`blk_buf`/`ind_buf`/`namebuf` 等共享静态缓冲无自旋锁），正确性系于"BKL 不在 FS 操作中途释放"的约定，值得在文件头注释明示。

### 网络栈下层与传输层（net/link + ip + transport）

**中**
- `c/net/transport/tcp.c:246` — RST 无任何序列号校验即销毁连接：只要猜中/看见 4 元组（slirp 网关在径上），一个 RST 包即可杀死任意连接。RFC 793 要求 RST seq 落在接收窗口内。
- `c/net/transport/tcp.c:266` — 清重传状态的 ACK 条件接受"确认了未发送数据"的非法 ACK：`snd_una` 更新（264 行）有上限保护，清重传槽这条没有；对端发 `seg_ack > snd_nxt` 会让我们丢掉未被真正确认的段 → 字节流出现空洞。修：改为 `seg_ack == snd_nxt`。
- （e1000 RX length 见驱动节，已合并；e1000 volatile 降级项见驱动节。）

**低**
- `c/net/transport/tcp.c:299` — FIN_WAIT 兜底超时杀半关闭连接：发 FIN 后 2 秒强毁，即使对端仍在合法流式发送（偶发响应截断）。
- `c/net/transport/tcp.c` 整体 — 从不记录对端通告窗口，`tcp_send` 恒按 1460 字节发送，丢包环境吞吐/延迟变差（非安全）。
- `c/net/link/arp.c:92-96` — `arp_warm` 每个循环迭代发一次 ARP 请求，无退避（局域网噪声）。
- `c/net/transport/tcp.c:332` — `tcp_connect` 超时路径 `c->used = 0` 在 net_lock 之外（极小的撕裂窗口，与 tcp.c:311 的约定不一致）。
- `c/net/ip/ip.c:71-96` — `ip_input` 不检查目的地址：叠加 `udp.c:61` 单接收槽不校验源端口，任何同网段主机可向 DNS 槽投毒（`dns.c:54` 事务 ID 固定 0x1234）。建议过滤 dst + DNS 随机 txid。
- `c/drivers/net/e1000.c:211-221` — `e1000_rx_poll` 在 net_lock（IF=0）内完成整包协议处理，极端 TX 环满时全系统卡顿（延迟设计问题）。
- `c/net/ip/ip.c` — [已知 H5] 入站 IP 包不校验头部校验和。

### 网络应用层与安全层（dns / http / tls）

**中**
- `c/net/tls/tls.c:442,445-447` — [已知 M3] `tls_der_sig_to_rs` 缺 `p<end` 检查：长度字节、`il`、`p += il` 及 0 剥离循环均可越 `end` 读。
- `c/net/tls/tls.c:340-342` — 见 H-15（agent-6 亦独立报告，已合并）。
- `c/net/tls/tls.c:256` — X25519 共享密钥未做全零检查：对端发低阶点（如全 0 公钥）时 `shared` 为全 0，握手密钥变成已知值。证书认证能兜底（攻击者无私钥过不了 CertVerify），但违反 RFC 8446/7748 的 contributory 要求。修：`x25519()` 后检查 `shared` 全零则 abort。
- `c/net/tls/x509.c:72` — UTCTime 年份解析错误：`YY` 一律映射为 2000+YY，而 RFC 5280 规定 `YY>=50` 属 19xx。`notAfter=991231`（1999 过期）的证书被解析成 2099 → 有效期检查通过，配合被盗老证书私钥可复活过期证书。修：`year = two(p) >= 50 ? 1900+yy : 2000+yy`。另 `parse_time` 不校验数字字符与结尾 'Z'。
- `c/net/tls/tls.c:104-119,396-402` — `aead_seal`/`tls_send` 无长度检查 + off-by-one：`plain[16384]`（107 行）在 `len==16384` 时 `plain[clen]` 恰好越界写 1 字节，`len` 更大则 memcpy 溢出；`rec[16400]` 同样。当前唯一调用方 `http.c` 的 req ≤704 字节打不到（公共 API 上的定时炸弹）。修：`tls_send` 入口 `if (len < 0 || len > 16383) return -1;`。

**低**
- `c/net/http/http.c:152` — [已知 H2] Content-Length 解析 int 溢出。
- `c/net/http/url.c:37` — [已知 H3] URL 端口解析 int 溢出。
- `c/net/tls/tls.c:297,327` — CertVerify 先于 Certificate 时 `th_cert` 未初始化（栈垃圾构造 signed_data；结果只是验签失败，fail-closed，属未初始化读）。
- `c/net/tls/tls.c:176,189-191` — SNI 主机名长度无界（公共 API 视角）：`hl > ~306` 即溢出 `ch[512]`；当前 http 层 host ≤127 打不到。
- `c/net/http/http.c:61-62` + `c/drivers/timer/rtc.c:53` — RTC 月份未校验：`md[m-1]` 可越出 `md[12]`（本地/仿真器 CMOS 异常）。
- `c/net/tls/tls.c:431-435` — `tls_close` 与 `tls.h:23` 注释契约不符：不发 close_notify 只清 `used`（文档/行为需对齐）。
- `c/net/http/http.c:273` — `res_fetch` 接受 status code 为 0 的响应：状态行解析失败的非 HTTP 响应直接进入图片解码器。建议 `code != 200` 一律拒绝。
- `c/net/tls/tls.c:260-261` — `sha256(zeros,0,...)` 与 `sha256("",0,...)` 重复调用，前者死代码。

### 自研密码学（crypto）

**中**
- `c/crypto/pubkey/x25519.c:118-148` — 见 TLS 节 X25519 全零检查项（agent-7 亦独立报告，已合并）。
- `c/crypto/pubkey/ecdsa.c:330-335` — ECDSA 未校验公钥点合法性（在曲线上、坐标 < p、非无穷远）：`qx/qy` 直接 `bn_from_be` 后进入 `jpt_mul`；Barrett 路径以输入 < m 为前提，攻击者证书可给出 `qx,qy ≥ p` 使中间结果不完全约减。后果几乎总是误拒（DoS），属已知类别的实现缺陷。

**低**
- `c/crypto/hash/hmac_hkdf.c:49-51,64-71` — `hkdf_expand` 的 `info` 无长度上限：栈缓冲 `in[48+256+1]`，`infolen > 256` 即溢出（当前唯一调用方安全）；另未执行 RFC 5869 `outlen ≤ 255*hlen` 上限。
- `c/crypto/hash/hmac_hkdf.c:20-21`、`c/crypto/aead/chacha20poly1305.c:132-135`、`c/crypto/aead/aesgcm.c:88-96` — 长度参数为有符号 `int`，负值导致 OOB（当前 TLS 调用方保证非负，纵深防御缺失）。
- `c/crypto/hash/hmac_hkdf.c:7-12`、`c/crypto/pubkey/rsa.c:144-149` — `hmac`/`hash_sel` 对非法 `hlen` 静默回退：`hmac(64,...)` 实际只写 48 字节；`hkdf_extract` 的 `memset(zero, 0, hlen)` 在 `hlen > 48` 时越界写 `zero[48]`（现调用方只用 32/48）。
- `c/crypto/pubkey/ecdsa.c:77-98` — `curves_init` 在 SMP 下非线程安全：两个 CPU 并发首次调用会交错写全局曲线参数/Barrett 表（后果限于错误验签结果）。修：启动早期单线程调一次。
- `c/crypto/pubkey/rsa.c:130-142` — `rsa_public` 未校验模数为奇数（对比 `rsa_modexp_be` 有检查）：偶数模数证书 → 验签失败，仅 DoS。
- `tools/genroots.py:10` — 输出路径指向已不存在的 `src/` 目录（agent-7 报低、agent-12 报中，已合并为中，见构建节）。
- `c/crypto/pubkey/rsa.c:186-190` — RSA-PSS 接受任意 salt 长度：TLS 1.3 要求 CertVerify salt 长度等于哈希长度（宽松接受不直接可利用，与协议不符）。

### 共享库与用户态 libc（lib / apps/libc）

**中**
- `c/apps/libc/src/stdio.c:87-92` — `printf("%f/%e/%g", ±Inf)` 死循环挂死进程：`inf - 1` 仍是 Inf，比较恒假，Inf 漏过检测；`while (t >= 10.0) t /= 10` 永不退出。任何用户态程序对无穷大做浮点格式化即 100% CPU。修：`if (v > DBL_MAX || v != v)` 在指数归一化前返回 "inf"。
- `c/apps/libc/src/stdio.c:117` — `%f` 对 v ≥ 2⁶⁴ 的 double 做 `(unsigned long long)v` 强转 —— UB（x86-64 上 cvttsd2usi 产生不定值）。`printf("%f", 1e30)` 可踩。
- `c/lib/image/jpeg.c:245,303-320` — JPEG DC 预测子无界累积，IDCT 定点乘法可溢出 int64：`jpeg.c:242` 允许 DC 类别到 16（baseline 合法上限 11），`|dcpred| ≤ 6.9e10` × `qt=65535` → `blk[0] ≈ 4.5e15`，IDCT 中 `(z2+z3)*25172 ≈ 2.3e20 > 2⁶³` → 有符号溢出 UB。`jpeg.c:234-237` 注释声称 64 位 "well clear of overflow"，对对抗性输入不成立（触发条件苛刻：约 3 MB 熵数据 + ~350 MB 内核堆）。修：`t > 11` 拒绝。
- `c/apps/libc/src/stdlib.c:107` — [已知 M23] `strtod` 指数部分 `int e` 无界累积：`"1e9999999999"` → int 溢出 UB。注：子代理复核发现 `strtoll` 已有 cutoff 防护（疑似已修、backlog 未更新），`strtod` 仍未修。

**低**
- `c/lib/image/gif.c:37-39` — GIF LZW：`clear` 后首码等于 `next` 且 `prev < 0`（非法流）未拒绝，读陈旧字典项 → 错误像素。
- `c/lib/image/gif.c:80` — GIF 交错位被忽略：交错 GIF 按顺序行渲染 → 图像错乱。
- `c/lib/text/utf8.c:15-25` — UTF-8 解码接受 overlong/代理区/超 U+10FFFF 码点：`C0 80` 解码为 cp=0，与 `utf8.h:8` "cp==0 表示停止" 约定冲突，可让调用方提前截断字符串。
- `c/apps/libc/src/malloc.c:66-78` — `free()`/`malloc_usable_size()` 无 arena 范围校验：`free(野指针)` 在 `p-16` 处写 8 字节 0（调用者错误，但与文件自己的防御理念不一致）。
- `c/apps/libc/src/stdio.c:220,249` — `fread`/`fwrite` 的 `sz * n` 溢出未检查（glibc 早年同类 CVE 模式）。
- `c/apps/libc/src/stdio.c:145,148`、`c/apps/libc/src/scanf.c:39` — 格式串 width/prec 数字解析有符号溢出 UB：`"%99999999999d"`；`width = -width` 对 INT_MIN 也是 UB。
- `c/apps/libc/src/stdlib.c:110` — `strtod` 无转换时 endptr 语义错误：应指回原始 `s`。
- `c/lib/image/jpeg.c:243,257` — JPEG 熵数据中途 EOF 被静默吞掉：`br_recv` 返回 -1 未检查 → 截断 JPEG 解出垃圾尾部。
- `c/apps/libc/src/stdio.c:310-316` — `fclose(stdin)` 泄漏已分配的读缓冲（一次性小泄漏）。
- `c/lib/image/jpeg.c:388` — JPEG 段长上限 `len > 0x7fff` 过严：合法上限是 65533，大 EXIF/ICC 的合规文件被误拒。
- `c/apps/libc/src/stdlib.c:18-20` — `abs/labs/llabs(INT_MIN)` 返回负值（经典边界，记录在案）。

### AetherScript 语言实现（apps/as）

**中**
- `c/apps/as/object.c:176-181,150-154` — `as_list_push`/`as_module_slot` 扩容失败的 realloc 惯用法：realloc 失败返回 NULL 时旧缓冲区泄漏，且 `items`/`vars` 被覆写为 NULL 而 `count` 保持 >0 → 脚本 try/except 捕获 OOM 后继续访问即 NULL 解引用。修：临时变量接收，失败保留旧指针。
- `c/apps/as/object.c:81-101,104-142,160-172,216-220,292-296` — 对象构造函数普遍不检查 `alloc_obj` 的 NULL 返回：OOM 即 NULL 解引用，使整套 g_oom 管道在这些路径上完全失效。
- `c/apps/as/compiler.c:1184,1192` — 单循环内 break/continue 超过 64 个时静默错编译：第 65 个 break 的 `pop_locals_to()` 已发射 OP_POP 但跳转不发射也不报错 → 值栈失衡，sp 可持续下溢至 `stack[]` 基址之下 → 越界读写。修：`else error("too many breaks/continues")`。
- `c/apps/as/compiler.c:645,669-670` — 表达式嵌套 >64 层时三元运算符错编译：rewind 失效，值栈污染（逻辑破坏为主）。修：深度超限报错。
- `c/apps/as/value.c:84-89,159-166` — 循环引用容器打印/str() 无限递归 → C 栈耗尽崩溃：`l = []; l.append(l); print(l)` 一行触发（Python 有 `[...]` 环保护）。修：传深度计数。
- `c/apps/as/as_bc.c:226` — `as_load`/`load_fn` 对 K_FN 常量递归无深度限制：数百 KB 的 `.la` 即可造成数万层 C 递归 → 栈溢出。
- `c/apps/as/vm.c:352-353` — `native_range`：`i += step` 有符号溢出 UB：`range(INT64_MAX-1, INT64_MAX, 2)` 第二次迭代溢出回绕成负 → 超长循环直到 OOM。修：循环前守卫 `i > INT64_MAX - step`。
- `c/apps/as/vm.c:672` — `op_POW` 浮点路径：指数 INT64_MIN 取负 UB（`2.0 ** (-9223372036854775808)`）。backlog H17/H18 的同族遗漏点。修：取负前特判 `e == INT64_MIN`。
- `c/apps/as/as_native.c:70-75` — `mem2str` 长度 int64→int 截断：`mem2str(p, 4294967295)` 截断为 -1 → `memcpy(buf, p, SIZE_MAX)` 立即崩溃（脚本本就有 peek/poke，按健壮性缺陷计）。

**低**
- `c/apps/as/vm.c:177,205`、`c/apps/as/as.c:17` — realloc 失败路径泄漏旧缓冲区（仅 OOM 时泄漏）。
- `c/apps/as/as.c:82-84` — `as -run` 二次 fopen 不检查返回值 + TOCTOU：两次读取长度可不一致，文件变大则越界读 src 缓冲区。
- `c/apps/as/as.c:91-98` — 模块 stamping 超过 256 个嵌套 fn 时静默跳过：部分 fn `->module` 保持 NULL → `OP_DEF_GLOBAL`/`OP_GET_GLOBAL` 解引用 NULL module 崩溃。
- `c/apps/as/complete.c:141` — `ctx_at` 行首关键字探测越界读：`c_neq(src+s, "import", 6)` 不检查 `s+6 <= len`（最多越界 5 字节）。
- `c/apps/as/compiler.c:238,245-326` — 编译器递归无深度限制：数万层嵌套括号/f-string → C 栈溢出（本地脚本 DoS）。
- `c/apps/as/vm.c:130` — `str_concat` 长度 int 溢出（理论）：需先物化 ~2GB 字符串，基本不可达。
- [已知 H17/H18]：`vm.c:612,620`（op_DIV/op_MOD 无 INT64_MIN/-1 检查）、`vm.c:681`（op_NEG 无 INT64_MIN 检查）在当前 HEAD 仍未修复（子代理复核确认）。

### 浏览器引擎（apps/browser）

**中**
- `c/apps/browser/browser.c:108-121,78` — `collect_scripts` 不写 NUL 终止符：`scr` 是 static 数组，第二次收集的脚本若比上次短，`[sn, 上次sn)` 区间残留上一页面的脚本字节，`JS_Eval(ctx, src, strlen(src), ...)` 把残留尾部一并编译执行——跨页面脚本污染/语法错乱。修：`int sn = collect_scripts(...); scr[sn] = 0;`。
- `c/apps/browser/css_vars.c:57,93` — `var_subst` 对 fallback 递归调用深度无上限：嵌套 `var(--x, var(--x, ...))` 每层仅 ~8 字节输入，1.25 MiB 样式表可造 ~15 万层递归 → 耗尽 ring-3 栈，确定性崩溃。修：递归改迭代或限深 32。
- `c/apps/browser/dom.c:93-95,150-151,193` — `newnode()` 返回值三处未检查，kmalloc 失败即 NULL 解引用（同函数 148 行的 `tb` 反而检查了，风格不统一）。远端大页面耗尽 app 堆时可触发。
- `c/apps/browser/css_engine.c:64-73` — [已知 M11] `h_node_classes` 每次 `css_select_style` 泄漏 malloc 的 classes 数组（复核确认仍未修）。

**低**
- `c/apps/browser/css_engine.c:447-449` + `layout.c` — `font_px` 只钳下界不钳上界：巨型 CSS 长度在布局坐标累加中造成 int 有符号溢出（渲染错乱/UB）；PCT margin/padding 把百分比原始值当 px 用。
- `c/apps/browser/layout.c:28,118` — `atoi_` 无溢出保护、`ih = ih*s2/iw` 乘法可溢出（渲染异常）。
- `c/apps/browser/dom.c:64-78` — 数字字符引用未按 0x10FFFF 封顶：`&#99999999;` 产生非法 UTF-8 字节序列；`&#xZZ;` 静默产出 NUL 截断文本。
- `c/apps/browser/dom.c:166-171` — 属性解析两处容忍度缺陷：引号内 `>` 提前结束标签扫描；未加引号的值不遇 `/` 停。
- `c/apps/browser/layout.c:306-317` — `layout_free()` 不重置 `page_has_bg`：加载失败清空页面后仍用上一页背景填充视口。
- `c/apps/browser/js_dom.c:39-42,198` — `wrap()` 失败时把异常 JSValue 直接交给 `JS_SetPropertyStr(doc, "body", ...)`，未判 `JS_IsException`。

### GUI 应用与命令行工具（apps/gui / apps/coreutils）

**中**
- `c/apps/gui/textedit.c:71-83` — 特殊键被截断成控制字符插入文本：ABI 明确 EV_KEY 的非打印键 > 0xFF（`include/abi/aether_abi.h:89-96`），这里直接 `(char)e.a`，方向键/Home/End 变成 0x01–0x08 插入文档并随 Ctrl+S 写盘。对比 `terminal.c:124` 有正确过滤。修：插入前加 `if (e.a > 0xFF) continue;`。
- `c/apps/coreutils/cp.c:30-43` — `cp -r` 复制目录到自身子树无防护：`cp -r /a /a/b` 无限递归直到 128 字节路径截断才碰巧终止（期间产生大量嵌套目录）。`files.c:236` 有 `path_under()` 防护，cp.c 没有，行为不一致。

**低**
- `c/apps/coreutils/ls.c:13-16`、`c/apps/gui/files.c:158,244-249,193` — `dir_name()` 返回 -1 时 `nm[]` 未初始化即使用（打印栈垃圾/未初始化读取）。建议调用方先 `nm[0]=0`。
- `c/apps/coreutils/sh.c:67-84` — `sys_fork()` 失败（pid<0）落入父进程分支：负 pid 压入 `pids[]`，该 stage 静默丢失。
- `c/apps/gui/terminal.c:60-64` — `spawn_shell` 失败路径 fd 泄漏；shell 退出后 `in_w` 永不关闭。
- `c/apps/coreutils/smptest.c:63` — `run_batch` 中途 fork 失败直接 return：子进程不 reap、fds 不关（测试工具）。
- `c/apps/coreutils/cat.c:4`（及 wc/head）— `sys_write` 返回值不检查：管道短写时静默丢数据。
- `c/apps/gui/files.c:102-133`、`c/apps/coreutils/cp.c:30` — `copy_tree`/`delete_tree`/`do_cp` 递归深度无界：过深目录树可耗尽 ring-3 栈。
- `c/apps/gui/studio.c:129` — `scan_error` 的 `n = n*10 + digit` 无上限 → int 溢出 UB。
- `c/apps/gui/studio.c:308` — EV_CLOSE 只 `sys_close(run_fd)` 不 `sys_waitpid(run_pid)`：正在运行的 as 子进程变僵尸。
- `c/apps/coreutils/sh.c:48-51` — `run_external` 拼 `/bin/` 前缀时超长命令名静默截断后以错误路径 exec，报误导性 "command not found"。
- `c/apps/gui/terminal.c:111-115` — WM 传入的文件路径未经引用直接进 shell stdin：含空格的路径拆成多个参数（本地触发，风险低）。

### 构建系统与工具链（Makefile / boot / tools）

**中**
- `tools/genroots.py:10` — 输出路径是已失效的 `src/crypto/trust/`：仓库已从 `src/` 迁移到 `c/`（真实文件在 `c/crypto/trust/roots_bundle.inc`，`Makefile:104` 引用后者）。现在运行会在 `open(OUT,"w")` 处 FileNotFoundError；若有人误建 `src/` 目录则静默写到错误位置，内核继续用旧根证书无人察觉——信任根更新的供应链工具，值得重视。（agent-7 报低、agent-12 报中，已合并取中。）
- `c/boot/enter_user.asm:23-58` — 新建/fork 的 ring-3 线程不初始化 FP/SSE 状态：`ring3_bootstrap` 和 `fork_ret` 都直接 iretq 进 ring 3，无 fninit/ldmxcsr/fxrstor。后果：(a) 新进程启动时 XMM0-15/MXCSR 是前一个进程/内核留下的残值（跨进程信息泄漏，QuickJS 大量用 XMM）；(b) fork 子进程不继承父进程 MXCSR 控制字（舍入模式不是 caller-saved 语义，`fenv.c` 的 fesetround 设置会静默丢失）。修：iretq 前加载干净默认 FP 状态；fork_ret 复制父中断帧 FXSAVE 区并先 fxrstor。

**低**
- `c/boot/switch.asm:29-40` — `popfq` 与 `cli` 之间存在一条指令的中断窗口：当前所有手工构造的帧都用 IF=0（目前不可达），属仅靠调度器侧不变量维系的防御纵深。修：popfq 前 `and qword [rsp], ~0x200`。
- `c/boot/ap_trampoline.asm:42-43` — AP 启动只读 CR3 的低 32 位：pmm 一旦返回 4 GiB 以上的 PML4 帧，所有 AP 加载截断 CR3 立即三fault（当前 512 MiB 下安全）。
- `c/boot/isr.asm:40` + `c/kernel/cpu/idt.c:41` — 双重 fault 无 IST 栈：#DF 成因若是内核栈溢出，#DF 压栈再次失败 → triple fault，配 `-no-reboot` 表现为静默死机无诊断。修：TSS 给 IST1 配专用 #DF 栈。
- `tools/gen_libcss.sh:11,17` — 可预测 /tmp 路径 + properties.gen 键名未消毒（vendored 可信输入）。
- `tools/mkaex.py:9-18,24,28` — 无参数校验：`r & 255` 静默回绕超范围颜色；`name.encode()[:31]` 可能在 UTF-8 多字节中间截断。
- `tools/mkfs.py:190` — `partition(":")` 与 Windows 盘符路径冲突：`D:\foo` 被解析成 host=`D`（当前 Makefile 只传相对路径）。
- `Makefile:81,95` — rustup 缺失时 RUST_BIN 为空，报错 "/cargo: No such file" 无指向性；`mkfont.py:16-17` 硬编码 macOS 字体路径且字体被 .gitignore——非 macOS fresh clone 后 `make` 必挂在字体步；`mkwallpaper.sh` 同理。（agent-12/13/14 三处重复，已合并。）
- `tools/gen_compile_commands.py:13,16` — 依赖 cwd 和 GNU find（Windows 无 GNU find 直接崩；IDE 辅助工具）。

### Rust 组件（rust）

**中**
- `rust/src/inflate.rs:156-158` — stored 块不校验 NLEN：`b.pos += 4` 直接跳过 LEN+NLEN，不查 `NLEN == ~LEN & 0xffff`（puff.c 会查）。损坏流被接受并复制错误长度数据（无越界，输出垃圾）。
- `rust/src/png.rs:331,355,390` — 内核态大额分配的资源耗尽面：8192×8192 上限下单张 PNG 触发 ~536 MB scratch + 256 MB 输出的 kmalloc，壁纸走内核堆，本地恶意文件可造成系统级 DoS（失败路径有检查，不会崩）。修：按调用域收紧上限。

**低**
- `rust/src/inflate.rs:238-241` + `png.rs` 整体 — 完整性校验缺位：不校验 FCHECK/CINFO≤7/adler32，PNG chunk CRC 全跳过（既定取舍，对网络来源数据是弱点，建议至少校验 adler32）。
- `rust/src/png.rs:267,108,328` — 宽松接受多项 spec 违规：interlace 未限 0/1；filter type >4 静默当 None；不检查 IHDR 必须是首块（均无越界，只产生错误图像）。
- `rust/src/lib.rs:23-26` — panic handler 为 `loop {}` 忙等：意外 panic 时永久烧 CPU 而非停在有诊断价值的状态；dev profile 默认开 overflow-checks，与 release 行为不同。

### 测试与 CI 设施（tests / Makefile test-*）

**中**
- `Makefile:85` — `.PHONY` 漏掉 `test-kheap`、`test-png`、`test-jpeg`：工作区出现同名文件即静默跳过测试。
- `tests/unit/` — 17 个测试源没有任何 make 目标：`parse_fuzz.c`、`x509_fuzz.c`、`pipeline_stress.c`、`as_stress.c`、`img_fuzz.c`、`img_test.c`、`rsa_test.c`、`ecdsa_test.c`、`inflate_test.c`、`ttf_test.c`、`utf8_test.c`、`var_test.c`、`page_test.c`、`layout_test.c`、`dom_test.c`、`css_engine_test.c`、`css_dump.c`、`js_dom_test.c`、`raster_test.c` 均无 target——覆盖"远端/恶意文件"攻击面的安全 fuzzer 全部失联，写了但 CI 跑不到。修：至少把 ASan fuzzer 接成 `test-fuzz`。
- `tests/qmp/qmp_fs.py:105-110`、`tests/qmp/qmp_term.py:64` — 磁盘探针跨运行残留导致假 PASS：两个脚本不带 `-snapshot`，一次成功后探针字符串永久留在 `disk.img`，之后即使 QEMU 内操作完全失败照样 PASS。
- QMP 驱动脚本坐标集体过期：第 7 个 Dock 应用加入后 `qmp_fs.py:96`、`qmp_term.py:56`、`qmp_apps.py:54`、`qmp_browser.py:60`、`qmp_stress.py:57` 全是旧坐标；光标原点也不统一；`qmp_demo.py`/`qmp_drag.py` 假设已不存在的 "Finder" 布局——这批手动脚本大概率已静默失效。

**低**
- `Makefile:81` — RUST_BIN 无空值检查（已并入构建节）。
- 工作区 5 个 shell 测试脚本 exec 位丢失（git diff 显示 100755→100644；Makefile 用 `sh` 显式调用故不受影响）。
- 引导脚本固定 sleep 时序脆弱：`run-as-test.sh:18`/`run-shell-test.sh:18` 仅 sleep 4s，`run-smp-test.sh:20` 自述需约 10s（TCG 慢机器 flaky）。建议轮询 serial 提示符。
- `tests/qmp/qmp_fs.py:20-21`、`qmp_files.py:36-37`、`qmp_term.py:4` — `tempfile.mktemp()` 已废弃、有竞态且不创建文件。改用 `mkstemp`。
- `run-selfhost-lex.sh:13`、`run-selfhost-compile.sh:41` — 对绝对路径参数失效（假定 ASC 为相对路径）。
- `run-test.sh:24` — `file=$DISK` 未加引号。
- `tests/unit/libctest_main.c:34` — `CHK_INT(memcmp(...), -1, ...)` 假定 memcmp 恰好返回 -1（C 标准只保证符号，断言过规定）。
- 各 fuzz/harness 卫生：`img_fuzz.c:20`/`x509_fuzz.c:15`/`pipeline_stress.c:42` fread 返回值不查、`malloc(0)` 边界；`rsa_test.c:9` hexb 无输出缓冲边界（输入硬编码）。

---

## 与 BUG_BACKLOG.md 的关系

backlog 的 13 项已知 bug 全部在本次审计中再次出现，且经复核**均未修复**：

| backlog id | 级别 | 本次审计位置 | 状态 |
|------------|------|--------------|------|
| C2 | high | H-12（`tls.c:421`） | [已知] 未修；backlog 标注 "the one urgent" 已逾 8 周 |
| M2 | high | H-13（`tls.c:319-321`） | [已知] 未修 |
| M3 | medium | TLS 节中危（`tls.c:442,445-447`） | [已知] 未修 |
| M11 | medium | 浏览器节中危（`css_engine.c:64-73`） | [已知] 未修 |
| H2 | low | TLS/HTTP 节低危（`http.c:152`） | [已知] 未修 |
| H3 | low | TLS/HTTP 节低危（`url.c:37`） | [已知] 未修 |
| H5 | low | 网络节低危（`ip.c`） | [已知] 未修 |
| H9 | low | 驱动节低危（`e1000.c:104,108,123,127`） | [已知] 未修 |
| M20 | low | 驱动节低危（`virtio.c:81`） | [已知] 未修 |
| H11 | low | 内核节低危（`smp.c:226`） | [已知] 未修 |
| H17 | low | AetherScript 节（`vm.c:612,620`） | [已知] 未修（子代理复核确认） |
| H18 | low | AetherScript 节（`vm.c:681`） | [已知] 未修（同上） |
| M23 | low | libc 节中危（`stdlib.c:107`） | [已知] 部分过时：`strtoll` 疑似已修（现有 cutoff 防护），`strtod` 仍未修；backlog 未更新 |

**新增发现**：除上述 13 项外，本报告全部条目均为 backlog 之外的新发现，包括全部 8 条严重问题。另有两项 backlog 完备性问题（来自 agent-14，记录在案）：

- backlog 声称 triage 自 `BUGS_EXTENDED.md` 的 62 条 claim，但该文件不在仓库中（git 全分支也未找到）——33 项 FALSE 和 9 项 DESIGN 的原始论证已不可考。
- FALSE 抽查（M9-ARP-VALIDATE）：作为内存安全 claim 判 FALSE 正确，但 `arp_input` 不校验 hlen/plen/op 且无条件 `cache_put`（gratuitous ARP 投毒）是被静默接受的设计取舍，更像 DESIGN 而非 FALSE，backlog 未记录。

---

## 修复优先级建议（Top 10）

排序原则：远程可触发 > 本地提权/内核破坏 > 稳定性 > 其他。

1. **S5 证书链 BasicConstraints**（`x509.c:358-376`）——远程 MITM 任意域名，整套 TLS 认证失效；修复量小（解析一个扩展 + 两个比较）。
2. **S8 rsa_public elen 校验**（`rsa.c:130-137`）——远程内核栈溢出，潜在代码执行；一行长度检查。连同 H-14（`rsa.c:81` off-by-one）一并修。
3. **S6+S7 TLS flight/Certificate 解析边界**（`tls.c:299-365`）——远程内核 OOB 读/崩溃；与 C2/M2/M3 合并为同一个 TLS 加固批次一次落地。
4. **[已知 C2] tls_recv 缓冲溢出**（`tls.c:421`）——远程 BSS 溢出写，backlog 标 urgent 已逾 8 周，一行检查，随第 3 项同批修。
5. **[已知 M2+M3] CertVerify/DER 签名边界**（`tls.c:319-321,442-447`）——远程 OOB 读，同批修。
6. **S2 ELF 加载器 VA 界限**（`elf.c:50-57`）——本地 ring-3 → 内核任意写；两行范围检查。
7. **S1 execve TLB flush**（`exec.c:111` / `vmm.c:150-168`）——本地 ring-3 → 内核物理内存读写；一行 CR3 reload。
8. **H-2 file_write 偏移溢出**（`file.c:222,163-167,234-247`）——本地 ring-3 → 内核破坏/整机冻结；三处溢出守卫。
9. **S3+S4+H-8 AetherFS 盘上数据校验**（`aetherfs.c:77,134,276`）——不可信镜像 → 内核堆破坏；统一在 `imap`/`bfree`/`inode_read` 三个隘口加范围校验，一处修补覆盖大部分攻击面。
10. **H-5 GUI_BLIT 尺寸 clamp + H-3 SYS_PIPE 悬挂 fd**（`wm.c:551-561` / `syscall.c:245-247`）——本地冻结/UAF，各一行级修复。

次优批次（稳定性/正确性）：H-1（smp BKL 自死锁）、H-6/H-7（NVMe/virtio 超时 desync）、H-16（TTF VLA）、H-17（malloc sentinel）、H-18（GC 标记反向）、H-20/H-21（浏览器 JS-DOM 桥）、H-25（inflate stored 对齐）、H-24（Makefile 大小写）。

---

## 已剔除的误报

**无整条剔除。** 所有严重与高级发现均能在当前源码中复现。有两处修正与一处降级，记录如下：

- **降级（高→中）：e1000 TX 描述符 status 未声明 volatile**（`e1000.c:196-198`）。缺失 volatile 属实（`e1000.c:91` 为普通指针），但原报告称"加载被提升出循环后 spins 计数器也救不了、永久自旋"不准确：`++spins > 1000000` 的兜底在循环体内、与 status 加载无关，即使加载被提升也仍在 1M 次迭代后返回 -1。真实后果是条件恒假时每次 TX 等待空转 1M 次后误报超时（功能受损 + net_lock 内短暂卡顿），不是死锁。已按此修正后果描述并降级为中。
- **行号修正 1**：S5 原文引用 `x509.c:366-375`，实际 `x509_verify_chain` 函数完整体为 `x509.c:358-376`（链上验签循环确在 366-374，已按函数范围修正）。
- **行号修正 2**：S6 原文引用 `tls.c:306-315`，Certificate 解析体实际起于 307 行（`cp = 0; cp += 1 + mb[0]`），已修正为 `tls.c:307-315`。

另外说明两点不确定性，供评审参考：

- H-20（QuickJS class 跨 runtime）的 QuickJS 内部数组边界论证（`class_count` 初值、`class_proto` 无边界检查）引自子代理对 `third_party/quickjs/quickjs.c` 的核读，本报告未逐行复核 vendored 代码；但仓库侧事实（静态 `elem_cid`、`browser.c:62` 每页新建 runtime、`js_dom.c:186` 的 `if (!elem_cid)` 守卫）均已独立确认，且第二页必走未注册路径，结论方向可靠。
- agent-9 高 #2（`.la` 未校验）引用的 `vm.c:1085-1092`（OP_CLOSURE upvalue 处理）未逐行复核；加载器只验证文件结构（`as_bc.c:151-238`）与 VM 侧 READ_CONST/跳转/槽位无界（`vm.c:525,566-567,699-701`）已确认，足以支撑结论。

---

## 修复状态（2026-08-04）

全部 8 严重 + 23 高 + 范围内中/低危已修复（11 个模块并行修复 + 收尾核验，含跨模块漏项 aex_info 与 rtc.c 月份校验）。修复后已安排 WSL 全量回归（编译 + 主机单测 + QEMU 引导测试）。

### 按模块修复要点

| 模块 | 修复要点 | 主要跳过/遗留项 |
|------|----------|------------------|
| tls/crypto | 4 严重 4 高全修：BasicConstraints 链校验、rsa `elen` 校验、`mont_mul` 上限收紧到 516 字节、x25519 共享密钥全零检查，及 C2/M2/M3/S6/S7 等解析边界 | `tls_close` 不发 close_notify；x509 `pathLenConstraint` 未解析 |
| fs | `bfree`/`inode_read`/`dir_add`/mount 全面校验盘上不可信数据；ATA LBA28 越界守卫 | `aetherfs_rename` 回滚失败仅告警（UAF-on-disk 风险记录在案） |
| kernel | vmm execve 后 CR3 reload 清 TLB、ELF 加载 VA 界限、`file_write` 偏移溢出、`file_close` 锁内快照拆毁、SYS_PIPE 悬挂 fd、smp BKL 前 `cli`、H11 AP 栈泄漏 | fork 不继承 fenv（舍入模式不传递） |
| gui 内核 / PCI | BLIT/RECT/TEXT 的 px/尺寸统一收敛 clamp、拖拽 `dragging` 悬索引、`wm_launch` 失败路径泄漏、`text_measure` font_ok 守卫 | `pci_find` 只扫 bus0/func0（设计限制） |
| drivers | nvme 完成队列 `cid` 校验、virtio `free_head` 轮转修复超时 desync、e1000 RX 长度校验 + volatile + H9 分配检查、多处 init 失败泄漏、pit/serial/rtc 无界等待加超时 | — |
| net | TCP/IP 入站校验和验证、RST 序列号窗口校验、DNS txid 随机化、`arp_warm` 退避、H2/H3 解析溢出 | tcp 不记录对端通告窗口（性能取舍）；UDP 入站校验和未验证 |
| lib/libc | ttf VLA 消除（改 scratch 尾部）、malloc HDR sentinel 修正、strtod 指数钳制、stdio Inf/%f UB、gif 交错行、utf8 严格校验（拒 overlong/代理区）、jpeg DC 类别封顶 | — |
| as | GC 标记 OOM 改 skip-sweep（保守存活）、`.la` 加载后 verifier 遍、H17/H18 INT64_MIN 检查、多处 realloc/NULL 防护 | `.la` 栈效应无法静态校验（已文档化） |
| browser | js_dom epoch 句柄修 UAF、Element class 跨 runtime 重新注册、M11 classes 泄漏、css_vars 递归深度限制、dom `newnode` 返回值检查 | — |
| gui apps / coreutils | terminal Tab 死循环、files 多选删除索引漂移、textedit 特殊键截断、`cp -r` 自递归防护、sh fork 失败分支 | sh 无 quoting（空格路径拆参）；后台进程僵尸（EV_CLOSE 不 waitpid） |
| build / rust / tools | Makefile `find c` 大小写、inflate stored 对齐回退 + NLEN 校验 + adler32、png 校验、genroots 输出路径、mkaex/mkfs/gen_compile_commands 跨平台、enter_user FP 状态初始化、switch.asm IF 窗口 | png.rs 尺寸上限按策略放宽；tests/unit 17 个测试源仍无 make 目标 |

### 修复中新发现并顺手修复的问题

- `c/fs/aetherfs.c` — `inode_write` 失败路径双重 `bfree`。
- `c/kernel/sched/sched.c` — `thread_create_idle` 的 `kmalloc` 返回值未检查。
- `c/kernel/cpu/smp.c` — `cpu_apicid` 数组越界写。
- `c/drivers/virtio/virtio.c` — descriptor `head` 恒为 0，使 used entry id 校验失效。
- `c/kernel/gui/text.c` — 字体回退路径除零。
- `c/apps/libc/src/stdio.c` — `%f` 第二处浮点强转 UB。
- `c/apps/gui/files.c` — `copy_tree`/`delete_tree` 目录条目处理缺陷。
- `c/apps/as/` — 多处 realloc 覆写旧指针 / NULL 解引用防护补齐。
- `c/lib/image/jpeg.c` — DC 类别封顶后，原"well clear of overflow"注释成立。
- `c/drivers/net/e1000.c` — `delay()` 死代码，记录在案。

### 遗留风险清单

- `c/apps/as/as_bc.c` — `.la` verifier 无法静态校验栈效应：栈平衡依赖编译器正确性，已文档化为"仅信任自产"。
- `c/net/transport/tcp.c` — 不记录对端通告窗口：性能取舍，恒按 1460 字节发送。
- `c/kernel/pci/pci.c` — `pci_find` 只扫 bus 0 / func 0：设计限制，不遍历 secondary bus。
- `rust/src/png.rs` — 尺寸上限放宽：策略性取舍，内核堆大额分配面仍在。
- `tests/unit/` — 17 个测试源无 make 目标：安全 fuzzer 未接入 CI。
- `c/boot/enter_user.asm` — fork 不继承 fenv：子进程丢失父进程舍入模式设置。
- `c/net/tls/tls.c` — `tls_close` 不发 close_notify：与 `tls.h` 注释契约的行为简化。
- `c/net/tls/x509.c` — `pathLenConstraint` 未解析：只校验 cA=TRUE。
- `c/fs/aetherfs.c` — `aetherfs_rename` 回滚失败仅告警：IO 错误时可致盘上 UAF。
- `c/net/transport/udp.c` — 入站 UDP 校验和未验证。
- `c/apps/coreutils/sh.c` — 无 quoting：含空格路径被拆成多参。
- `c/apps/gui/studio.c` — 后台进程僵尸：EV_CLOSE 只 close 不 waitpid。

---

## 终端卡死专案（2026-08-05，用户报告「开关应用后再开终端必卡死」）

**根因（已复现并修复）**：`c/kernel/gui/wm.c` `wm_process_mouse` 的左键命中测试先查窗口、后查 dock。dock 是绘制在所有窗口之上的 chrome（悬停 tooltip 已按顶层解析），但高窗口（如 Code Studio，底边伸进 dock 条带区域）会在点击路径上先命中，把 dock 图标的点击吞成窗口内容点击——「你看到的图标不是你点到的图标」。复现链：开机 → 开 Code Studio（窗口底边盖住 dock）→ 点终端图标 → 点击被吞、`wm_launch` 从未调用、终端永远不开。修复：dock 命中测试提到窗口测试之前。

**同场修复的 5 个稳定性 bug（对抗性审查发现）**：

- `c/drivers/net/e1000.c` — `e1000_rx_poll` 的 drain 循环无界：每次迭代立刻把 buffer 还给 NIC（写 RDT），持续 RX 洪峰下 NIC 重新置 DD 的速度 ≥ 清 DD 的速度时循环永不退出；调用方若是 NIC IRQ（vector 65，IF=0 + 持 BKL）= 全系统硬卡死。TCG/user-net 安静所以 fuzz 炸不出来，真实网络/桥接环境可触发。修复：一圈预算上限（RX_DESC），剩余由下次 RXT0 IRQ / net_poll 兜底。
- `c/kernel/gui/wm.c` `wm_launch` — 入口处无条件 `cli`、出口处无条件 `sti`：从 int 0x80（SYS_OPEN_PATH，Finder 打开文件）进入时 IF=0，返回后 IF=1，系统调用出口路径全程 IF=1 + 持 BKL，嵌套 IRQ 窗口超出 int 门设计假设。修复：入口 pushfq 保存、出口按保存值恢复。
- `c/kernel/cpu/tlb.c` `tlb_flush_all` — 无界等 ack：排队等 BKL 的核 IF=0 无法响应 IPI 240，任何未来调用方都会立刻死锁（`smp_present_par` 对此既有竞争闸门又有超时，此处两者皆无）。修复：有界等待，未 ack 核跳过（下次 CR3 切换自然 flush）。当前无调用方，属拆除引信。
- `c/kernel/gui/wm.c` — `g_net_busy` 看门狗：线程死于 fetch 中途（关窗/fault）时标志永久卡 1，`net_poll` 永不再跑（重传/FIN 回收全停，后续所有 http_get 连环失败）。修复：100 秒超时自动失效。
- `c/kernel/cpu/interrupts.c` — **偶发硬冻结（约 1/14）的根因，循环等待死锁**：`timer_tick()` 原先在 timer IRQ 拿到 BKL 之后才执行，而整个网络栈（dns/arp/tcp/tls/http）的超时与重传全部以 `timer_ticks()` 为时钟。当 fetch 线程被调度到 AP 核、持 BKL 阻塞等网络超时（`SYS_HTTP_GET` 全程持 BKL）时，BSP 的 timer IRQ 在 BKL 上自旋（IF=0）→ 全局 ticks 冻结 → fetch 的超时/重传永不触发 → fetch 永不结束 → BKL 永不释放，四方互锁。症状全部吻合：WM 秒级时钟停（BSP 卡在 timer ISR）、串口沉默（四核全灭）、QEMU 存活（guest 自旋）、低概率（需「fetch 在 AP 上」且「该次 fetch 丢包/停滞」同时成立）。修复：BSP 的 `timer_tick()` 移到 BKL 获取之前（ticks 是单写者 volatile 计数器，读者本无锁）。由全内核静态审查（排除法：其余轮询/自旋路径逐一核实有界）定位。

**验证**：`tests/qmp/qmp_freeze.py`（Studio 编辑 → 开终端 → uname → 拖拽）与 `tests/qmp/qmp_freeze2.py`（Studio 编辑→关闭 → Monitor 开→关 → 终端 → uname）在 `make run` 全参数（-smp 4 TCG + virtio-gpu + e1000 + 真实显示窗口）下通过，终端正常启动并执行命令。tick/BKL 修复后，`tests/qmp/qmp_watch.py` 零扰动冻结猎手连跑 **30/30 轮无一冻结**（修复前基线约 1/14 轮冻结）。注：QEMU TCG 下从未复现出「发白+全系统冻结」的确切画面，已修复的 6 个 bug 覆盖该类症状的已知路径；wm_launch 的各失败分支已补全串口日志（此前 4 处静默返回），若现场再现有日志可查。

**黑框专案（同日第二现场，QMP 取证实锤）**：用户 100% 复现流程「关 Finder → 关 Clock → 开 Code Studio → 关 Studio → 开终端」之后终端黑框 + 菜单栏时钟停（整机冻结）。用 QMP 看门狗（画面静止 10 秒自动 dump 全核寄存器与访客内存，`tests/qmp/` 同方法）活捉现场：串口止于 `[execve] pid 6: /bin/sh loading`；CPU0/1/2 全部停在 `spin_lock_irqsave` 的 ticket 等待循环（IF=0，RDI=&g_bkl），`g_bkl` ticket−serving=4（1 持有人 + 3 等待者），`g_bkl_owner`=3；CPU3 在 sh 的新用户地址空间（CR3 已切换）里从 `pipe_read` 空调用 `schedule()`。

**根因**：`pipe_read`/`pipe_write`/`proc_waitpid` 的阻塞等待循环用裸 `schedule()`。`schedule()` 在所有其他线程都 running=1（各自核上运行或排队等 BKL）时走 next==prev 分支立即返回 → 调用方自旋重试且**全程持 BKL** → 其余核全堵在 BKL ticket 队列（IF=0，无法响应任何中断/IPI）→ 整机冻结。触发条件苛刻：需要「可运行线程数 ≤ CPU 数」窗口——用户关掉 3 个应用后剩 4 线程对 4 核，人人有核、无人可换，必中；此前自动化脚本一直留着 Finder/Clock（6 线程对 4 核，总有 running=0 的线程可切入），故 35+ 轮从未复现。`tty_read`（file.c）早前修过同一病灶，修复模式照抄：新增 `bkl_hlt_wait()`（sched.c）——cli → in_kernel=0 → 释放 BKL → sti;hlt;cli（睡到下一个中断，让出本核）→ 重拿 BKL → in_kernel=1——替换 pipe_read、pipe_write、proc_waitpid 三处裸 `schedule()`。`schedule()` 的 next==prev 语义本身未改（抢占路径依赖它），SYS_YIELD 保持原样（一次性让出，不会楔死）。

**同场修复**：virtio-gpu 分辨率锁死——QEMU 窗口未撑开时 GET_DISPLAY_INFO 回报 640x480，驱动按此建 framebuffer，所有 1280x800 布局的窗口画出屏外（症状：「打不开预览/Studio/浏览器」）。修复：Makefile 的 `QEMU_GPU` 钉定 `xres=1280,yres=800`（EDID），驱动侧 GET_DISPLAY_INFO 后钳制 `w<1024||h<700` 回退 1280x800。

**取证方法备注**：QMP `human-monitor-command` 的 `cpu N` 无法切核（每次调用回 CPU0），须用 `info registers` + `cpu-index` 参数；访客内存用 `xp /2wx 0x<addr>`（低地址恒等映射）读 `g_bkl`/`g_bkl_owner`（地址随 build 变，用 `nm build/kernel.elf` 重取）。诊断埋点 `[execve]`/`[fork]`/`[pipe]`/`[sched] first-run tid` 保留在代码里（first-run 的线程名偶发乱码，boot sh 名字指针寿命问题，仅打印层面）。

---

## WSL 回归测试结果（2026-08-04，Ubuntu 26.04 + clang 21 + qemu-system-x86_64 TCG）

- **全量编译**：kernel.elf + aether.iso + disk.img（全部 .aex、真实 rust 静态库）零错误。
- **主机单测 8/8 通过**：test-as(254)、test-as-gcstress、test-tcp-host(26)、test-complete、test-fb-clip、test-kheap、test-png、test-jpeg。
- **QEMU 引导 5/6 通过**：test、test-nvme、test-shell、test-libc(93/93)、test-as-os。
- **test-smp 失败但已证实为环境问题**：`T1=4s TN=18~21s children=4 distinct_cpus=4 corruption=0` —— 4 核全部跑到、无数据损坏，仅无 wall-clock 加速。用修复前基线（HEAD 3973dec）建 git worktree 做 A/B 对比，基线同样失败（`T1=6s TN=22s`），签名一致，判定为 x86 TCG 模拟器多核串行化所致，非本次修复引入的回归。真机/KVM 下应复测。
- **回归中捕获并修复的 2 个修复引入问题**：virtio cap 遍历 `continue` 跳过 `cap=next` 导致死循环（"missing caps" → AETHER_FB_FAIL）；elf.c VA 安全检查误拒 ld.lld 新版生成的 vaddr=0x200000 只读 headers PT_LOAD（改为跳过不映射）。均已复测通过。

---

## 后续 TCP/IP 补充（2026-08-05）

本节是审计快照之后的后续记录，不改写上文当时成立的发现。它取代「遗留风险清单」中“TCP 不记录对端通告窗口”和“UDP 入站校验和未验证”两项：

- TCP：加入 MSS option 收发与 IPv4 536-byte 缺省值；记录并按 `SND.WL1`/`SND.WL2` 更新对端窗口；限制发送量；零窗口 persist probe 指数退避；接收序列可接受性检查；超前 ACK 拒绝；RFC 5961 风格 exact-RST/challenge-ACK；未确认数据后的延迟 FIN；随机化 ISN；保留单 outstanding segment 与 OOO 重组模型。
- UDP/DNS：出站生成 UDP 伪首部校验和，入站非零校验和必须验证；保留 IPv4 零校验和兼容；记录源端口；DNS 响应同时校验随机 transaction ID、配置的 DNS 源地址与 53 端口。
- IPv4/ICMP：拒绝当前无法重组的 IPv4 分片、TTL=0 与不支持的源地址形态；ICMP 入站校验 checksum，echo reply 按源地址、ID、序列号匹配。
- 测试：新增 `make test-net` 与 `make test-net-os`；`TCP protocol tests` 39/39、`IPv4/UDP/ICMP protocol tests` 17/17 通过；相关网络对象以 freestanding x86_64 flags 强制重编译通过；QEMU 启动 smoke test 通过，guest 经 e1000/IPv4/TCP/HTTP 完整获取宿主 32768-byte fixture。

仍未实现的协议范围记录在 `docs/NETWORK.md`，包括 IPv6、DHCP、IPv4 分片重组、被动 TCP、拥塞控制、RTT/RTO 估计、窗口缩放、SACK、时间戳和完整 TIME_WAIT 状态机。
