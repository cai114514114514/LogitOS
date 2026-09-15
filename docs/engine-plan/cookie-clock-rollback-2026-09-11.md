# Cookie 快照时钟回退修复 — 2026-09-11

真实页面日志的 `[webapi] cookie persistence failed status=-2` 已定位为启动恢复时把正常的时钟回退判成损坏。修复位于 `cookie_persistence.c`；保存文件格式、Cookie 属性校验和双槽写入规则不变。

## 观测与根因

`status=-2` 是持久化层的 `CK_PERSIST_CORRUPT`，不是文件系统 errno。私有 guest adapter 的原始 `read=-2` 会被转换成 `CK_PERSIST_IO=-1`。日志先出现 `Cookie snapshot could not be loaded`，然后三次成功 Cookie mutation 报告相同的锁存状态；它们不代表三次独立的磁盘写失败。

只读检查了未被进程打开的源盘 `build/disk.img`，通过现有 `lfs_snapshot` 生成权限受限的独立检查副本，未读取正在运行的 guest 磁盘。结果：

- 文件系统检查通过，journal 回放 5 块、丢弃 0，和本次 profile 保留时相同。
- `/browser` 和 `/browser/cookies` 是 uid/gid 0、权限 0700；两个 jar 文件权限 0600。
- 两槽均为版本 1、generation 170、32 条记录；尺寸、结构尾位置、CRC 均正确。
- 32 条访问时间都领先默认 UTC 时钟，检查时最大领先约 7.6 小时。原 `ckp_decode` 在 `accessed > now` 时直接返回损坏；9 条记录还会碰到相对于较早时钟的 400 天上限。

原产品启动参数 `Makefile` 的 `QEMU_RTC=-rtc base=localtime`，本次私有 guest 未提供 `-rtc`，使用 QEMU 默认 UTC。`js_webapi.c:d_now_unix()` 将 `SYS_GET_TIME` 的 civil 字段直接解释为 UTC。因此上海时区的两个启动基准相差 8 小时。正常启动配置与 UTC API 语义不一致仍是独立产品问题，本修复没有更改共享内核、全局 RTC 参数或系统显示时区。

同一离线快照、同一实际 decoder/core，只改变输入时钟的对照如下。检查只输出状态和条数，没有输出 Cookie 名称、域名、值或 localStorage 内容。

| 实现与时钟 | status | 恢复条数 |
|---|---:|---:|
| 原实现、默认 UTC | -2 | 0 |
| 原实现、匹配原 localtime | 0 | 28 |
| 修复后、默认 UTC | 0 | 32 |
| 修复后、匹配原 localtime | 0 | 28 |

两种时钟下相差的 4 条由绝对截止时间判定自然清理；修复不使 `expires <= now` 的记录继续存活。权限受限的元数据和结果在 `build-ds-render-fixed/cookie-meta-diagnostic/{metadata.json,clock-current-control.json}`，目录 0700、检查盘和 JSON 为 0600。

## 恢复规则

1. 对整个原始快照检查 CRC、结构、flags、无 NUL 字符串、重复条目、`0 <= created <= accessed < expires`；通过核心 restore API 继续校验域、PSL、prefix、Secure、SameSite、长度与配额。
2. 核心校验以原记录的 `accessed` 为时间锚，先证明原始剩余期限不超过 400 天，不能先 clamp 再把不合法的存储记录洗成合法。
3. 整份快照验证后，如最新访问时间超过当前时间，用同一个差值平移创建/访问元数据；接近 Unix epoch 时下限为 0，并保留原创建顺序作为归零后的稳定排序。
4. `expires` 保留绝对含义，只取原值与饱和计算的该记录回拨后 `accessed + 400 days` 中较小值。它不会增加，也不会按“剩余寿命”重新起算。GC 继续按当前 `now` 执行。此逐条上限修正了下述首次实现的再次写回缺陷。

共同平移元数据还防止下一次请求把 `accessed` 写成较早的当前时间，却留下未来 `created`，导致下次重启再次失败。正常前进的时钟保留原元数据；负 Unix 时间作为明确的 IO/时钟失败处理。

## 验证

永久门禁 `tests/cookie_clock_rollback.mk` 已接 Makefile 与 `ci-host`：

```sh
make BUILD=build-cookie-clock-fix test-cookie-clock-rollback-san
make BUILD=build-cookie-clock-fix test-cookie-persistence
make BUILD=build-cookie-clock-fix test-mk-wired
```

- 47 项正向及 ASan/UBSan：全部通过。包含 8 小时回退、epoch/INT64_MAX 边界、实际 Cookie header 顺序、后续持久化再打开、原截止时间清理，以及原始非法时间/期限/CRC/flags/prefix 拒绝。
- 原 strict 时钟规则作为前置控制，精确 13 项失败；非法记录拒绝检查继续通过。
- 现有持久化 59 项、跨进程实际文件读写及 guest adapter 门禁通过，既有 no-write/prefix-read 负控制触发。
- shipping `js_webapi.o`（包含持久化实现）交叉编译通过。
- `test-mk-wired`：276 fragments、275 reachable、1 declared，通过。

日志：`build-cookie-clock-fix/final-acceptance.log`、`persistence-regression.log`、`mk-wired.log`。没有为本修复启动或操作 VM，也没有新增任何 DS 请求；真实聊天失败的 business/script 调查仍由主任务分别记录。

## 后续重启发现的写回回归

在首答调试的下一次真实启动中，私有测试 profile 再次报告恢复失败。离线检查之前关闭的测试盘：两个槽均为 generation 171、32 条、3685 字节，CRC 与结构正确，但 7 条记录超过其自身 `accessed + 400 days`；严格 decoder 返回 -2。仅在权限受限目录 `build-ds-first-answer/cookie-check/` 保存元数据，未输出 Cookie 内容。默认用户盘没有被本轮检查或修复写入。

首次实现平移所有记录的访问时间，却统一按 `now + 400 days` 截断截止时间。较旧且之后未访问的 Cookie 因而可能在下次保存后违反 decoder 的逐条期限约束。原回归在写回前调用了 Cookie header，恰好刷新了访问时间，未覆盖这个场景。此前“恢复成功”的 guest 证据只证明当次恢复，不能证明之后每次写回重启都成功。

现将上限改为每条记录平移后的访问时间加 400 天，保持原始记录校验及绝对到期清理。新增 `test-cookie-clock-rewrite-san` 直接对不同访问年龄、不同路径的记录保存再打开，中间不读取 header。8 项正向与 ASan/UBSan 通过；旧统一上限控制精确出现 2 项失败；原 47 项回拨及 sanitizer 回归继续通过。证据为 `build-ds-first-answer/cookie-rewrite.log`。

这个修复防止新的非法写回，并不放松校验来接受已被旧实现写坏的时间元数据。本轮新代码尚待下一份 guest 二进制与实际持久化重启验证。


后续实际持久化检查：使用上述逐条上限修复的 `snapshot-worker` 浏览器关闭后，检查并复制 `attempt-02` 私有盘。其两槽均为 generation 171，严格 decoder 返回 0，28 条未过期记录成功恢复；`attempt-03` 原样保留全部 9 个浏览器状态 inode，未再次使用备份恢复。证据 `build-ds-first-answer/attempt-03/profile-result.json` 与 `profile-check.log`。这证明该实际 guest 写回可再打开；特定 8 小时时钟回拨且存在未访问旧记录的组合仍以混合年龄持久化门禁为证据。
