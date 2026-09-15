# 浏览器扩展真站测试与动画卡顿修复 — 2026-09-13

本轮补测百度、Bing 搜索、哔哩哔哩、Apple、Stripe、微信官网，另以
example.com 检查仪器和网络。完成编辑前、第一版修复、最终版本三组共
21 次独立真站启动，再完成一次 Bing 原生输入测试。

**整站加载慢仍未解决。已验证的提升是动画期间的响应速度，不能称为整站
加载快了 3.6 倍。** 真站暴露了持续重复布局，也暴露了与它无关的网络、
hydration、图片容量及输入处理问题。所有失败均保留，没有减少页面脚本、
图片请求或样式规则来得到更短时间。

## 真站：主流程和 load 事件分别记录

下表是编辑前与最终冻结版本的观测值，单位 guest 秒。每次新 VM、相同
内核 ISO、snapshot 磁盘，两台测试 VM 并行。宿主另有其他任务，CDN 内容
和网络时间也波动，不能把这张单次整站表解释为相同工作量的因果速度比。
中间版本的完整结果也保存在 `evidence/live-after/`。

| 网站 | 编辑前主流程 | 最终主流程 | 编辑前 load | 最终 load | 最终观察 |
|---|---:|---:|---:|---:|---|
| 百度重定向后的首页 | 8.75 | 9.30 | 9.87 | 11.11 | 2 个 JS 异常；初始跳转页另耗 0.64s |
| Bing 搜索 python | 3.04 | 3.09 | 3.14 | 3.20 | 资源失败及请求缺口；排版仍有重叠 |
| 哔哩哔哩 | 5.98 | 7.80 | 8.80 | 10.80 | hydration mismatch；正文内容亦变化 |
| Apple | 7.19 | 9.72 | 19.72 | 18.59 | 字体请求 404；4 张图片被解码缓存上限拒绝 |
| Stripe | 17.12 | 20.78 | 未观察到 | 未观察到 | React hydration 错误，实际呈现错误页面 |
| 微信官网 | 1.86 | 1.81 | 2.16 | 2.12 | PAINTED；24 段文字、152 字节保持一致 |
| example.com 控制页 | 1.54 | 1.22 | 1.54 | 1.22 | 控制通过，19 段文字、109 字节 |

Bing 基线还发生了第二次导航，主流程 2.61s、load 2.70s，未合并到首次
导航中。百度初始跳转页基线 0.62s，亦未算作最终首页加载速度。

`[load-perf]` 的 total 是同步导航、样式、初始脚本及初次生命周期处理；
`[load-complete]` 是浏览器实际派发 load 的记录。后者仍不等于所有后续
fetch、计时器或页面交互完成。宿主 `load_seconds` 不参与速度计算。
`PAINTED` 也不是视觉和交互完全正确的判定。本轮查看了微信、Apple、
Stripe 和 Bing 输入截图，Stripe 的错误页没有被当作成功主页。

## 修复：透明度动画不再重算整页几何

微信基线观察期间出现 **288 次 layout_page**，典型每次含 **546 次
flex/grid trial**；最终同一仪器流程只出现 **4 次布局**。初始主流程本来
只有约 1.8 秒，这种后续计算更接近“已经打开却仍然很卡”的症状。

旧事件循环收到透明度变化，就完整执行 layout_page，重新分配显示列表、
测量文字、运行 flex/grid、处理 SVG 和图片绑定。现在由
`layout_refresh_opacity(root)` 更新现有绘制项的 opacity/hidden，并使用
原来的 zsort 重建绘制与命中顺序；DOM、字体、边框、尺寸等变化继续走
完整布局。排序分配失败也回退到完整布局及原有滚动范围协调。

测试实际找出了两处实现陷阱：

- 文字取父元素样式；生成内容必须保留 generated_style，不能误用宿主元素。
- opacity 跨过 1 会改变 stacking context，单纯修改 alpha 会画错层级。
  即使一个祖先没有自己的绘制项，也必须被排序器考虑。

内联 SVG 的绘制项沿用完整布局的 alpha 规则，避免再次给栅格内容叠加
根 alpha。新增布局次数计数器用于工作量验证，不使用宿主耗时充当浏览器
性能。没有加入任何站点、框架或 bundle 名称分支。

## 固定工作量：真实 browser.aex 中的两轮测量

`tests/fixtures/browser/animation-refresh.html` 含 240 个 grid/flex 单元，
一个持续透明度动画及原生键盘监听。JS 用 guest requestAnimationFrame
时钟记录固定 48 个帧间隔；宿主仅提供 HTTP 字节、键盘及截图。
**这是合成渲染工作量，不能冒充真站加载。**

| 指标 | 基线第 1 / 2 轮 | 最终第 1 / 2 轮 |
|---|---|---|
| 48 帧耗时 | 5210 / 5180 ms | 1420 / 1460 ms |
| 最大帧间隔 | 210 / 210 ms | 50 / 80 ms |
| 完整布局次数，含初始及最终固定姿态 | 57 / 58 | 5 / 5 |
| 动画期间原生按键 | 均收到 | 均收到 |
| 元素几何保持不变 | 均通过 | 均通过 |

