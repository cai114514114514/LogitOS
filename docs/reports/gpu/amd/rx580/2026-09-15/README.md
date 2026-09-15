# RX 580：SDMA 执行链与目录整理

2026-09-15。按用户“显卡尚未到手，先实现代码”的要求，主线与三个代理完成了从完整固件包、SMU 启动、SDMA 配置到脏矩形呈现的调用链，并整理了文件目录。**这轮已超出只读识别／RAM 包编码；尚不能宣称 RX 580 插卡开机即可自动加速。**

## 代码结果

入口是 [polaris/desktop.c](../../../../../c/drivers/gpu/amd/polaris/desktop.c)，核心编排在 [runtime.c](../../../../../c/drivers/gpu/amd/polaris/runtime.c)。所有相关生产模块已进入最终 `kernel.elf`，见 [symbols.txt](symbols.txt)。

- 完整固件 bundle：7 份引擎文件生成 9 个 TOC 项，MEC 主代码长度与 JT1/JT2 分开处理；两份 jump table 独立存放。SMC 单独解析。官方固定文件生成 385024 字节，已有 mask `0x5fe`、所需 mask `0x47e`、缺项 0；此处的“完整”指 normal-PF 固件库存，不代表已被实卡加载。
- 显存布局：TOC、819200 字节 SMU scratch、固件、ring、fence、staging 均放入显式独占 arena，检查 scanout／保留区、40 位边界、CPU 别名和工作区覆盖。GART 编码器提供 GMC8 PTE／页表构造；当前呈现路径使用 VRAM，不启用系统内存 GART。
- SMU loader：使用实际 MMIO 与间接 SRAM 协议，覆盖运行中 SMU、普通冷启动和受保护冷启动；核对安全密钥、上传 SMC、fresh ACK 的有效低 16 位，以及 SoftRegisters 内的完整加载状态。全 1 读值、假 ACK、缺失状态和超时均拒绝。
- SDMA engine／queue：21 次初始化寄存器写入均读回，禁用上下文切换和字节交换，采用 Polaris10 golden 设置。实际提交 copy／fill／fence 包，处理 ring 环绕，等待 fence 与 RPTR，再逐字核验结果；配置、映射或指针漂移后保持隔离。
- native adapter：真实 volatile BAR 访问、PCI／D0／decode 检查、MC/HDP 地址翻译和缓存同步。额外检查 GMC 系统窗口与 L1 地址模式，避免把“CPU 能看到显存”当成“SDMA 映射正确”；映射漂移不自动放行。
- 桌面接线：持有 framebuffer 锁，验证启动画面与物理资源一致；屏幕外填充和非恒定数据拷贝自检通过后才安装 hook。完整连续行合并上传，局部脏矩形保留 pitch。未知完成返回 `-2`，阻止 CPU 前台写入。

## 文件整理

40 个旧平铺文件移动到 `c/drivers/gpu/amd/`、`tests/gpu/amd/` 和 `tools/gpu/amd/`。Polaris 分为 `firmware/`、`memory/`、`smu/`、`sdma/`，文件名缩短；[paths.json](paths.json) 中没有失踪目标或残留旧路径。历史报告保留当时文件名与哈希。

AMD 测试统一从 `tests/gpu/amd/tests.mk` 接入。Make 可达性、测试清单、链接前置项和负控审计改为递归处理子目录；最终 `test-mk-wired` 为 362 个片段、361 个可达、1 个原有声明例外，没有新增失联门禁。目录移动同时暴露了通用 `io.h` 名称遮挡内核端口 I/O 头的问题，已改名 `platform.h` 并由真实内核构建验证。

## 验证结果

