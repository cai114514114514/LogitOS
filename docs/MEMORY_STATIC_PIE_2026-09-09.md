# 高物理页、宽用户地址与静态 PIE：第一轮交付

2026-09-09，仓库 `/Users/wangzhe/system/LogitOS`。六种 BIOS/UEFI 配置实际运行通过；不是只报告固件内存容量。全部测试构建、镜像、磁盘副本与日志在 `/tmp/logitos-memory-20260909/`。

## 实现范围

- `pmm_init` 在共用内核路径建立 `0xffff800000000000 + physical` 的 supervisor physmap，只映射固件 AVAILABLE 的完整 RAM 页。低于 1 GiB 的对象继续走旧 identity alias。映射建立成功后才释放高页；RAM 空洞和设备 MMIO 不进入 RAM physmap。ACPI 保留表使用另一段只读、NX 的内核别名，修复大内存启动时固件表位于 3 GiB 的实际故障。
- `pmm_alloc/contig/reserve` 继续限制在 1 GiB 内；`pmm_alloc_any/reserve_any` 用于匿名页、COW、页缓存、SHM、ELF/用户栈及 swap-in。8 GiB 机器优先从 4 GiB 以上分配，再环回 1–4 GiB，最后回退低区。页表、内核堆、旧 DMA 继续低区；高 CPU 指针经已有低区 bounce buffer 做块 I/O。
- 用户地址是两个窗口的并集：`[0x40000000,0x80000000)` 与 `[0x10000000000,0x800000000000)`。中间空隙和 physmap 不获得用户权限。页表、VMA、usercopy、ELF 共用地址规则；clone/free 遍历所有私有用户子树，rmap 使用 64 位绝对 VPN。大申请进入宽窗口，旧程序的小申请优先旧区；稀疏范围按页表层级跳跃处理。
- 裸 ELF 与 AEX 内嵌 ELF 支持无解释器、无共享库依赖的静态 `ET_DYN`，保留旧 `ET_EXEC`。接受 `R_X86_64_NONE/RELATIVE`；符号重定位、REL/RELR、TEXTREL、PT_INTERP、DT_NEEDED 等明确拒绝。先验证、装载、重定位，再收紧段权限和 RELRO；TLS 使用重定位后的初始化数据，auxv 保留原始程序头并给出正确入口和凭据。
- 基址采用确定性的轮换，不宣称 ASLR。TLS 已接通 execve、proc_spawn、cap_spawn、WM 与首次 IRET；修复分发层覆盖新 FS_BASE 和加载 FS selector 清零基址的实际问题。现有用户线程 trampoline ABI 保持不变。

## 实际验收

| 启动 | RAM | 实测匿名物理页 | 内存流程 | 裸 PIE / AEX PIE |
|---|---:|---|---|---|
| BIOS | 512M | `0x31cb000` | 通过 | 均通过 |
| BIOS | 2G | `0x40106000` | 通过 | 均通过 |
| BIOS | 8G | `0x100107000` | 通过 | 均通过 |
| UEFI | 512M | `0x3184000` | 通过 | 均通过 |
| UEFI | 2G | `0x40107000` | 通过 | 均通过 |
| UEFI | 8G | `0x100108000` | 通过 | 均通过 |

每组都验证：真实匿名页首次触页、跨 PML4 边界、fork/COW 内容隔离、mprotect 与实际只读写故障、设备 swap 往返整页内容、页缓存真实磁盘读取，以及解除映射和回收。8 GiB 两组的匿名页和缓存页均明确位于 4 GiB 以上。512 MiB 两组验证低区回退。

每组预留 8 GiB 虚拟空间，只触碰五页。预留新增实际帧不超过 32，触页新增不超过 64。内核私有测试地址空间释放后，物理帧数和 swap 槽数精确回到原值；持续存活的 ring 3 进程解除映射允许最多 32 帧的页表/并行活动余量，未将其描述成整个桌面计数严格归零。

测试内核通过 `WIDEVERIFY=1` 才包含诊断入口。swap 验证选择一个独立地址空间的明确候选，然后调用真实 candidate/gather/try_swap 和真实 NVMe I/O；这是定向驱逐验证，不是宣称做过耗尽 8 GiB 的压力试验。普通内核中没有该诊断入口。

同一份 PIE 的 SHA256 为 `d72d734d8e1a78169ae32a3ecb45974ba423318bef52f51bc775fb2c6590885e`。裸 ELF 依次在 `0x10000000000`、`0x10010000000`、`0x10020000000` 运行，AEX 再用下一组三个基址。每组检查全局指针、函数指针、主线程和子线程 TLS 初始化及隔离、argv/env/auxv、fork/exec 与 cap_spawn 的退出结果。

负向控制均实际观察到预期失败：

