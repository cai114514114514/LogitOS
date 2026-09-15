# 驱动代码可读性整理

本轮针对新增代码的阅读障碍做了实际重构：按职责拆模块、展开多语句行、使用具名状态与协议常量，并把缓冲区所有权和失败路径写在对应代码旁。没有用全仓格式化覆盖其他任务的改动。

## 从哪里读

| 范围 | 入口与组织方式 |
| --- | --- |
| Wi-Fi | `c/drivers/net/wifi/README.md` 导航；生命周期、扫描、关联、握手、密钥派生、数据收发各有模块。 |
| AMD SMU | `c/drivers/gpu/amd/polaris/smu/loader.c` 分开映射检查、SMU 启动与固件发布；寄存器操作按执行顺序展开。 |
| USB HID | `c/drivers/usb/hid/generic.c` 分开报告解码、跨报告合并、触点生命周期与手势累计；`usb_hid.c` 负责输入事件发送。 |
| NVMe | `c/drivers/block/nvme.c` 分开命令准备、发布、DMA 回收与局部扇区完成；`NVME_PARTIAL_READING/WRITING` 说明异步状态。 |
| ACPI 电源 | `c/drivers/power/acpi/devices.c` 负责 AML 设备查询；`native.c` 负责内核服务；`c/kernel/init/power.c` 负责系统关机与重启流程。 |

NVMe 的局部写入保留“读原生块 → 合并调用者字节 → 写回原生块 → 更新完成计数”的顺序，整个过程受块层的介质请求锁保护。新的测试逐字节检查相邻扇区，同时覆盖第二阶段失败和超时。

电源整理同时补齐了原草稿缺失的工作队列和 SCI 接口，并接到实际关机、重启和状态查询路径。uACPI 放在 `third_party/uacpi`，只有解释器及其适配层使用专属头文件搜索路径，避免同名头文件污染内核。

## 已运行的验证

- Wi-Fi：148 项 ASan/UBSan 检查通过；MIC 负控产生预期 4 项失败。
- USB HID：67 项新检查及旧输入回归通过；触点负控仍产生预期失败。
- AMD SMU：mailbox 141、TOC 186、stage 105、loader 94、runtime 92 项检查通过，负控通过。
- NVMe：44 项 DMA/异步检查通过；AHCI 既有 13 项回归通过。错误总线地址、错误停止处理、破坏邻接扇区的负控均被检测到。
- ACPI：使用完整 uACPI 解释器执行测试固件 AML，8 个场景、162 项检查通过；错误温度换算负控产生预期 3 项失败。测试接线检查通过（366 个片段，365 个可达，1 个已声明例外）。
- 完整内核链接和 ISO 构建通过。产物 SHA-256 记录在 `artifacts.json`。
- QEMU 原生 4Kn NVMe：日志确认 `lba=4096`，文件修改、同步和逐字节校验通过，第二次冷启动保持一致。
- QEMU USB：关闭 PS/2 后，键盘和平板输入经过实际 xHCI/HID 路径通过。
- QEMU 电源：uACPI 加载实际固件 AML 后进入 `_S5`，QEMU 自行退出；文件数据保持且无日志重放；重启在同一进程中产生第二次启动记录。

根目录下各 `.log` 保存对应验证输出；电源三次启动的原始串口记录为 `power-b1.log`、`power-b2.log`、`power-b3.log`。NVMe 原生 4Kn 逐次启动记录位于 `build/drivers/readability/storage-hardware/native-4kn/`；USB 记录位于 `build/drivers/readability/usb-tablet-evidence/`。

## 尚未覆盖

Wi-Fi 的 Intel 实卡传输、固件启动和系统网络接口接入仍未完成。RX 580 尚无实卡验证，自动资源获取及完整图形加速仍有缺口。电源按需初始化，尚未接入开机自动采样、挂起恢复和 CPU 电源状态管理；依赖 EC/SMBus OperationRegion 的电池方法仍不支持。USB 实体触控板、手柄以及 NVMe 实体控制器仍需真机验证。

上述检查证明本轮实现与整理的对应范围；整个五条驱动路线仍在推进。