| 范围 | 结果与证据边界 |
|---|---|
| 固件 bundle | 8378 项 ASan/UBSan 检查通过；独立真实文件 oracle 核验全部 9 项目录、payload 和 padding |
| 内存／GART | 76／177 项通过 |
| SMU loader | 94 项通过，包含三种启动和 fresh 状态检查 |
| SDMA queue／engine | 275／177 项通过，CPU/GPU 使用独立存储，模型解释实际 SDMA 包 |
| native adapter | 最终 117 项通过，覆盖系统窗口、地址模式、HDP invalidate 与漂移 |
| 呈现 | 20630 个断言通过；大部分是逐像素／边界内容比较，不是 20630 种功能 |
| 完整 runtime 组合 | 92 项通过；生产 parser、layout、loader、engine、queue、present 全部真实链接，只有硬件边界被模型替代 |
| desktop 接线 | 8 场景共 93 项通过；明确使用 native/runtime 替身，仅证明 hook／锁／stride／错误传播 |
| 旧 AMD 与 framebuffer | bootfb 59、RV100 604、Polaris probe 89、接线 259、旧固件／包／SMU 工具门禁及 fb native 21 均通过 |
| 内核 | 两个隔离 BUILD 的最终 ELF／ISO 均成功构建，实际完成链接；AMD 模块符号可见 |
| QEMU 11 RV100 | 最终 GPU 镜像有 113 次可见区域拷贝；CPU 对照 0 次；最终整帧 SHA256 一致 |

所有新负控都是正向门禁前置项，已经观察到失败。例如破坏 JT2：bundle 2 项失败、完整 runtime 33 项失败；跳过 SMU 最终状态等待：5 项失败；接受迟到 fence、忽略加载状态、缺少 HDP invalidate、跳过 copy canary、错误 stride 换算分别精确触发预期失败。随后正常版本通过，没有取消断言。

[host.log](host.log) 是完整套件结果；末轮增加 GMC 检查后重跑的 native、desktop 和 Make 审计记录在 [final-checks.log](final-checks.log)，其中 native 117 项取代前一日志的 72 项。新 golden 寄存器的三条组合断言曾独立报 3 个失败，对应实现加入后转绿。

QEMU 使用真实 `1002:5159` RV100 设备模型，未伪造 RX 580 身份。它只证明旧 AMD 路径与重构回归。主机模型不执行真实 SMC 机器码，**不构成实体 RX 580 固件执行、画面输出或加速比证据**。当前每次传输保留完整读回，也没有宣称更快。

## 尚未完成的软件与硬件工作

`polaris_desktop_start()` 要求调用方已经拥有 PCI function、完整固件显存保留区记录、独占 arena、真实 UC BAR 映射及正确固件。**默认开机流程尚无自动提供这些资源并调用该入口的实现**：VBIOS 保留区接管、资源所有权建立、板卡修订／密钥对应的固件选择与读取仍需补齐。因此默认 67df 探测继续只读；这项软件缺口不是单纯“未实测”。

GFX8 shader／3D、OpenGL/Vulkan、模式设置、DPM 和硬件复位恢复也未完成。实体卡未到手，实卡验证未运行。可继续开发的具体入口、前提和调用顺序见 [驱动说明](../../../../../c/drivers/gpu/amd/README.md)。

## 复现与绑定

```sh
make BUILD=build/gpu/amd/rx580 test-amd-host test-fb-native-present-host test-mk-wired
make BUILD=build/gpu/amd/rx580 POLARIS_FIRMWARE_DIR=build/gpu/amd/firmware test-polaris-bundle-files
make BUILD=build/gpu/amd/rx580-kernel -j6
make BUILD=build/gpu/amd/rx580-control AMD_PRESENT_DISABLE=1 -j6
python3 tests/boot/run-amd-rv100-present.py \
  --iso build/gpu/amd/rx580-kernel/logit.iso \
  --cpu-iso build/gpu/amd/rx580-control/logit.iso \
  --out reports/gpu/amd/rx580/2026-09-15/regression
```

固件输入固定为 linux-firmware `1522c78ab870b3c051d8a3a1d24ecbbc12b23be5`；哈希与来源在 [firmware.json](firmware.json)，检查器不会自行联网或接受其他哈希。[bundle.json](bundle.json) 是独立文件验证结果；派生二进制只放在 build 目录。

[源码与最终产物哈希](files.json) · [内核构建](build.log) · [CPU 对照构建](control-build.log) · [实文件测试](firmware.log) · [QEMU 结果](regression/result.json) · [QEMU 门禁日志](guest.log)
