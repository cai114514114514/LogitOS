# CSS compatibility audit — 2026-09-09

审计阶段原记录为：“本轮是只读生产链路审计：没有修改 CSS/layout/paint，也没有构建或运行新增试验。” 随后根代理批准实施第 1 项；实施与实测更新列在文末，其余候选仍为只读发现。下列数值是给复现页面设计的预期/源码推导，**不是新的运行结果**。审计目的为普通组件共有的布局缺陷，不使用域名、框架名、bundle 名或站点选择器作为修复条件。

当前真实入口为 `browser.c:2110 browser_style_layout()` → `css_apply()` → `css_extra_apply()` → `layout_page()` → `js_cssom_reconcile_element_scroll()`；渲染和输入再消费显示项、盒记录与滚动状态。下面每项均在此入口可达的实现内，不能以 parser/算法单元测试的存在代替 production bridge 已支持。

## 1. auto 高度盒的 max-height 完全无效；滚动列表再次失去视口

- **生产证据**：`css_engine.c:1627` 已把声明写入 `has_max_h/max_h/max_h_pct`。`layout.c:730 spec_h()` 在 `!has_h` 时立即返回 -1，后面的 max-height 分支仅限指定 height。`layout.c:747 block_height()` 仅应用指定 height 与 min-height，不读取 max-height。
- **另一个必要消费者**：`layout.c:2624 clip_push()` 也只读 `spec_h()`；height:auto 时纵向裁切保持 `0x3fffffff`。只修几何而不修裁切，会得到“盒子缩小、内容仍画到外面”的半成品。
- **普通触发**：下拉菜单、日志列表、对话框正文 `max-height:80px; overflow:auto`，子内容共 240px。当前源码会产生 240px 盒；应产生不超过 80px 的视口，同时保留 240px 内容溢出 extent。小于上限的 40px 内容仍应为 40px，不能把 max-height 当 height。
- **已有样例的空隙**：`tests/fixtures/engine-expansion/element-scroll.html` 用显式 `height:140px`，刚修好的固定 height 门也用显式 height；两者均无法发现 auto + max-height。`tests/fixtures/browser/baidu.html:246` 的普通下拉列表确实使用 max-height + overflow-y:auto，作为形态样本即可，修复不应针对该选择器。
- **建议归属**：layout 代理负责 `layout.c` 中 used-height 与 clip 的一致性；复用 root 刚接好的 paint/scroll consumer，不另造滚动状态。需要时在自己的新测试链接真实 painter。
- **优先级**：本轮首选。它直接阻断已接线的滚动组件，且有窄而清晰的修复边界。

## 2. 普通流子元素的 percentage height 丢失已知父高度

- **生产证据**：`css_engine.c:1598` 邻近 height/calc 转换已经保留 `has_h/h_pct/h_off`。`layout.c:732` 遇 `h_pct && avail<0` 返回 auto。普通块子元素在 `layout.c:3161` 总以 `block_height(st,ch,-1)` 结束；调用 `layout_block()` 的接口也不传父 content-height。`g_cbh` 仅用于定位 containing block，不能拿它冒充普通流父盒的百分比基准。
- **普通触发**：父 `height:200px`，子 `height:50%`，子内容 20px；当前普通流路径仍用内容高度，目标是 100px。这影响分栏面板、嵌套高度链、百分比封面容器。
- **修复边界**：需要明确传递“definite content height”及其生命周期；不能无条件把视口高或 abspos 的 g_cbh 填进去。父 auto 高度时百分比仍应保持未定，padding/border/box-sizing 与 flex/grid 的 definite-height 规则需要分别验证。
- **建议归属**：layout.c/h 代理。CSS producer 已有信息，不需要先增加新语法。比第 1 项范围大，不推荐夹带进本轮窄修。

## 3. flex-direction:column 未消费已解析的 flex-basis / flex-shrink

- **生产证据**：`css_engine.c:1979–1984` 产生 shrink、basis。`layout.c:4013` 附近仅 row 进入 `flex_row_spec()`/纯 flex 算法。column 分支 `layout.c:4022` 从自然布局高度开始；`4057` 仅向 `flex_place()` 传 `spec_h()`。分配分支 `4063` 只在 `container_h > cy-y` 时执行 positive-slack grow/auto margins/justify，负空间没有 shrink 分支，basis 也没有进入此分支的 main-size 计算。
- **普通触发 A**：固定 100px 高的 column，两个 `height:80px; min-height:0; flex-shrink:1` 子盒。源码会留下共 160px 内容，而两项应可以压到各 50px。
- **普通触发 B**：两个 `height:auto; min-height:0; flex:0 0 40px` 子项，内容仅一行；column 使用自然高度而不是 40px basis。
- **为什么 pure flex 测试不能结案**：`tests/unit/flex_test.c` 有 basis/shrink 的算法测试，但生产 column 根本没走该算法桥。需要 parser→CSS→layout_page 的普通页面与实际 child box 断言。
- **建议归属**：layout.c + layout_flex.c/h 的独立代理；先做有 definite height 的 column，明确 natural-height 测量与 min/max 迭代，不能只加一个忽略输出的算法调用。列 wrap 当前也被注释明确退化为 nowrap，宜单列后续工作，不与 shrink 混为一个完成项。

## 4. Grid 的 rem 被当成 em，随网格本身字号错误缩放

