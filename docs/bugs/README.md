# docs/bugs — 全库 Bug 审计（2026-09-16）

十个并行智能体对约 34.5 万行源码（C/Rust/asm）做了一轮通读审计。每份报告
由独立 agent 产出：先对照 `docs/CODE_AUDIT.md` 与 `CLAUDE.md` 的已知问题
清单去重，再按分区深读。**所有发现均带 file:line 与代码引用**；无法完全
证实的标 SUSPECTED。审计为静态阅读，未动态复现——修复前应先按报告中
的触发场景复现。

## 总量

| 严重度 | 数量 |
|---|---|
| critical | 1 |
| high | 10 |
| medium | 16 |
| low | 59 |
| **合计** | **86** |

## 报告索引

| 报告 | 分区 | 最重发现 |
|---|---|---|
| [01-kernel-mm-cpu-sched.md](01-kernel-mm-cpu-sched.md) | 内核 mm/cpu/sched/core/module/pci/audio | kheap magazine 快速路径使双重释放检测失效（high） |
| [02-kernel-exec-gui.md](02-kernel-exec-gui.md) | 内核 exec/gui | legacy ET_EXEC 栈放置可重映射镜像页（medium） |
| [03-fs-drivers.md](03-fs-drivers.md) | fs + 全部驱动 | lfsro 目录遍历无 size 上界（medium）；logitfs flush 失败后 RAM 与盘分叉（medium） |
| [04-net.md](04-net.md) | 网络栈 | **UDP 接收队列越界写（critical，远程内核内存破坏）**；TCP 零窗口探测停滞（high） |
| [05-crypto.md](05-crypto.md) | 密码学 | ML-DSA 秘密中间量不清零（low）——本区整体质量最好 |
| [06-text-audio-media-image-rust.md](06-text-audio-media-image-rust.md) | 文本/音频/媒体/图像/Rust | sbix dupe 环无界递归（medium） |
| [07-video-gfx.md](07-video-gfx.md) | 视频/2D/3D | H.264 first_mb_in_slice 有符号比较绕过→野指针写（high）；H.265 RPS 栈溢出（high） |
| [08-browser.md](08-browser.md) | 浏览器 | 网页缓存可绕过 8 MiB 上限 + 计数下溢（medium）；DOM 无树深防线（medium） |
| [09-as-libc.md](09-as-libc.md) | AetherScript + mini-libc | eval.c 字符串借用语义 UAF（high）；一元运算符静默返回操作数（high） |
| [10-gui-coreutils-studio.md](10-gui-coreutils-studio.md) | GUI 应用/coreutils/Studio | preview 音频声道溢出（high）；F 键被当 Unicode 码点注入（high） |

## 修复进度（2026-09-16 晚更新）

已修复并提交（均先看过红灯再转绿）：
- ✅ 04 critical：UDP 越界写 → 入队按 UDP_SLOT 拒绝并计数；udp_recv 的
  u16 窄化一并修复。回归 = net_proto_test.c 用例 14（修前实测入队成功
  drops=0，修后 ASan 与 test-net-proto 55/55 绿）。
- ✅ 07 high ×3：H.264 first_mb 有符号比较、H.265 keep[] 容量、H.265
  conf_win 校验。提交在库 fixture 修后仍逐位一致（b02a15b9 / 8e985b89）；
  畸形流负向探针待做（树内尚无码流写入器）。
- ✅ 09 high ×2：eval.c 字符串借用 UAF（改为赋值深拷贝，ASan 实证修前
  use-after-free、修后干净）；一元 `-`/`~` 静默丢弃（补 float 取负 +
  其余按名拒绝）。test-as-examples 保持绿。

其余 78 条待处理，按子系统分别记录在上列各报告中。

## 建议的处理顺序

1. **04 的 critical（UDP 越界写）与 07 的三个 high（解码器内存安全）**——
   远程/码流可触发的内存破坏，先复现再修。
2. 01 的 kheap 双重释放、09 的 eval.c UAF——本机可触发的内存错误。
3. 其余 medium 按子系统批处理；low 大多是契约/一致性问题，可攒着批量修。

## 审计边界（如实声明）

- 静态阅读，非动态验证：CONFIRMED 指代码路径可静态推演至缺陷，
  修复前仍应按触发场景复现。
- 已知问题（CLAUDE.md "OPEN BUG"、docs/CODE_AUDIT.md 的 31 条）已去重；
  各报告末尾列出了核实过确实已知的条目。附带收获：CLAUDE.md 记载的
  munmap 跨核 TLB shootdown 在当前 HEAD 已有修复迹象，该条目建议更新。
- 覆盖密度随分区而异：browser 12.6 万行无法逐行深读（报告内列出了
  未深读清单）；crypto 质量显著最好（0 中高危）。