平均耗时 **5195 → 1440 ms，快 3.61 倍，减少 72.3%**；平均帧率约
9.2 → 33.3 fps。最终固定姿态的页面区域 `(138,188)-(1260,695)`，两组
前后截图逐像素比较均为 **0 不同像素**。比较不含时钟、地址栏、dock，
也不是对所有动画中间帧和全部页面区域的证明。

此前两版截图装置在动画取消尚未经过更新时就固定姿态，照片留下不同的
动画 alpha；其时间记录、失败照片仍保留在 `animation-before/after-*` 和
`animation-fixed-*`。最终装置在取消跨过一次动画更新后再设置固定 CSS，
**计时区间不包含这段拍照准备**，完整重跑两轮后才进行上述像素比较。

## Bing 原生输入：仍有明显延迟

在真实搜索页观测到的 `sb_form_q` 控件中输入 `logitos`，不提交搜索。
7 个原生默认编辑动作均被记录，之后观察到绘制，截图显示输入内容；
建议菜单亦出现，但标签和输入框仍有重叠。

七次 `key-arrived → frame-painted` 分别是
**620、280、210、500、950、390、400 ms**。这些是 guest 在浏览器分发
按键后到自身绘制返回的区间，既不含之前的内核输入排队，也不证明显示器
已经呈现。仅默认编辑阶段约 150–180ms，后续回调/样式/绘制又增加延迟。
这条慢路径没有被本轮透明度优化解决，仍需要单独采样归因。

## 回归与构建

- 新 `test-animation-refresh`：13 项通过；实际 app_main/计时队列驱动
  动画，检查 alpha 变化、几何不变、没有新几何布局，并在 127、255、0、254
  四个 alpha 状态比较快速刷新与完整布局的结果。
- 旧实现负对照实际失败：`FAIL: animation frames do not rebuild geometry`，
  同时动画变化和几何检查通过。负对照是正向测试 prerequisite，已接入 ci-host。
- 同一测试的 ASan/UBSan 通过；`detect_leaks=0`，不称为泄漏检测通过。
- 既有 opacity-group 12 项、WAAPI paint 39 项、动画时钟 80 项、stacking
  46 项及其 sanitizer 检查通过。日志里的负对照 FAIL 是预期结果。
- `test-mk-wired` 通过：299 个片段、298 可达、1 个声明的特殊入口。
- 初次新 host 链接漏了动画插值依赖，改为从既有 ANIMCLK_SRC 减去 QuickJS
  源集合派生后通过。这是测试装置问题，没有算成产品 bug。
- 独立 `BUILD=build-browser-speed-0913` 完成实际 disk.img 构建；源码
  whitespace 检查通过。保留共享仓库已有改动。

最终 ISO SHA256：
`5e33a17a9acfce481157a0f4a5a5e5548c2c9bcfcf2de835df9dd4a45a17f80d`

最终磁盘 SHA256：
`9dd15756f5adcf25276761f860b9e57d742835490f93b78290447560e5633bd4`

最终冻结后，共享 browser.c/layout.c 又收到其他工作的修改。本报告对应
冻结镜像；核心源码快照已逐项校验与构建时保存的 SHA256 相同，不把后续
焦点/布局工作混进本轮测量。没有重启其他任务已打开的虚拟机。

## 复现与证据

```sh
make BUILD=build-browser-speed-0913 test-animation-refresh-san
python3 tools/perf/animation_refresh.py \
  --iso build-browser-speed-0913/final/logit.iso \
  --disk build-browser-speed-0913/final/disk.img --out /tmp/animation-repeat
python3 tests/qmp/sites_run.py \
  --iso build-browser-speed-0913/final/logit.iso \
  --disk build-browser-speed-0913/final/disk.img \
  --only baidu,bing-search,bilibili,apple,stripe,weixin,control-example \
  --jobs 2 --label speed-0913-repeat --outdir /tmp/browser-sites-repeat
```

上述 make 会测试当时的共享源码；复核本轮精确实现应使用保存的源码快照
或源码补丁。冻结镜像不受共享目录后续构建影响。

- 可直接启动本轮版本：`build-browser-speed-0913/final/run-browser.sh`。
  使用 snapshot，测试会话写入不会保存；已启动的其他 VM 不会自动换成新版。
- 原始真站结果：`build-browser-speed-0913/evidence/live-{before,after,final}/`。
- 接受的帧测量及照片：`evidence/animation-settled-{before,final}-{1,2}/`。
- Bing 输入截图、串口及七次 guest 时间：`evidence/native-input/`。
- 汇总：`evidence/measurements.json`；构建及核心源码 hash：
  `evidence/manifest-before.json`、`evidence/manifest-final.json`。
- 本轮精确源码快照与独立差异：`evidence/source-final/`、
  `evidence/source-changes.patch`。新单元测试、页面和驱动也包括在差异中。
- 门禁：`evidence/animation-final-gates-2.log`、`related-gates.log`、
  `mk-wired.log`；早期失败记录另存，未覆盖。
