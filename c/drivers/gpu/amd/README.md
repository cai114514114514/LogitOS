# AMD 驱动

代码按家族与硬件功能组织。旧的 `amd_*`、`polaris_*` 平铺文件已移动，公开 C 符号与 Make 目标保持兼容；历史报告保存当时的文件名和源码哈希。

```
amd/
  accel.c                 自动探测与 RV100 桌面接线
  bootfb.c                已有启动画面的 AMD PCI 归属验证
  rv100/engine.c          RV100 2D 引擎
  polaris/
    device.c              67df 的只读探测
    platform.h            设备访问与映射接口
    native.c              真实 BAR/PCI/MC/HDP 适配
    runtime.c             完整启动与呈现调用链
    desktop.c             framebuffer 锁、启动画面核对、present hook
    present.c             屏幕外自检、脏矩形上传
    firmware.c            SDMA 固件格式解析
    firmware/bundle.c     完整 normal-PF 固件目录与 RAM staging
    memory/layout.c       显存保留区与控制对象布局
    memory/gart.c         GMC8 页表编码
    smu/loader.c          SMU 启动、微码加载与完成等待
    smu/mailbox.c         独立 Test 消息传输检查
    smu/toc.c             SMU7 目录编码
    smu/stage.c           两份 SDMA 文件的独立 staging 工具接口
    sdma/packet.c         SDMA3 包编码
    sdma/engine.c         队列硬件配置
    sdma/queue.c          提交、fence、读回与失败隔离
```

## RX 580 / Polaris10 的调用路径

`polaris_desktop_start(resources, request)` 在 framebuffer 图形锁内核对当前 GOP/Multiboot 显示区域，调用真实 native adapter，再运行 `polaris_runtime_start()`：

1. 解析 7 份引擎固件和单独的 SMC 文件，计算 9 项 TOC 与真实显存需求。
2. 在调用方明确拥有的显存 arena 内放置 TOC、SMU scratch、固件、ring、fence、staging；拒绝 scanout、保留区、CPU 别名和源文件覆盖。
3. 通过映射回调核对 MC 地址到 CPU BAR 地址的实际翻译，上传固件并完成 HDP 可见性同步。
4. 验证运行中的 SMU，或执行普通／受保护冷启动；提交完整加载请求，等待新 ACK 和 SRAM 中的 `0x47e` 完成状态。
5. 配置并读回 SDMA0 ring，以填充和非恒定数据拷贝分别验证执行和结果；自检只写屏幕外 staging。
6. 自检完成后安装桌面 hook。全宽连续脏行合并上传，局部区域保留 pitch；每次提交都等待 fence、RPTR 和目标内容读回。

任意不确定提交会保留所有 DMA 内存并锁存隔离状态，桌面返回 `-2` 阻止 CPU 写前台；软件重新初始化不能充当硬件复位。

## 平台前提与当前边界

平台必须先拥有该 PCI function，提供完整的显存保留区记录、确实独占的 arena、UC BAR 映射以及匹配芯片／修订／安全密钥的固件。`native_bind` 检查实际 PCI 身份、D0、memory/busmaster、BAR 和 MC/HDP 状态；资源描述本身不等于取得这些资源。实现保持固件已有的平面 VRAM/display 映射，拒绝无法确认的布局。

**当前自动开机探测还没有这样的资源提供方**：尚未实现从板卡 VBIOS 自动取得完整保留区、建立所有权并选择／读取固件后调用上述入口。因此默认 67df 启动路径仍只读，不能宣称插卡开机即可自动加速。新增入口与各模块均链接到内核，调用方具备前提时可以执行上述硬件协议；组合测试直接调用完整入口。此处的软件缺口与“尚无实卡验证”是两件事。

本实现是线性 SDMA 拷贝／填充与呈现，不包括 GFX8 shader、OpenGL/Vulkan、模式设置、DPM、GPU reset/recovery。GART 模块可以构造页表，当前 VRAM 呈现路径不启用系统内存 GART。PCI `1002:67df` 覆盖多种 Polaris 板卡，不能据此断言一定是某一款 RX 580 或其显存容量。

当前每次传输保留完整读回验证，未宣称它比 CPU 呈现更快。主机模型验证协议、错误处理和结果，不能证明真实 SMU 指令执行、实体卡画面或性能。

## 验证

嵌套测试在 `tests/gpu/amd/`，工具在 `tools/gpu/amd/`。`test-driver-host` 与 `ci-host` 包含新门禁，每项负向测试都是正向测试的前置条件。递归 Make 审计同时检查子目录，避免搬完文件后测试失联。

```sh
make BUILD=build/gpu/amd/check test-polaris-runtime-host test-polaris-present-host \
  test-polaris-native-host test-polaris-desktop-host test-mk-wired
make BUILD=build/gpu/amd/check POLARIS_FIRMWARE_DIR=build/gpu/amd/firmware \
  test-polaris-bundle-files
make BUILD=build/gpu/amd/kernel -j6
```

固件文件测试要求官方固定提交及 SHA256，独立 Python 检查全部目录、payload 和填充字节。模型源只出现在测试目标中，不参与内核链接。QEMU RV100 回归只用于旧 AMD 路径，不能作为 Polaris 硬件证据。

实现依据固定 [Linux v6.12 SDMA](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c)、[Polaris SMU](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/powerplay/smumgr/polaris10_smumgr.c)、[SMU7 微码加载](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/pm/powerplay/smumgr/smu7_smumgr.c)及[GMC8](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c)；各模块保留具体寄存器与格式来源。
