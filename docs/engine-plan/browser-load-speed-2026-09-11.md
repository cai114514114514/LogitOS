# 浏览器加载速度优化与实测 — 2026-09-11

本轮修改浏览器加载、模块预取、DOM 选择器及样式失效处理。直接访问
GitHub、DeepSeek、Wikipedia、Python 官网，以 QEMU 中浏览器自己的
`[load-perf]` / `[module-perf]` 时钟记录作比较。

## 结果与证据边界

最终完成四个站点各两轮、共八次独立启动的复测。下表单位为 guest 秒，
基线为本轮编辑前保存的镜像；两轮最终测量都使用冻结后的同一个浏览器镜像。

| 真站 | 编辑前 | 最终第 1 轮 | 最终第 2 轮 | 检查结果 |
|---|---:|---:|---:|---|
| [GitHub](https://github.com/) | 119.00 | 39.37 | 37.69 | 正文保留；两轮均有后续 fetch 失败 |
| [DeepSeek](https://www.deepseek.com/)，首次导航 | 2.36 | 2.12 | 2.09 | 两轮 PAINTED，42 段 / 245 字节 |
| [Wikipedia: Operating system](https://en.wikipedia.org/wiki/Operating_system) | 22.34 | 16.69 | 17.84 | 首轮缩略图 TLS 失败，次轮 PAINTED |
| [Python](https://www.python.org/) | 13.54 | 10.54 | 12.22 | 两轮有广告脚本报错及仪器记录的请求缺口 |

GitHub 主加载流程平均 38.53s，比基线快 **3.09 倍**，耗时减少 **67.6%**。
其中模块 eval/jobs 从 58.59s 降到 **2.70 / 2.53s**，平均快 **22.4 倍**；
脚本阶段总体从 82.63s 降到 13.85 / 11.66s。初始模块编译均为 70 次、
6,151,828 字节，初始 loader fetch 62 次；基线与最终记录的模块 URL / 字节数
序列一致。完整外链资源与页面交互仍不是全通过，不能把上述比例解释为所有
网站所有资源都能以这个比例加速。

DeepSeek 的脚本还触发一次后续导航，分别记录为基线 1.80s、最终 1.49 / 1.50s，
没有混入首次导航数据。Python 广告内容变化，最终 136 段 / 589 字节与基线
141 段 / 594 字节不同，因此只列观测值，不据此宣称相同工作量下的提升。
Wikipedia 成功的第二轮相对基线约快 20%，CDN 时间仍占明显比例。

`total_ms` 是初始导航、样式、脚本和初次生命周期处理这条加载流程的耗时，
**不等于全部异步图片、后续 fetch 和交互都已完成**。它从导航开始计时，
首段名为 `teardown`，目前也包含导航下载。工具的 `load_seconds`、宿主等待
和截图预算不参与速度计算。

每站每轮单独启动 VM；前后使用同一个固定内核 ISO，镜像先复制再运行，
QEMU 使用 snapshot，不复用前一个站点的 cookie、会话或缓存。TCG 在 Apple
Silicon 宿主运行，宿主仍有其他任务；真站 CDN 和请求成功率也存在波动。
因此整站单次比例是观测值，因果证据另由确定性回归和负对照提供。

`PAINTED` 只表示仪器观察到了正文像素、无记录到的脚本异常且资源请求数量
符合检查，不代表页面视觉或交互完全兼容。已查看 GitHub 前后截图，标题及
正文保留相同 55 个文字片段、321 字节；导航栏重叠等既有排版问题仍在。
Python 的资源缺口、GitHub 偶发 TLS / 图片 / 后续 fetch 失败会保留在报告里。

## 找到并修复的重复工作

1. **首次样式重新计算。** 初始完整样式已经应用，动态样式跟踪器的签名却
   仍是零，第一帧又扩展变量、执行 cascade、layout。现在在脚本执行前提交
   当前 stylesheet 签名。后续真实样式变化仍能被发现。
2. **已加载模块重复预取。** 共享模块和环中的父模块已经进入 QuickJS 的
   当前 context 模块表，递归预取却再次占用缓存。新增 `JS_HasModule` 查询
   与解析器相同的模块表；不把“已进入模块表”当作“执行成功”，异常缓存
   和循环模块身份仍由 QuickJS 原逻辑负责。
3. **预取请求满额时丢失队尾。** 缓存允许 32 项，请求句柄只有 16 个，
   原来 24 个预取只启动 16 个。现在保留待启动 URL，在前序完成释放句柄后
   继续启动；未提高连接数或响应体限制。其他消费者占用所有句柄时返回，
   避免相互等待；清空缓存也清空未启动任务。
4. **反复拆分类名和转换标签大小写。** 私有有界缓存保存精确字符串的
   计算结果，不缓存节点或查询答案。修改 class 会立即使用新字符串；
   容量满、长字符串继续走原计算路径，ASCII 大小写语义不变。
5. **复杂选择器列表和首次匹配全 JS 扫描。** 真实 GitHub 诊断捕获到两个
   超过 100 项的列表查询，分别花了 21.27 / 22.81 guest 秒；大量不存在的
   `react-partial[partial-name=...]` 查询也逐次扫描整页。现在由原 JS parser
   的 AST 提供必要的正向字面条件，C 侧一次前序扫描完成候选并集，完整 JS
   matcher 再确认结果。保留文档顺序、去重、Shadow DOM 边界和静态 NodeList。
   `querySelector` 一次只取一个候选，失败才从该节点之后继续，不生成整份
   候选数组。无必要条件或列表超过原生容量时使用完整回退路径。
6. **动态样式应用后再次消费 DOM 脏标记。** 完整样式计算和布局已完成，
   随后的 `settle_dom` 又做一次。现在在 load/error 回调之前消费已经处理的
   失效记录，同时保留动画快照和滚动范围协调。回调中新产生的失效继续保留。

选择器筛选沿用原有启发式，不能保证每个条件都稀疏。例如 `.row.hit` 选择
公共类 `.row` 时仍可能保留全部候选；这是额外工作，并不会丢失正确结果。
未引入站点名称分支、减少脚本执行、忽略规则或扩大 watchdog 预算。

## 回归闭环

每个新负对照都作为正向测试的 prerequisite 接入 `ci-host`，并实际观察失败。

| 检查 | 修复前 / 负对照 | 修复后 |
|---|---|---|
| 初始样式提交 | 初始样式被重复构建 | 初始 47px 正确、后续编辑 93px 正确 |
| 样式脏标记二次消费 | 一次编辑触发两个布局 | 只消费一次；load 回调再改 137px 仍生效 |
| 模块图预取 | 14 项检查中 6 项失败 | 14 项通过，共享依赖只预取一次，环身份保留 |
| HTTP/2 预取队列 | 24 个 URL 仅预取 16 个 | 31 项通过；24 个完整内容、单连接、取消与外部占用 |
| 选择器字符串计算 | 12 项中两项重复计算失败 | 12 项通过，DOM 修改、NUL、非 ASCII、容量回退 |
| 批量与首次选择器 | 两项工作量检查失败 | 15 项通过，候选筛选、顺序、去重、提前停止 |

相关既有门禁：选择器 50 项、原生选择器 37 项、动态样式页 13 项、模块预算
15 项、模块失败重试 12 项、HTTP/2 168 项。选择器与预取的 ASan/UBSan
检查通过；这些命令关闭了 leak detection，不能据此宣称做了完整泄漏检测。

两次测试装置假设也被纠正并保留日志：首次模块图测试借用的简化 URL resolver
不能处理 `./sub/../`，改成同图的绝对 URL；批量候选测试最初假设公共 `.row`
必然被过滤，实际其作为必要条件会合法保留所有节点，改为分别检查稀疏候选
工作量和公共条件下的结果正确性。没有把它们算成产品 bug。

## 固定页面样本

`tools/perf/browser_load.py` 的 DeepSeek / Wikipedia 是保留的 HTML/CSS 样本，
去掉真实脚本及图片网络，再加入完成探针，用于隔离样式成本，不能冒充真站。
最终隔离结果：Wikipedia 总流程 **7.32s → 3.88 / 4.10s**，平均快 1.83 倍；
重复生命周期处理 **2.84s → 0.04s**。DeepSeek 样本为 3.61s → 2.87 / 2.27s。
四项最终运行均检查到实际脚本和 load 完成标记，运行前后镜像 hash 未变。
这些运行的完整 guest 串口及结果保存在 `evidence/workloads-before/` 与
`evidence/workloads-final/`。

## 构建、复现与产物

所有构建使用 `BUILD=build-browser-speed-0911`，保留共享目录原有修改。
源码编辑前保存的基线不等同于 Git HEAD；只对这一会话的前后快照进行比较。
内核首次构建因同时进行的 GPU / TCP 修改暂时失败，后来重新构建 ISO 成功。
新内核配合最终浏览器也通过独立启动、HTTP 页面、脚本及 load 生命周期烟测
（`evidence/current-kernel-smoke/`），这不替代固定内核上的两轮真站比较。
性能比较继续使用相同的固定内核，避免把内核变化算作浏览器优化。

```sh
make -j4 BUILD=build-browser-speed-0911 build-browser-speed-0911/disk.img
make BUILD=build-browser-speed-0911 test-module-prefetch-loaded test-prefetch-queue \
  test-selector-tokens test-selector-batch test-initial-stylesheet test-mk-wired
make BUILD=build-browser-speed-0911 test-simple-selector-asan \
  test-selector-batch-asan test-prefetch-queue-asan
python3 tests/qmp/sites_run.py \
  --iso build-browser-speed-0911/final/logit.iso \
  --disk build-browser-speed-0911/final/disk.img \
  --only github,deepseek,control-wikipedia,python --jobs 1 --repeat 2 \
  --label speed-repeat --outdir build-browser-speed-0911/evidence/live-repeat
```

- 性能对比镜像：`build-browser-speed-0911/baseline/`、`final/`。
- 启动冻结后的测试版本：`build-browser-speed-0911/final/run-browser.sh`。
  脚本使用 snapshot，不改写保存的浏览器测试 profile。
- 每站日志、JSON、PNG、宿主页面探针：`build-browser-speed-0911/evidence/live-*`。
- 阶段汇总：`build-browser-speed-0911/evidence/measurements.json`。
- 源码与镜像 SHA256：`build-browser-speed-0911/evidence/manifest.json`。
- 已保存基线的核心文件差异：`build-browser-speed-0911/evidence/source-changes.patch`。
- 诊断程序与镜像：`query-diagnostic/` 及 `evidence/query-profile/`。
  这些带采样的运行只用于定位，不用于报告速度比例；JS 源码采样代码未留在运行路径。
- 最终门禁日志：`evidence/final-gates.log`、`module-gates.log`、
  `prefetch-gates.log`、`selector-tokens-gates.log`、`selector-final-gates.log`。
  带 `negctl/old` 的 FAIL 是预期负对照，测试命令退出状态才说明门禁是否通过。
