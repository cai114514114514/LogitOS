# Definite percentage height — 2026-09-09

生产修复 `c/apps/browser/layout.c`：先前普通 block / float / inline-block / flex / grid 桥接入口统一传 `-1` 给 `spec_h`，即使祖先明确 `height:200px` 也放弃百分比。图片两个入口另行丢弃百分比高度，inline 图片还丢弃百分比宽度。

现在使用共同 `height_basis` / `definite_content_height`，按 CSS 2.2 10.1 / 10.5 从真实包含块求 definite content height，普通盒排除父 padding/border，absolute 使用 positioned ancestor 的 padding box，fixed / root 使用 `css_media_height()`。auto 祖先立即中断链，min-height 不冒充 definite height；普通 inline wrapper 不建立包含块。calc 的 px addend、box-sizing、min/max 同走已有解析器。百分比图片高度不再由 HTML 属性覆盖，确定的 0 与 auto 区分。clip 也消费相同基准。

规范：https://www.w3.org/TR/CSS22/visudet.html#the-height-property

## 实测与负控

`make BUILD=build test-percentage-height`：最终 **41 checks, 0 failures**。其 prerequisite 用 `LAYOUT_PERCENT_HEIGHT_LEGACY` 恢复缺失基准及旧图片路径，**41 checks, 18 failures**，明确打印 `definite parent resolves percentage height -- got 0, want 100`、`block image percentage height overrides HTML attribute -- got 11, want 100`、`definite zero block image never resurrects HTML height -- got 20, want 0`。

完整日志位于 `build/site-general/layout/percentage-final.log`；首次未修复生产 baseline 为29 checks/12 failures（后续增加resize/root abs/zero边界），位于 `percentage-baseline.log`。这是 parser → LibCSS → layout 的 host 几何测试，使用固定 glyph advances，不是 guest 字体、图片解码或真实站点视觉验收。

回归：layout-box 51、flex 176、max-height 18、intrinsic 65、grid parse/place/size/align 142+84+40+52 全部通过，原有负控照常红。日志 `percentage-regression.log` / `percentage-regression-final.log`。

普通 fixture：`tests/fixtures/engine-expansion/percentage-height.html`，目标 `PERCENTAGE-HEIGHT PASS half=100 full=100 auto=37 image=160x100`。根代理负责统一 disk build 和 guest before/after 验证，尚不能由此断言真实网站已恢复。

## 明确边界

此处不从半成品 box record 或上一帧高度推断百分比基准，避免内容环及 resize-history 污染。flex/grid 算法后分配给 auto 项的尺寸需要明确的 definiteness 传递契约，仍未由本补丁实现。列 flex 的 shrink / basis、Grid 字体单位、fixed 的 transformed-ancestor containing block 等独立缺陷不能计为本项完成。祖先链深度超过128次返回indefinite，保留有限成本。
