# AMD RV100 运行期桌面呈现 — 2026-09-15

本轮从启动时的显存自检推进到真实桌面刷新。三个代理分别实现运行期命令核心、帧缓冲集成测试、QEMU 对照验收；主线完成驱动接线、并发互斥、构建及整机验证。

## 实现

- `rv100_present` 接收 CPU 后缓冲中的脏矩形：普通图像上传到独立屏幕外显存，再由 GPU 拷贝至 scanout；纯色矩形使用 GPU fill。
- 首次实际呈现逐像素读回验证；随后等待引擎完成及缓存清理，累计完成次数、像素和上传字节。成功后跳过 CPU 对显示区域的重复拷贝。
- 呈现与驱动重置共用 graphics mutex；排空 AP framebuffer 写租约后才允许 GPU 提交。可确认未提交的失败允许 CPU 回退；无法确认 GPU 停止的超时进入持久隔离，禁止 CPU 写显示显存，防止损坏图像。尚无硬件复位恢复。
- `AMD_PRESENT_DISABLE=1` 专门生成 CPU 对照版本，保留同一显卡和启动 fill/copy 自检，仅关闭运行期接线。需独立 BUILD，避免 Make 不跟踪 CFLAGS 的问题。
- 修正产品 BIOS 的 ABI 常量生成器：原规则漏掉新版启动协议字段偏移；现从权威头文件提取数值常量、排除函数宏，并在 Makefile 更新后重新生成。两份 ISO 使用本轮独立构建目录中的当前源码产物，没有复制旧 bootinfo 对象。

## 实测

QEMU 11.0.0 / TCG，真实模拟设备 `ati-vga,model=rv100`，PCI `1002:5159`，BIOS，1024×768×32，2 个 vCPU。没有挂载应用磁盘；截图是内核桌面、菜单栏、dock 和软件指针。

| 检查 | 结果 |
|---|---|
| RV100 核心 ASan/UBSan | 604 检查，0 失败 |
| 真实 fb.c 集成 ASan/UBSan | 21 检查，0 失败 |
| AMD / Intel bootfb 回归 | 59 / 31 检查，0 失败 |
| 变异负控 | 错误现代 GPU 放行、越界、上传像素损坏、CPU 覆盖 GPU 结果均被捕获 |
| 缩放回归 / Make 接线 | 位精确 oracle 通过 / test-mk-wired 通过 |
| 显示区域 GPU copy | 112 条，含 1024×768 首帧及软件指针脏矩形 |
| 最后周期性完成日志 | completed=64，commands=66，verified=1；日志只在 2 的幂次输出，因此少于完整 trace 命令数 |
| 首帧显存读回 | 通过 |
| CPU 对照 | 同样启动自检通过；运行期显示区域 GPU 命令为 0；被正向断言以 runtime-present-missing 拒绝 |
| 画面一致性 | 最终整幅 PPM 的 SHA256 相同；内部采样 104960/104960 相同 |
| 其他 AMD 兼容显示回归 | QEMU Rage128 BIOS bootfb 通过 |

纯色 fill 路径通过核心测试；本轮桌面序列实际产生 112 次 copy、0 次运行期 fill，因此不声称已在 guest 中覆盖运行期 fill。输入在 guest 中有坐标日志；刷新也包含菜单时钟，不把每条命令归因于单次鼠标输入。测试超时时间和其他缩放测试的耗时均不是本轮加速比。

## 复现

```sh
make -j4 BUILD=build-amd-present-live all
make -j4 BUILD=build-amd-present-cpu AMD_PRESENT_DISABLE=1 all
make BUILD=build-amd-present-live AMD_PRESENT_CPU_ISO=build-amd-present-cpu/logit.iso test-amd-present-control
make BUILD=build-amd-present-live test-amd-bootfb-host test-intel-bootfb-host test-fb-scale-bl test-mk-wired
python3 tests/boot/run-amd-bootfb.py --iso build-amd-present-live/logit.iso --firmware bios --out build-amd-present-live/amd-bootfb-regression
```

上方命令在仓库根目录执行。重做验收会更新 build 下的日志；本目录 runtime 是本次证据快照。首次整机尝试和之后 CPU 对照均通过；最终 build 日志只记录最后增量构建，不作为完整编译清单。

## 产物与边界

- 加速 ISO：[logit.iso](../../build-amd-present-live/logit.iso)
- 原始对照结果、命令、设备身份、trace 和串口：[runtime/result.json](runtime/result.json)
- 源码快照和内核/ISO/关键对象哈希：[source-and-artifacts.json](source-and-artifacts.json)
- 最终桌面：[desktop.png](desktop.png)

目前只验证 QEMU RV100 的同步 2D 显示搬运。CPU 仍负责界面光栅化以及普通图像上传；未测量或证明净加速、浏览器性能、动画帧率、着色器/3D 合成、现代 GCN/Polaris/Vega/RDNA 加速、UEFI 运行期加速或实体 AMD 显卡。现代显卡仍保留兼容帧缓冲，需要另行实现其固件、GPUVM、命令队列、完成通知及复位路径。
