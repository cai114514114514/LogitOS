# RX 580：SMU 目录与固件装载准备 — 2026-09-15

本轮把上一轮分开的固件解析结果接成可执行的 RAM staging 流程，并增加 SMU Test 消息状态机。三个代理完成协议核对、TOC 编码及消息传输测试，主线实现 staging 与真实文件消费验证。**RX 580 的产品驱动仍保持只读，未上传固件、未启动 SMU/SDMA 队列，也未实现实卡加速。**

## 新增代码

- `polaris_smu_toc.c/.h`：按真实 ABI 生成 344 字节 SMU 目录，包含最多 12 个 28 字节条目。地址高 DWORD 在前、低 DWORD 在后，避免字序错误；检查固件 ID、版本截断、flags、重复项、40 位地址、TOC 页和镜像范围重叠。输出同时报告已有项与缺项。该接口保守拒绝镜像重叠，MEC jump-table 子镜像若复用同一地址必须另行设计或独立分配。
- `polaris_smu_stage.c/.h`：真实解析两份 SDMA 文件，校验 IP 3.1，将目录、两个完整微码 payload 和零填充组装到页对齐的普通 RAM 镜像。所有失败均发生在写输出前；保持原输出和 metadata 不变。地址是显式传入的未来放置假设，函数不建立 GPU 映射。文件顺序及来源由调用者确认，通用文件头不能认证 SDMA0/1 身份。
- `polaris_smu_mailbox.c/.h`：有锁、时间与轮询双预算的 Test `0x100` 协议状态机。等旧响应结束，写参数，清除并读回 ACK，再提交消息；只有新响应 1 成功。不确定写入/提交超时后保持隔离，迟到 ACK 不允许自动重试。没有产品 MMIO 回调，也没有 LoadUcodes 接口。Test 本身不承诺无副作用；没有在实卡运行。
- `tools/stage-polaris-sdma.c`：使用同一内核实现，把真实文件生成检查用 RAM staging 文件；不是可烧录镜像。对应 Python 检查器独立验证 TOC、payload、padding 和地址变化。

## 实测

| 验证 | 结果 |
|---|---|
| TOC 主机 ASan/UBSan | 186 检查通过 |
| mailbox 主机 ASan/UBSan | 141 检查通过 |
| staging 主机 ASan/UBSan | 105 检查通过 |
| 新增检查合计 | 432 检查通过 |
| 强制前置负控 | GPU 地址高低字交换：2 项预期失败；把 0xfe 当成功：1；第二 payload 损坏：2 |
| 既有模块主机回归 | 驱动接线 259、固件解析 132、SDMA 包 208 检查通过 |
| 实文件端到端 | 官方两份 Polaris10 SDMA 固件成功构建并逐字节检查 |
| 实文件负控 | 损坏的第二 payload 被同一 oracle 拒绝；48 字节截断头被拒绝且原输出未变 |
| 地址变量对照 | 从 0x120000000 改成 0x340000000，只改变目录地址，不改变 payload |
| 内核 | 独立 BUILD 构建成功，三个模块符号已进入 ELF |
| Make 接线 | test-mk-wired 通过 |
| QEMU 回归 | 同一最终 ISO 的 RV100 桌面 GPU 呈现测试通过；不是 RX 580 测试 |

真实输入固定为 linux-firmware `1522c78ab870b3c051d8a3a1d24ecbbc12b23be5` 的 `polaris10_sdma.bin` 和 `polaris10_sdma1.bin`，来源与 SHA256 见 [provenance](firmware-provenance.json)。输出 36864 字节：TOC 在 0，payload 位于 4096 / 20480，各 12436 字节；目录/尾部 padding 均为零。normal PF 保留整个 payload，不臆测剥离摘要。已有 mask=0x6，缺失 mask=0x478，明确只是部分库存。

## 协议边界与下一阶段

本轮核对的是 **SMU74** ABI。TOC/消息依据固定 [Linux v6.12 目录结构](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/powerplay/inc/smu_ucode_xfer_vi.h)及[SMU7 加载实现](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/powerplay/smumgr/smu7_smumgr.c)。normal PF 完整加载 mask 为 0x47e，包含 RLC/CP/SDMA；未找到仅两份 SDMA 就能独立加载的依据，因此该 staging 不自动发送加载消息。

消息 ACK 与固件加载完成是两件事。Polaris 的 SoftRegisters 指针来自 SMU SRAM 的 0x20030，UcodeLoadStatus 在该结构偏移 0x6c；完整加载还需独立检查状态 mask。偏移由 [SMU74 原结构](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/powerplay/inc/smu74.h)编译 offsetof 核对；这些读取/加载步骤目前没有接入产品路径。

真正上传还缺 GPU 可访问内存分配与 GMC/GART 映射、SMU 本体启动、完整 RLC/CP 固件组及 scratch、缓存可见性和加载状态等待。SDMA ring 启动、完成 fence、复位与像素读回在其后。即使未来目录缺项变为零，也不能视为已加载或可呈现。

## 复现

```sh
make -j4 BUILD=build-rx580-smu all
make BUILD=build-rx580-smu-host test-polaris-smu-toc-host test-polaris-smu-mailbox-host test-polaris-smu-stage-host test-mk-wired
make BUILD=build-rx580-smu-host POLARIS_FIRMWARE_DIR=build-rx580-firmware-input test-polaris-smu-stage-files
python3 tests/boot/run-amd-rv100-present.py --iso build-rx580-smu/logit.iso --out build-rx580-smu/rv100-regression
```

实文件门禁要求上述固定哈希输入，且不自动联网。生成的固件派生文件只存于 build 目录，本报告只保存检查结果与哈希。所有 SMU 专属测试均是主机 RAM/假邮箱协议检查，无实卡寄存器、微码执行、GPU 加速比或浏览器性能证据。QEMU 用真实 RV100 PCI 模型作回归，未伪造 RX 580 身份。构建 ISO 仅含内核。

[端到端结果](staging-result.json) · [工具输出](staging-tool.log) · [主机日志](host.log) · [实文件日志](real-files.log) · [源码/产物绑定](source-and-artifacts.json) · [内核符号](kernel-symbols.txt) · [QEMU 回归](rv100-regression/result.json)
