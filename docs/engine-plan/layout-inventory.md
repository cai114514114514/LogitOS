# HTML / CSS / layout / paint 能力与实现入口


**第二批更正（2026-09-09，保留下面首次审查的旧结论）：生成内容、扩展级联和 modal top layer 已有新消费者；具体支持边界与 host/guest 证据见[统一接线报告](expansion-status-2026-09-09.md)。下面 snapshot/无持久化/无顶层消费者等原判断只描述第一批时点。**

更新：2026-09-09；只据当前源码与本地机制门禁，不把 API 存在、历史 WPT 数量或单页截图当完整兼容证明。此文件由 layout 代理维护。工作策略是补可组合的引擎组件，再进行系统性兼容验证。

## 数据流与可扩展边界

`dom_parse` → `html_parse` / tokenizer / tree builder → `css_apply`（LibCSS 选择、继承、computed style）→ `css_extra_apply`（扩展属性）→ `layout_page` / `layout_block` → `item[]` 与 node border boxes → `browser_paint_scroll`。JS 修改通过 dirty/scoped style 路径重新进入该链。源码中的旧注释有滞后，例如 `html_tree.h` 仍写未替代 dom_parse，而 `dom.h:237` 和 dom_parse 实现已指向新树构造；以下以实际入口为准。

| 组件 | 已有实际路径 | 缺口/边界 | 下一步应扩展的接口 |
|---|---|---|---|
| HTML 文档与片段 | `dom.c:dom_parse`、`html_tree.c:html_parse[_fragment]_ex`、`html_tokenizer.c`；DOM 有 namespace、quirks、template 标记 | HTML 树存在不等于 script/parser 时序、foreign content 端到端完整；样式/布局遍历需统一 shadow 与伪节点策略 | 保持 document/fragment 同一 tree builder；给解析任务与资源调度明确回调，不另建简化 parser |
| 选择器与标准 cascade | `css_engine.c:g_handler/style_node` 驱动 LibCSS；祖先、兄弟、属性与静态伪类走真实 matcher | hover/active/focus 当前 handler 惯性返回 false；不能宣称交互状态选择器完备 | 状态变更→失效范围→同一个 select handler；保持现有样式共享/cache 边界 |
| 扩展属性 cascade | `css_extra.c:compile_exact_matcher/walk_exact/apply_patch` 使用主引擎完整匹配；grid/raw transform 等交给消费者 | 属性 patch 仍按 source order；specificity/important/layer 尚未完整合流。伪元素专属规则拒绝并计数 | 把扩展 declaration 纳入可比较的 cascade priority，而非增加 last-compound 字符串匹配 |
| CSS 依赖 | `css_import.c` + browser CSS collector：递归 import、相对 URL、顺序、media、预算 | import supports/layer qualifier 尚拒绝/保留；加载覆盖不等于支持 cascade layers | 扩展 dependency record 的条件树，不丢条件拼接 |
| 行高/字体相对值 | `css_engine.c:normalize_line_height` 新增继承前维度规范；NUMBER 保留比例；`cstyle.has_line_px` 区分 0/normal | 原实现 130%→130px，em 继承到子字号重新解释；新代码已落盘，36项机制检查通过（旧行为负控16项失败）。像素字体与内部 fixed 精度仍需区分 | 在 computed 阶段规范长度，clone interned style，禁止原地 mutate 共享对象 |
| block / BFC / margins | `layout.c:layout_flow/layout_block/is_bfc_root`；margin collapse、负 margin、auto、百分比边界已有门禁 | 页/多列 fragmentation 不是完整组件；复杂 orthogonal writing mode 无完整排版模型 | 提取 containing-block/formatting-context 参数；让 margin/float/overflow 走统一盒协议 |
| 原子行内盒 | `flow_node` 统一三类原子盒路径，内部各走 block/flex/grid | inline-block 原被当整行 block；inline-grid 丢失 outer display；grid intrinsic 原不从 track solver 取值 | **本批实现**：统一 outer inline 分类、shrink-to-fit、换行、边框/内边距、flex/grid item blockification，复用各内部布局算法 |
| flex | `layout_flex.c/h` 纯数值算法；`layout.c:flex_collect/flex_row_spec/flex_place` 为 DOM adapter；本地176项回归 | 原子 outer 与内层 algorithm 需分离；intrinsic 是估计；复杂 baseline/fragmentation 不等于完整规范支持 | 保持 measure/place adapter，扩展 intrinsic constraint 输入，避免重写第二套 flex |
| grid | `layout_grid.c/h:grid_layout`：tracks、placement、named lines/areas、auto repeat、alignment；`grid_spec/grid_measure_cb` 接真实 DOM | 模块明确未实现 subgrid、masonry、fragmentation；跨轨 baseline 与再解析阶段有限；百分比 gap 仍无完整 producer | 使用 `GRID_INDEFINITE` 和 measure callback 提供 min/max intrinsic；subgrid 需要父子共享 track ownership，不能仅加解析名 |
| table | `layout.c:layout_table` 有 row/cell 测量与分配；CSS adapter 区分 table 相关样式 | 仍是单体 minimal table 路径，完整 intrinsic、rowspan/colspan、caption、collapsed-border冲突、分页均需分项验证 | 提取 table grid model 与 cell contribution/placement 接口，再扩宽算法，不在 flow 中逐标签补丁 |
| 文本与行内流 | 产品路径仍 `flow_text` + `iflow`；已接 Unicode break、字重测量、跨 inline 空白、控件 intrinsic | `layout_text.h` 明确 `ltx_layout_runs` 整体尚未接替产品 flow；完整 bidi/shaping/vertical-align、自动断词、多行 balance 尚未由统一行盒完成 | 优先让 ltx_env 提供 `line_avail(ctx,y,probe_h,x_out)`；复用 `float_band` 后分阶段替换旧分词/排版环 |
| pseudo / generated content | LibCSS 能产生 pseudo computed styles；adapter 当前只将 NONE 转为实际 node style | before/after 没有真实可排版的生成盒树；css_extra 已拒绝伪元素误作用到本体 | 引入 generated-box owner/target 身份，content/counter→盒树；不要把 pseudo style 写回 originating element |
| position / floats | `layout_abspos_child/place_float`、CB 保存恢复、relative/sticky 与 paint 路径存在 | 复杂定位 CB、变换形成的 containing block、滚动容器 sticky 需按统一上下文审计；当前有重复 CB 保存逻辑 | 统一 positioning context 与 containing block 查询，测量/正式放置共用 |
| scrolling / overflow | `layout_node_scroll` 查询；viewport 横纵 scroll 与 `browser_paint_scroll`、水平交互已集成 | nested scroll container、滚动链/overscroll、CSSOM scroll offsets 需要统一 owner；不能由根 scroll 推断所有层 | 每个 scroll container 有 content extent、offset、clip 与输入分发接口，根视口只是一种 owner |
| paint / hit test | `browser_paint.c` 消费 item[]；背景、边框、渐变、transform、clip、阴影与文本/控件绘制；inline 空白有 hit item | 平铺 display list 与 node boxes 不是完整 fragment tree；复杂 stacking context/group opacity/filter、文本基线、盒坐标同步仍有边界 | fragment/paint property tree 与 hit geometry 共用变换/clip；不要分别发明坐标系 |
| SVG / 图像 | `layout.c` 将 SVG 作为 replaced content，媒体解码/图像资源有单独缓存与生命周期；SVG 在库层 | 不等于完整 SVG DOM、滤镜、SMIL、foreignObject 与 CSS cascade 集成 | 保持 replaced intrinsic-size / decode / paint 三段接口；foreignObject 需显式 HTML formatting context |
| Canvas | `js_canvas.c` 是实际绑定/绘制实现，并非空 API；图像数据与导出有具体路径 | 全状态/字体/色彩/合成/跨资源语义须逐机制验证；存在接口不等于全部 Web canvas 合规 | 复用 graphics surface 与资源 ownership；序列化、像素回读、drawImage 同一资源边界 |
| 表单控件 | `forms.c/h`、`ctl_metrics` 与 layout control item；JS/native select 状态和文本尺寸已接 | 控件外观、focus 状态、selection、事件默认行为需要统一组件契约 | 控件 model→intrinsic→layout item→paint/input，避免 JS 状态与 native 状态双份来源 |

