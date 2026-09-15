# Stacking context 修复与验收 — 2026-09-10

`parent z-index:8 / child z-index:1` 的文字被父背景覆盖，已在通用 display-list 排序处修复。独立真实 guest 同页比较显示，文字区域由 0 个深色像素恢复为 441 个，与正常控制区完全一致；真实鼠标也命中可见文字所属子元素。

## 原因与实现

[此前审计](ds-render-audit-2026-09-10.md) 的结论保留为修复前证据：旧 `zsort()` 用一个继承的整数全局排序，使较小子 z-index 落到父背景之前，也允许嵌套高 z-index 越过父层。旧注释提出固定四层路径；新实现不采用固定深度假设。

`layout_stacking.inc` 在完成布局后建立临时、按实际格式树组织的 paint group/context 树，迭代生成最后的 item 顺序。背景、负层、普通块、浮动、行内、定位 auto/0、正层按所在上下文组织；定位 auto 和临时浮动/行内组中的真正子上下文仍归属最近的真正上下文。Flex/grid 使用 order 修改后的子树顺序。

真正上下文包含根元素、合法定位或 Flex/grid item 的非 auto z-index、fixed/sticky、opacity 小于 1、有效非 none transform 及原生 top layer。新增 `cstyle.opacity_context` 保留 `.999` 的语义，避免 8 位透明度四舍五入成 255 后丢失上下文。

树、哈希、事件排序和输出缓冲均在单次布局末尾释放，独立子文档上下文不会持有这些暂存数据。列表标号的 `marker[]` 自引用在排序拷贝中重定位；分配失败会明确撤销该次 display list，避免静默退回错误全局排序。正向 painter 和反向 hit-test 继续使用同一个完成排序的列表，原生 modal/popover 的单独绘制与输入规则保留。

语义依据：[CSS 2.2 Appendix E](https://www.w3.org/TR/CSS22/zindex.html)、[Flexbox painting order](https://www.w3.org/TR/css-flexbox-1/#painting)、[CSS Transforms rendering model](https://www.w3.org/TR/css-transforms-1/#transform-rendering)。这些依据对应排序与上下文边界；此项验收不等同于完整 CSS 绘图兼容性测试。

## 永久主机门禁

`tests/layout_stacking.mk` 已从 Makefile 接线，`ci-host` 包含正向目标。命令：

```sh
make BUILD=build-ds-stack-fix test-layout-stacking-san
make BUILD=build-ds-stack-fix test-modal-top-layer
make BUILD=build-ds-stack-fix test-paint-gfx test-opacity-group test-passive-layout-context test-popover-top-layer
make BUILD=build-ds-stack-fix test-mk-wired
```

- `test-layout-stacking`：46 检查，0 失败；旧 flat 排序为强制前置控制，精确 24 项失败，包括父背景覆盖、跨父层、负层、Flex/grid item 和 80 层上下文。
- 同一 46 项 ASan/UBSan：0 失败，无仪器错误。Darwin 未启用 leak sanitizer。
- Modal：原三个独立负控制触发；正向 paint 及 runtime 14 项通过。新 layout 本身已有 top-layer 隔离，因此仅 ordinary-paint 负控制同时启用旧 flat 排序，原“modal 高于最大 page z-index”断言保持不变。
- Popover paint、runtime 28 项及 browser gate 通过；opacity 12 项、passive layout context 67 项、paint-gfx 全部通过，既有负控制均触发。
- `test-mk-wired`：272 fragments，271 reachable，1 declared，通过。先前其他并行任务新增 fragment 的未接线状态已经改变。

最终日志：`build-ds-stack-fix/stacking-acceptance.log`、`stacking-final.log`、`stacking-related.log`、`mk-wired.log`。

最初测试器用普通 inline span 复现父/子 z，但旧布局不为该 inline 路径记录子 z，无法检测审计中的故障；改为审计中的 block 子容器后，原排序明确触发两项父背景/命中失败。此为 fixture 校正，产品实现没有为了保住控制而改变。

## 真实 guest 像素与鼠标证据

`tests/qmp/layout_stacking_guest.py` 用真实 parser/CSS/layout/painter、QEMU framebuffer 和原生鼠标。只访问本机自有 HTTP fixture，无站点脚本替换，无真实账户操作。新旧均只请求 `/stack` 一次，fixture SHA-256 完全相同：`75f39846272a3dd0e729ba231521dd5ea04c86a0d4ffead7999f0791264f67ce`。

| 观测 | 旧 flat 控制 | 新上下文树 |
|---|---:|---:|
| parent 8 / child 1 文字深色像素 | 0 | 441 |
| 同页 child auto 文字深色像素 | 441 | 441 |
| 外层前景绿色像素 | 0 | 3600 |
| 错误越过父层的红色像素 | 3600 | 0 |
| 第一次原生点击 | 父容器 `bad` | 可见子元素 `badchild` |
| 第二次原生点击 | 越界的 `trapped` | 正确前景 `front` |

旧截图：[guest-old/page.png](../../build-ds-stack-fix/layout-stacking/guest-old/page.png)。新截图：[guest-current/page.png](../../build-ds-stack-fix/layout-stacking/guest-current/page.png)。每个目录包含 `results.json`、fixture、serial、launch 参数。主日志为 `build-ds-stack-fix/guest-gate.log`。

```sh
make BUILD=build-ds-stack-fix test-layout-stacking-guest \
  STACKING_GUEST_ISO=build-terms-layout-evidence/snapshot-final/logit.iso \
  STACKING_GUEST_OLD_DISK=build-ds-stack-fix/guest/old-disk.img \
  STACKING_GUEST_DISK=build-ds-stack-fix/guest/current-disk.img
```

两个磁盘均由已授权、无浏览器 profile 的独立 system 副本新建，QEMU 用 `-snapshot`；脚本只结束自己创建的进程。没有改动 `build/disk.img`、默认 QEMU 或用户已登录现场。它验证的是审计根因对应的通用页面机制；DS 登录后页面的最终综合验收由主任务另行记录。

冻结证据哈希见 `build-ds-stack-fix/guest/hashes.json`：

| 文件 | SHA-256 |
|---|---|
| 当时新 browser.aex | `23043c2ad4a16afa4676966e44a3c2b3755dc23ada63affb86cc6487764b7c0a` |
| 新 private disk | `83946ff4a8d94eb14344324f3ef938d0e43fb783225c18b9f4a9b69e268af994` |
| 当时旧排序 browser.aex | `642db1513052c678bdadef025d8227c9faf75cc7e2a0e182b4d2a748129d5734` |
| 旧 private disk | `b56481b39c2311800094e2cecb486c099af9e90f7c20416572596fe713604254` |

镜像是上述 guest 验收时的冻结产物；后续并行 SVG 修订可使工作 BUILD 的 browser.aex 再次变化，不能用新文件名推断它就是已观测的那个二进制。