- 禁用高页映射：host 构造页表测试失败；另有真实 8 GiB BIOS 客体在“匿名物理页超过 4 GiB”和“缓存物理页超过 4 GiB”断言失败。
- 漏克隆高用户子树：真实 MM host 测试明确丢失高地址叶节点及 swap 映射。
- 漏重定位：同一 host PIE 在全局、函数及 TLS 指针断言失败，不接受崩溃作为假阳性。
- ptrace 旧物理指针别名和页缓存跨别名边界的旧合并行为，各有针对性失败控制。

原 MM host 回归：PMM 13308、VMM 405、rmap 69、reclaim 333、pcache 226、forkfile 59、SHM 128，全部通过，原负控按预期失败。新增 physmap 4678 项分别覆盖 NX 开/关；宽 VA 96 项；PIE 普通与 ASAN/UBSAN 各 53 项；旧 loader 291 项通过。新增高页 SHM 接线经过静态检查与原低内存 host 回归，本轮六配置未单独运行高页 SHM 生命周期测试。

普通内核（没有 WIDEVERIFY）另在 8 GiB BIOS 上跑完三组桌面/TextEdit/输入法回归。独立磁盘抽取的保存结果分别是 `nihao `、`你好`、`你好，我爱中国。你好吗？女孩输入法二维码西安`，逐字节 UTF-8 一致；桌面与保存截图也已检查。六配置中的旧 shell、echo、true 正常执行。

## 构建与重跑

使用新的绝对 BUILD 路径；默认 `WIDE_BASE_BUILD=build` 仅复用已有产品应用。磁盘文件列表读取 Makefile 的真实 recipe，复用 `mk-tcc-disk.py` 的文件系统打包实现，没有复制维护第二份应用列表。没有重编或修改浏览器/第三方源码。

```sh
make BUILD=/tmp/logitos-wide-check WIDEVERIFY=1 test-wide-memory-os
```

此入口先运行三个 host 测试及其必需负控，再构建 ISO/ESP/独立测试程序和私有产品磁盘，启动六配置矩阵。PIE host 检查和 guest fixtures 使用不同目录，允许并行 make。macOS 默认使用 Homebrew QEMU 的 OVMF；其他安装位置通过 `OVMF_CODE`、`OVMF_VARS_SRC` 指定。

只构建独立静态 PIE 示例：

```sh
make BUILD=/tmp/logitos-pie pie-fixtures
```

普通内核及高 RAM IME 回归：

```sh
make BUILD=/tmp/logitos-release /tmp/logitos-release/logit.iso /tmp/logitos-release/esp.img
IME_TEST_RAM=8G python3 tests/boot/run-ime-usability.py /tmp/logitos-release/logit.iso /tmp/logitos-wide-check/wide-disk.img /tmp/logitos-ime-check
```

真实禁高页映射对照：

```sh
make BUILD=/tmp/logitos-nomap WIDEVERIFY=1 WIDENOMAP=1 /tmp/logitos-nomap/logit.iso
python3 tests/boot/run-wide-memory.py --build /tmp/logitos-nomap --disk /tmp/logitos-wide-check/wide-disk.img --out /tmp/logitos-nomap/results --modes bios --ram 8G --skip-pie --expect-failure
```

## 本次产物与边界

- 普通 BIOS 镜像：`/tmp/logitos-memory-20260909/release/logit.iso`
- 普通 UEFI 镜像：`/tmp/logitos-memory-20260909/release/esp.img`
- 产品加验收程序磁盘：`/tmp/logitos-memory-20260909/build/wide-disk.img`
- 六配置串口及桌面图：`/tmp/logitos-memory-20260909/matrix/`
- 普通内核 IME 结果：`/tmp/logitos-memory-20260909/release-ime/`
- 结构化结果和 SHA256：`/tmp/logitos-memory-20260909/acceptance.json`
- host 接线测试日志：`/tmp/logitos-memory-20260909/host-gates.log`
- 真实负控：`/tmp/logitos-memory-20260909/negative-guest/bios-8G/serial.log`

本轮没有更换既有应用编译方式，没有建设共享库动态装载器、PT_INTERP、dlopen、IOMMU、NUMA、内核高地址重链接或 Linux 二进制兼容层。内核堆、启动页表和旧驱动 DMA 仍限制在低于 1 GiB 的区域；大量 RAM 不消除低区耗尽的可能。PMM/rmap/cache 元数据仍是按物理页规模增长的低区数组，不宣称可以直接管理任意巨量 RAM。

所有新测试位于 `tests/unit`、`tests/boot` 和对应 `tests/*.mk`。本轮没有引入外部词表或新第三方组件；此前 IME 的许可继续保留在 `LICENSES/` 和 `fsroot/licenses/`，未将许可证混入内核实现目录。工作区中已有的浏览器和第三方改动保留，本轮写入操作没有触及这些路径。

最终一体化入口也已实际执行：`make -j4 BUILD=/tmp/logitos-memory-20260909/build WIDEVERIFY=1 test-wide-memory-os` 返回 0，包含全部 host 负控与六配置重新启动；日志为 `/tmp/logitos-memory-20260909/integrated-gate.log`，该次完整串口为 `build/wide-results/`。结构化验收文件中的哈希对应最终打包镜像。