## 实现依赖顺序

1. **统一原子行内盒（本批已落盘）**：保留现有 display 内部算法，增加 outer 分类；统一尺寸/间距/换行；grid intrinsic 直接复用 grid solver。产物是可用 CSS→layout 代码与组合门禁。
2. **统一 generated / normal fragment 身份**：以 node owner + pseudo target + fragment kind 表达生成内容和真实元素，给 pseudo paint、hit、CSSOM boxes 提供共同基础。
3. **扩展 declaration cascade priority**：在真实 selector 结果上加入 specificity、important、layer、origin；与 generated style 使用同一匹配/优先级，不建平行匹配器。
4. **可变行宽文本组件接线**：先 line_avail，再把 bidi/shaping/line breaking/空白处理合流到 ltx；沿用真实 float/inline boxes，逐步退休旧字符循环。
5. **scroll / positioning / paint 上下文统一**：嵌套 scroll owner + CB + clip/transform tree，为 sticky、hit 与可见区域查询提供统一坐标契约。
6. **table model 与 fragmentation**：先建立 rows/cells/spans 的可测量模型，再实现完整分配/冲突处理；分页/多列共享 fragmentation 接口。

## 本批验证边界

机制门禁使用真实 DOM→CSS→layout 链，覆盖文本与三类原子盒分别混排、wrap、固定/自动尺寸、内层 flex/grid、nested item blockification 和 repeated layout。负控必须由正控依赖执行并以预期 exit1/命名断言证明失效。最后由主线程统一 fresh disk 构建；本批不进行真实站点/逐像素调试。未列出的组合不据此宣称支持。
