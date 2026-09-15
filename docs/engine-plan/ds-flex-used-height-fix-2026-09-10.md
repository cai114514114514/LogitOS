# Flex used height、侧栏滚动与原生按钮内容框修复

2026-09-10。本页接续 `ds-cross-stretch-audit-2026-09-10.md` 的只读审计。原报告“未改布局算法”描述的是当时的调查阶段；用户随后授权修复，本页记录实际产品修改和验收。没有 DS 域名、class 或页面专用布局分支。

## 产品修改

`layout_flex.c` 在真实的 cross-axis stretch 分支给结果标记 `cross_stretched`，定义在 `layout_flex.h`。`layout.c:flex_row_spec` 消费它，通过现有 column 使用的 `flex_place_impl` used-height 临时作用域，在子内容布局前传入确定高度。完成后恢复 computed auto height。未拉伸的 flex-start、auto margin 项不会被强行变成确定高度。

旧 row bridge 只在子布局完成后扩大外框：562px 外层里留下 14px 列与 0px 绝对定位 clip。新路径在建立绝对定位 containing block、百分比子高度和 overflow clip 前使用最终 stretch height。该行为来自 [CSS Flexbox §9.4](https://www.w3.org/TR/css-flexbox-1/#algo-stretch) 的内容重新布局要求。

同一修复使 row 中的 sidebar column 得到真实 562px used height，随后现有 column solver 为 60px header、44px footer 与中间滚动区分配 458px。滚动区内 920px 内容保留，footer 不再被内容挤到视口下面。没有调整站点间距常量。

`layout.c:button_children` 合并两个原生按钮子内容路径，按 computed border/padding 计算内容框，替代固定左右 6px、上下 4px。padding 百分比先按外部 containing width 求值。`css_engine.c` 的 UA button padding 从共享 `forms.h` 的 `FC_PAD_*` 常量生成，默认外观保留且 `padding:0;border:0` 可真正覆盖。已有长标签不按字符折行的兼容下限保留；本次没有重做通用断词算法。

未改动 layout 的 stacking/SVG 区域、被动文档 context 生命周期、TopLayer 与 positioned-inset 的 used-height 解析。并行 stacking/SVG 工作分别验收。

## Host 正反回归

新增两项 `tests/*.mk` 已接 Makefile 和 `ci-host`，负控制是正向的前置条件；测试使用 shipping DOM、CSS 与 layout，Flex gate 还链接真实 browser painter。

| Gate | 当前 | 旧行为控制 |
|---|---:|---:|
| `test-flex-used-height` | 75 checks，0 failures | 74 checks，18 failures |
| `test-button-content-box` | 65 checks，0 failures | 65 checks，15 failures |

Flex 三种 fixture（原 overflow hidden、只去 clip、只补 explicit height）现在均得到 562px column/absolute child，保留且实际 paint 一份文字。还覆盖 sidebar/footer/clip、min/max、零高度、百分比、padding/border、row-reverse、非 stretch、auto margin 与重复 layout 后 computed auto 恢复。

按钮覆盖 inline、block、inline-block、flex/grid 直接子项；24px/12px 的零 padding 内容框、非对称 border/padding、百分比 padding。旧控制确实产生 24→12、12→1 的内容宽度错误。

相关回归通过：纯 Flex 176 checks、column bridge 59、percentage height 41、positioned insets 70、positioned projection 18、inline-flex 36、被动 layout/CSS context 67（含 ASan/UBSan）、原生 container/control chrome、modal paint 与 modal runtime 14。新 stacking 使旧 paint-only modal 负控制失去原有失败路径，其所有者已将该控制与旧 flat-layout 顺序组合，保留精确失败断言后整项通过。

日志：

- `build-ds-flex-fix/{primary-gates,primary-final,related-gates,remaining-gates-final}.log`
- 两个新 gate 目录中的 `before.log` / `legacy.log` 保留修前及负控制结果。
- `inline-current.log` / `inline-before.log` 均为当前源码重新编译后 36/36；初轮共享施工窗口的旧产物失败未被当成已确认产品回归。

## 独立真实 guest

只构建 `make BUILD=build-ds-flex-fix build-ds-flex-fix/browser.aex`。使用已冻结的 `build-terms-layout-evidence/snapshot-final/logit.iso`，对其磁盘副本运行 shipping fsck/journal snapshot helper，再仅替换 browser.aex 生成私有镜像。没有覆盖默认 disk、导航或关闭已登录用户 VM。

`build-ds-flex-fix/flex_guest.py` 启动独立 QEMU：1280×800、1GiB、`-snapshot`、独立 QMP socket、本地通用 HTTP fixture。第一次截图前脚本不读取几何，避免 forced layout 掩盖初始问题。

手工检查截图：

- `build-ds-flex-fix/guest/cross.png`：三列均显示 VISIBLE 和 INPUT，欢迎区高度一致；旧截图左列完全空白。
- `build-ds-flex-fix/guest/sidebar-before.png`：ROW 01 起始内容与 ACCOUNT footer 同时显示。
- `build-ds-flex-fix/guest/sidebar-after.png`：18 次真实鼠标滚轮后显示 ROW 16–30/LAST ROW，header 与 ACCOUNT footer 留在原位置。原生滚动事件观测 `scrollTop=462`、`clientHeight=458`、`scrollHeight=920`；没有调用 JS scroll setter。
- 同一 sidebar 页面中的 24px/12px 零 padding 原生按钮分别显示完整宽度的绿色子元素。

`guest/results.json` 保存数值与私有 PID 33977；运行已正常结束且其进程已终止。冻结输入 SHA256 运行前后相同：

```
ISO     89466926e7e34e117f538e08fd5772cc18eab02eda4ed78b1d93c96f4e00c58b
disk    0337d9064fac30fffc52b57c4d1742ccd2aef95906d0622be9d4bbb284911891
browser 25ea3becc26dfce98bff73660e0adce0e037b13bd26890f8a2c1fcefa4e0c4ae
```

验收证明通用机制在真实 guest 修复，不等于 DS 已登录页面所有状态已验收。用户现场最终截图与并行 SVG、stacking 的完整镜像集成由主任务继续。原协议中文叠绘在此前当前镜像中未复现，本次不声称修过一个未经证实的协议行高问题。
