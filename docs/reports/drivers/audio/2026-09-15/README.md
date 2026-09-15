# 声卡驱动扩充：AC’97 与 ES1370

本轮新增两类真正接入播放链路的声卡驱动；加上原有 HDA，现在共有三类播放驱动。新增代码按控制器放入独立目录，应用继续使用现有 sound syscall 和混音器。

| 驱动 | 本轮范围 | 输出 |
| --- | --- | --- |
| [Intel ICH AC’97](../../../../c/drivers/audio/ac97/README.md) | 新增，PCI `8086:2415` | 48,000 Hz、S16、双声道 |
| [Ensoniq AudioPCI ES1370](../../../../c/drivers/audio/es1370/README.md) | 新增，PCI `1274:5000`，DAC2 / AK4531 | 48,662 Hz、S16、双声道；混音器转换 48 kHz 输入 |
| HDA | 保留原有驱动并回归播放 | 48,000 Hz、S16、双声道 |

两类新驱动均包含 PCI 资源检查、DMA32 播放环、codec 初始化、中断确认、合并中断进度处理和停止失败时的内存保留。AC’97 分开控制器、PCI 集成和原生 I/O；ES1370 将寄存器定义与驱动实现分开。错误路径会阻止未确认停止的设备复用旧 DMA 内存。

## 最终镜像的实际音频结果

三个独立 guest 使用同一份 ISO，运行原有 ring-3 `sndtest ramp 1000`，再检查 QEMU 实际写出的 PCM。不是仅凭发现设备或 syscall 返回成功验收。

| QEMU 设备 | 有效信号帧数 | 测得频率 | 结果与录音 |
| --- | ---: | ---: | --- |
| intel-hda / hda-output | 48,000 | 200.000000 Hz | [PASS](hda/result.json) · [录音](hda/playable.wav) |
| AC97 | 48,000 | 200.000000 Hz | [PASS](ac97/result.json) · [录音](ac97/playable.wav) |
| ES1370 | 48,662 | 199.998622 Hz | [PASS](es1370/result.json) · [录音](es1370/playable.wav) |

三项均检查一秒信号长度、幅度、上升斜率、频率和左右反相声道。HDA 与 AC’97 还通过原有逐样本顺序检查；ES1370 使用独立的重采样波形检查。测试音具有周期性，不能据此证明相同完整周期之间的字节来源；详见 [guest 检查说明](../../../../tests/drivers/audio/guest/README.md)。

[冻结的已测试 ISO](../../../../build/drivers/audio/final-hda/input.iso) SHA-256：

`f61ccff42352e9f7cb3060fa774c58a3f82c1eda498ba66a8d8a5e3167a58c5f`

使用 QEMU 11.0.0 / TCG。每次启动采用磁盘快照，输入磁盘哈希在测试前后保持一致；没有重建或写入共享 `build/disk.img`。本次只改内核驱动，使用磁盘中已有的 `sndtest`。

本机 QEMU 正常 QMP 退出后仍留下全零 RIFF/data 长度字段。原始 `capture.wav` 完整保留；`playable.wav` 仅修正两个容器长度字段，PCM 字节完全相同。结果明确记录 `header_finalized: false`，没有把音频样本修补后当作驱动输出。

## 自动检查与失败对照

[统一 host 日志](host.log) 包含以下成功结果，所有负控均为正例目标的前置条件：

- 原有 PCM：71 项通过；移除饱和裁剪后实际出现 2 项预期失败。
- 原有 HDA：36 项通过；8 个故障变体分别命中指定失败。
- AC’97：10,667 项和 5,678 项通过，启用 ASan/UBSan；错误描述符长度触发 102 项失败，移除末尾 CIV 修正触发 2 项失败。隔离分配有意保留至进程退出，因此该 fixture 关闭 leak 检查。
- ES1370：131,460 项通过，启用 ASan/UBSan，包含跨环 PCM 字节检查；移除 period 通知后触发 4 项指定失败。检查数量包含逐字节断言，不代表同等数量的独立场景。
- PCM 检查器：两种采样率共 26 个正反例，包含静音、错误速度、声道交换、缺失或重复 period，以及损坏 WAV。
- [无声卡 / 未支持声卡回归](no-device.log)：均完成启动、返回 `SNDTEST_NODEV_OK`，查询之后 Shell 仍可用。未支持设备改用 virtio-sound，AC’97 已不再适合作为这个对照。

另用冻结的旧镜像重跑两张新卡：[AC’97](negative-controls/ac97-before.json)、[ES1370](negative-controls/es1370-before.json) 均实际返回 `SNDTEST_NODEV_OK` 并被音频验收拒绝。失败不是缺少 QEMU 或未成功启动造成的。

构建时发现其他并行工作新增的 AS 私有 `runtime/file.h` 遮蔽了内核同名头文件，因此将 AS 子目录移出全局扁平头文件搜索路径，保留 AS 根路径与相对引用。[内核构建](kernel-build.log)和独立的 [AS 应用构建](as-build.log)均通过；没有改 AS 源文件或安装该测试产物。

## 复现

```sh
make BUILD=build/drivers/audio/verify test-audio-cards-host test-audio-cards-oracle
make -j4 BUILD=build/drivers/audio/kernel all
python3 tests/drivers/audio/guest.py \
  --iso build/drivers/audio/kernel/logit.iso --disk build/disk.img \
  --device AC97 --output build/drivers/audio/new-ac97-run
```

`--output` 必须是新目录；其他设备分别使用 `intel-hda`、`ES1370`。`test-audio-cards-os` 提供包含独立磁盘构建的完整入口，并为每轮录音生成新目录。当前验证的源码、ISO 和磁盘身份见 [source-manifest.json](source-manifest.json)。

## 支持边界

这是模拟设备上已验证的播放支持，尚未验证实体声卡。AC’97 没有笼统声明覆盖所有厂商；ES1371 / CT5880 不在 ES1370 匹配表中。两张新卡没有录音支持。

当前混音器选择首个成功注册的输出设备，尚无多输出选择界面或同时输出能力。共享混音器已有的 engine-start 状态没有在解绑时复位，因此不声明支持播放后的热拔插重绑。USB Audio 和 VirtIO Sound 也不属于本轮新增驱动。
