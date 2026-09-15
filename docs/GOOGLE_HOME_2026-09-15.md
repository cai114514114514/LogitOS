# Google 首页驱动的通用布局修复（2026-09-15）

## 范围与验收

用真实 `https://www.google.com/` 返回的首页定位浏览器缺陷。本站在此次网络环境中重定向至 `www.google.com.hk`，为 LogitOS 的正常 User-Agent 返回旧式表格首页。

验收项目：标志比例和居中、输入框与相邻链接不重叠、按钮同行、物理键盘输入、回车产生实际搜索导航。产品代码不判断站点、URL 或网页类名；旧页面只是样本。

## 修改及原因

| 原来的表现 | 通用原因 | 修改 |
| --- | --- | --- |
| 标志和页脚靠左 | UA 样式没有 `center`；HTML 对齐属性未进入级联 | 补 block/inline 对齐默认值，将适用元素的 `align`、单元格 `nowrap`、表格与单元格 `width` 作为低优先级 HTML 样式提示交给 LibCSS，允许作者 CSS 覆盖 |
| 512px 搜索框落入 131px 单元格，高级搜索压进框中 | 列宽只按最宽文本单词分配，输入控件没有文本子节点 | 使用现有 min/max-content 测量，包含控件与图片，结合固定及百分比宽度；小视口不把不可拆分内容压入邻格 |
| 两个搜索按钮分成两行 | 中间表格列未预留控件的真实宽度 | 同一列宽修复让两个 inline-block 按钮保留在一行 |
| 272×92 标志被拉成 272×134 | 图片把带 padding 的 border box 当成像素目标区域 | 位图仅绘制到 content box；布局、点击和脏矩形保留外框，内边距变化加入绘制签名 |
| 居中后脚本读到旧坐标 | 换行对齐只移动 display items | 同步移动该行产生的 box records，保持浮动与定位元素的独立位置 |

参考：[HTML 默认渲染与样式提示](https://html.spec.whatwg.org/multipage/rendering.html)、[CSS 2.2 表格列宽](https://www.w3.org/TR/CSS22/tables.html#auto-table-layout)。这不是完整表格实现：现有 `colspan`/`rowspan` 仍按 1 处理；也未宣称所有过时 HTML 表现属性均已支持。

源文件：`c/apps/browser/css_engine.c`、`layout.c`、`browser_paint.c`。

## 可重复回归

```sh
make BUILD=build-google-home/work test-legacy-home
make BUILD=build-google-home/work test-inline-hit test-inline-flex test-image-intrinsic
```

`tests/unit/legacy_home_test.c` 通过真实 DOM → LibCSS → layout → paint 流程检查 360、800、1126px 三个宽度、级联覆盖、固定单元格里的更大控件、非对称图片内边距与边框、外框不变时图片移动的重绘。最终 95 checks，0 failures。

负对照 `LEGACY_HOME_NEGCTL` 恢复旧实现。必须观察到 `center aligns inline image`、`table cell contains its input`、`image pixels exclude padding and border` 失败，才能运行正测试。负对照已失败，正测试已通过。

`tests/grender.mk` 原来是 2026-08-30 留下的空占位，现在接入上述测试；使用二次展开获取后续 include 定义的公共 painter 源列表，避免编译命令正确却遗漏重建依赖。既有 inline-hit、inline-flex、image-intrinsic 回归亦通过。

全仓库 `test-mk-wired` 未通过：另外三份 AMD Polaris 测试未被 include（`native/test.mk`、`present.mk`、`runtime.mk`）。新增网页测试不在未接入列表中。记录在 `build-google-home/wiring.log`。

## 客机证据

隔离目录 `build-google-home/work` 从上一轮 B 站已验证目录作 APFS 副本，重建浏览器，使用根 Makefile 的 mkfs 配方原子重打包系统盘；保留了 5 个用户状态 inode。此次未执行整个工作区的全量构建，未替换工作区默认系统盘，也未把旧内核称为当前全部源码的构建结果。

```sh
python3 tests/qmp/qmp_site.py \
  --iso build-google-home/work/logit.iso \
  --disk build-google-home/work/disk.img \
  --name google-final --url https://www.google.com/ \
  --out build-google-home/final/google.json \
  --boxes --images --keep \
  --input-class lst --input-text LogitOS --trace-input
```

截图和 JSON：

- `build-google-home/before/google-before.png`：原始画面。
- `build-google-home/after/google-after.png`：修复后的真实首页。
- `build-google-home/after/google-after.input.png`：物理输入 `LogitOS`。
- `build-google-home/after/google-after.submit.png`：搜索提交后的实际返回页面。
- `build-google-home/after/google.json`：初次修复的首页与独立搜索观察。
- `build-google-home/final/`：最终二进制复验。

最终复验已完成：`google-final` 为 `PAINTED`，首页无 JS 异常或资源缺口；7 个物理输入字符全部经过 native default 并触发绘制。已分别查看最终首页及输入截图，确认标志比例、居中、按钮同行、链接不重叠和 `LogitOS` 输入内容。最终二进制的 inline-hit、inline-flex（36 项）、image-intrinsic（141 项）回归亦通过；负对照按预期失败。

初次修复的客机显示：输入框仍为 512×32，其单元格由 131px 变为 564px；左右列各 281px；两个按钮同处 y=238；图片外框仍为 272×134，但位图按 272×92 内容区域绘制。已查看 QEMU 截图确认像素表现。

测试工具新增显式 `--submit-input` 开关才会按 Enter；普通输入观察保持不提交。搜索阶段另存日志、截图和错误，避免把结果页错误写进首页评分。

搜索提交的边界：7 个输入字符均经过 native default 并绘制；Enter 确实产生带 `q=LogitOS` 的 `/search` 导航。Google 随后返回“异常流量”验证页，未展示结果列表。验证页还暴露 `solveSimpleChallenge` 未定义及 passive iframe 无脚本上下文的错误；本轮未修改验证机制。因此不能宣称搜索结果页、验证流程或整个 Google 网站兼容已完成。

## 最终镜像指纹

| 文件 | SHA-256 |
| --- | --- |
| `work/browser.aex` | `e4a1f7c6ca6b9e74b0fa09d31be78d3a1425a9c548160b5c0e13e15ede0ca863` |
| `work/disk.img` | `278ff42729f5cac73158b2139f15d4f7fb6f87239779eacc3da45f9df8d4280b` |
| `work/logit.iso` | `0f80979f37e8a17e590b45e7579f26231ae137607f99047c2f9926784190ec73` |

路径均相对于 `build-google-home/`。实测环境为 QEMU x86_64 TCG、4 vCPU、1 GiB、1280×800。此记录证明该首页的渲染和输入改善，不是实机或性能提升结论。
