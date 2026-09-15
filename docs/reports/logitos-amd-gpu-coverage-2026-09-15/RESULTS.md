# LogitOS AMD 与通用启动显卡覆盖结果（2026-09-15）

本轮增加了两层能力。第一层是 AMD/ATI 与 Intel 厂商范围的启动帧缓冲接管：只有固件
提供的完整 LFB 区间确实落在同一 PCI 显示设备的内存 BAR 内，设备处于 D0 且内存解码
有效时才绑定。第二层是精确限定到 `1002:5159` 的 RV100 2D 启动 canary：它在屏幕外
显存执行一次填充和一次拷贝，读回验证后恢复显存与 14 个寄存器。

桌面合成仍使用 CPU。现代 AMD 的 GCN、Polaris、Vega 和 RDNA 只进入安全诊断并停在
`firmware-gpuvm-ring-fence`，不会映射旧式 RV100 BAR 或发送命令。实体 AMD 显卡、现代
AMD 加速与 RV100 UEFI 路径尚未验证。

运行 `python3 reports/logitos-amd-gpu-coverage-2026-09-15/verify.py` 可重新核对最终 ISO
哈希、主机门禁、QEMU 身份、串口、MMIO trace、屏幕截图统计和内核链接。

## 已实现范围

- `amd-bootfb` 覆盖 PCI vendor `1002` 的 display class，实际主机夹具覆盖 Rage128、
  Polaris、Navi 10、Navi 21、Navi 31 代表设备。完整 LFB 所有权验证失败时保持零接管。
- `intel-bootfb` 覆盖 PCI vendor `8086` 的 VGA/3D display subclass，作为 14700KF 无核显
  情况之外的通用 Intel 显示回退；同样只保留已存在的固件帧缓冲。
- `rv100_accel` 仅接受 `1002:5159`，检查 PCI 命令、D0、BAR0/2、实际 VRAM、scanout、
  pitch、scissor 和两块对齐的屏幕外 canary 区域。
- RV100 命令等待同时有 5 ms deadline 和 100000 次迭代上限。命令提交后的 engine/cache
  timeout 会隔离上下文，不会在仍可能执行的命令上用 CPU 覆写显存。
- 启动 canary 完成 PATCOPY fill、SRCCOPY copy、idle/cache flush、读回和原始状态恢复。
  日志写成 `engine=rv100-2d-canary-passed desktop=cpu`，不宣称桌面已经接入 GPU。

寄存器定义与命令行为按固定 Linux 源提交
`587858367581b9c55c3690f4e63382ad622719d4` 的
[Radeon 寄存器头](https://github.com/torvalds/linux/blob/587858367581b9c55c3690f4e63382ad622719d4/drivers/gpu/drm/radeon/radeon_reg.h)
核对。来宾使用 QEMU 11 提交
`98b060da3a4f92b2a994ead5b16a87e783baf77c` 的
[ATI RV100 设备模型](https://gitlab.com/qemu-project/qemu/-/blob/98b060da3a4f92b2a994ead5b16a87e783baf77c/hw/display/ati.c)。
现代 AMD 停止在诊断阶段，因为完整驱动还需要
[GPUVM、rings、fences 与固件生命周期](https://docs.kernel.org/gpu/amdgpu/driver-core.html)。

## 验证结果

| 范围 | 结果 |
| --- | --- |
| RV100 主机核心 | 518/518；ASan/UBSan；宽泛 ID 与越界 surface 两个变异负控各精确失败 1 项 |
| AMD 启动帧缓冲 | 59/59；LFB 部分重叠、MEM decode off、混合 BAR、坏 PM cap 四个负控各精确失败 1 项 |
| Intel 启动帧缓冲 | 31/31；外厂、D3、PCI all-ones、错误 LFB 所有权负控均被抓到 |
| RV100 QEMU 来宾 | PASS；QMP 为 `1002:5159`；两次 `13x7` trigger、fill/copy master 各一次、cache flush 三次 |
| Rage128 QEMU 来宾 | PASS；真实 QEMU `1002:5046` 身份；被动保留 1024x768 scanout |
| 屏幕证据 | 两条路径均为 1024x768，抽样 221 种颜色，12288/12288 个样本为非暗像素 |
| 内核集成 | `amd_accel.o`、`amd_bootfb.o`、`intel_bootfb.o`、`rv100_accel.o` 和入口符号均在最终 map |
| Make 集成 | 336 个测试 fragment；335 个可达，唯一 declared 项为有意避免递归的 `schedneg.mk` |

QEMU RV100 trace 精确观察到两次 `DST_HEIGHT_WIDTH=0x0007000d`、fill master
`0x52f006de`、copy master `0x52cc36ff`，以及三次 `DSTCACHE_CTLSTAT=0xf`。串口同时报告
`fill=ok copy=ok restore=ok commands=2 desktop=cpu`，QMP 再核对 PCI 身份与非空 scanout。

## 最终产物

| 产物 | SHA-256 |
| --- | --- |
| `build-gpu-coverage-final/logit.iso` | `adbced2ffcb09dbfb1618fd9945d1c89241d5d912507de60ff51704f4ee3870c` |

证据保存在 `evidence/`，包括三份主机测试日志、RV100/Rage128 串口、两个 result JSON、
RV100 MMIO trace、两张 PPM 截图、最终 kernel map 摘要和产物哈希。

共享树的 BIOS loader 在最后重建期间被另一项工作改动，NASM 拒绝了那份尚未收敛的独立
改动。为避免覆盖或把它混入本轮，最终 ISO 复用了 `build-audit-rv100` 中此前已经通过
QEMU 的 loader 与匹配 bootinfo 对象，再重链本轮现编译的四个 GPU 对象。精确哈希和来源
记录在 `evidence/artifact-provenance.json`；因此本报告证明 GPU 组合产物，不证明当前共享树
中那份新 loader 源码能够构建。

## 尚未覆盖

现代 AMD 原生加速还缺少按代固件加载、GPUVM、内存控制器初始化、ring/fence、IRQ、故障
恢复、reset 与显示 modeset。当前的九个现代设备 ID 只是诊断家族样本；AMD 启动帧缓冲
覆盖按厂商与显示类判断，但不能据此称为所有 AMD 芯片都完成了原生加速。QEMU 的 ATI
设备模型只验证 BIOS RV100/Rage128；OVMF 对该模型没有 GOP，因此本轮没有伪造 UEFI
成功结论。