- **生产证据**：`layout.c:4404–4411` 将网格容器的 `fpx` 作为唯一字体单位参数传给 `grid_parse_template/tracklist`。`layout_grid.c:129` 的 breadth 分支把 em 和 rem 共同算成 `v*font_px`。这是明确的错误单位转换，不是未实现的 raw declaration。
- **普通触发**：`html{font-size:16px}`，网格 `font-size:32px; grid-template-columns:2rem 1fr`。首轨当前推导为 64px，应为 32px；旁边 `2em` 对照应保持 64px。卡片内部改标题字号会意外改变框架使用 rem 设置的轨道宽。
- **建议归属**：layout.c 网格桥 + layout_grid.c/h 解析接口。应传 element/root 两个明确基准，并同步已有 pure parser 调用方；不能把 rem 固定写成 16px。vw/ch 等单位在同函数降级为 auto，是另一个能力边界，修 rem 不应声称一起解决。

## 5. Grid 显式 start 对齐被当作默认 stretch

- **生产证据**：`css_engine.c:1938` 的 justify-content 转换把初始/default/flex-start 压为同一个 `JC_START`。`layout.c:4302` 附近 `grid_ga_from_jc()` 的 default 又映为 `GA_NORMAL`，让 auto-max tracks 拉伸。旁边旧注释已承认“显式 start 作者会得到未请求的 stretch”，当前实现仍相同。
- **普通触发**：200px 网格，`grid-template-columns:auto auto; justify-content:flex-start`，每项固定内容宽 30px。作者要求两列靠起点，总轨宽 60px；bridge 仍采用默认 normal 的拉伸行为。
- **修复边界**：必须在 cascade→cstyle 层保留 normal 与 start 的差异，不能在 grid bridge 直接把 JC_START 全改为 GA_START，否则会破坏默认网格 auto track 的 stretch。原注释记有 123 个净 reftest 回退，正是这个区别不能丢的证据；该旧数字本轮未复测。
- **建议归属**：CSS producer 代理负责 css.h/css_engine.c 中的准确枚举，layout 代理对接 grid mapping；需要 normal/start 成对对照。

## 推荐本轮窄修：第 1 项

拟新增普通页面 `tests/fixtures/engine-expansion/max-height.html`（**目前尚未创建或运行**），并在 host 用同一内容形态运行 parser/CSS/layout/painter：

```html
<style>
.panel { width:160px; max-height:80px; overflow:auto; border:2px solid #234; }
.row { height:40px; background:#aee; }
.reference { width:160px; height:80px; overflow:auto; border:2px solid #234; }
</style>
<div class="panel"><div class="row">One</div><div class="row">Two</div><div class="row">Three</div></div>
<div class="reference"><div class="row">One</div><div class="row">Two</div><div class="row">Three</div></div>
```

验收必须同时观察：

1. auto + max-height 长内容的 border-box 为 84px、client viewport 为 80px，内容 extent 仍为 120px；短内容自然缩到 40px content-height。
2. 上限以下内容、显式 height、min-height 大于 max-height、border-box/content-box 四类对照，后续普通流元素的位置正确。
3. 真正 paint 中第三行初始被 clip；滚动后该行进入视口，父背景留在原处，native hit 与 client geometry 一致。
4. 临时负控恢复旧“仅指定 height 才 clamp max-height”行为，并让正门依赖该负控；必须看到明确高度/裁切断言红，不能仅看编译或退出码。
5. 根代理重建 disk，在 guest 打开同一普通页面，记录盒几何、滚动文字出现和截图。host 录制器通过不替代这一步。

本轮修复建议先限定为现有 block/flex/grid container 的非百分比 max-height，用 shared used-height 规则保持 min/max 与 box-sizing 一致，并让 clip 使用同一最终视口；百分比高度解析、column flex 算法、vertical text 不夹带进来。其余项目待根代理分配文件归属后实施。


## 第 1 项实施更新（根代理授权后）

- `layout.c:762 block_height()` 对 content-derived height 也应用 max-height，再应用 min-height；保留旧“只 clamp 指定 height”理由及其失效原因。自然 child/item geometry 不裁短，所以 scrollHeight 仍由全部内容计算。
- `layout.c:2980 layout_block()` 在 block/flex/grid 子布局结束后，用最终 used padding-box 高度收紧已产生的 descendant clips。短内容按自然高度裁切、min-height 大于 max-height 时按 minimum 裁切。没有把 max-height 当 height，也没有先保留无限纵向 clip 再仅修改盒记录。
- 初始 15 项 host baseline 实测 11 红：border-box 244px（目标 84px）、clientHeight 240px（目标 80px）、scrollTop 0（目标 80），实际绘制还显示本该裁切的目标。日志 `/tmp/max-height-baseline.log`。
- 最终 `make BUILD=build -f Makefile -f tests/max_height.mk test-max-height` 为 **18/18**。前置负控 `LAYOUT_NO_AUTO_MAX_HEIGHT` 恢复旧上限行为；`LAYOUT_NO_AUTO_MAX_CLIP` 保留正确盒高却移除最终裁切，出现 clip 与真实 painter 断言红。日志 `/tmp/max-height-final.log`。
- 相关回归：layout-box 51、text wiring 16、element-scroll 28、generated content 54 全部正例通过，既有负控也按预期触发。日志 `/tmp/max-height-regressions.log`。
- 普通 fixture 已创建为 `tests/fixtures/engine-expansion/max-height.html`：max-height 面板与显式 height 参考各 100px，内容 200px，短面板自然为 50px；“Reveal red row”按钮滚动 100px 显示第三行。JS 打印 `MAX-HEIGHT PASS` 和实际 geometry。
- 新 gate 为 `tests/max_height.mk`；Makefile include 和 ring-3 disk/guest 验收交根代理统一完成。本条记录不把 host PASS 当作已经完成 guest 绘制验收。
- 本轮边界：auto height + 非百分比 max-height 的 block/flex/grid 容器；百分比高度依赖、列 flex 算法、Grid 字体单位及对齐信息丢失仍是上述独立待办。没有修改站点适配分支，没有新增链接 TU 或改动 item ABI。
