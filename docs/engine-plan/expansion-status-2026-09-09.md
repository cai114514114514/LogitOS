# 第二批浏览器扩展与统一接线（2026-09-09）

用户要求多模型同时推进、最后统一接线。本批在隔离工作树上分成 DOM/Range、CSS/布局/模态、WebCrypto/存储三路；主代理负责级联、事件路径、Promise/MO 调度、原生输入接线、统一构建和 guest 验收。合回前逐文件核对初始 SHA-256，保留主目录原有未提交内容。

## 实际实现

| 组件 | 本批代码行为 | 明确保留的边界 |
|---|---|---|
| 扩展 CSS 级联 | `css_extra_cascade.inc` 按实际匹配选择器 specificity、源码次序和 important 分层合并；inline normal 与 author important 正确竞争；保留单个声明的 shorthand/longhand 次序 | author 非 layered 扩展属性子集；cascade layers/user origin、逻辑/物理属性交叉仍需统一 |
| 生成内容 | 独立 `::before`/`::after` 样式与代理节点，字符串、attr()、none/normal；inline/block/flex/grid 进入真实布局与 painter；不伪造 DOM 子节点 | counter/quotes/URL content、扩展属性的 pseudo 路由和完整 pseudo CSSOM 未完成 |
| 原生 DOM mutation | C 插入/移除/属性/文本/分裂/销毁/关闭通知，UTF-16 范围与 UTF-8/WTF-8 数据转换；原生逐字节编辑保留其输入合同 | 全文档/跨 realm 生命周期仍需扩展 |
| Live Range | 原生和 JS 变更更新边界；compare、clone/delete/extractContents、insertNode、splitText、CharacterData | Selection 关联对象身份、geometry、surroundContents、Selection.modify；Document 级 insertNode 和跨文档明确拒绝 |
| 事件路径 | dispatch 时冻结路径；target capture 先于 bubble；shadow retarget、closed root 路径隐藏、非 composed 边界；dispatch 后清空路径 | slot 分配路径、relatedTarget 修剪未完成；未把旧事件压力崩溃问题标为已解决 |
| MutationObserver | 原生 CharacterData 与 splitText insertion；按 mutation 时 ancestry 选订阅；独立 oldValue、option 校验、多注册合并；JS/native 统一通知 job，保持 Promise/MO 次序 | 其他 childList/attribute 仍使用 JS producers；destroy/recycle 后未交付的记录可能失效，尚无完整节点保留/移除后 transient observer |
| Promise 拒绝 | 在下一任务边界报告，已 catch/await 的拒绝不再同步误报；支持晚处理 rejectionhandled 和事件取消 | 单活动页面；256 条有界记录；子 realm 需要独立所有权 |
| inert/native focus | 原生 click/Tab/程序 focus 共享焦点，动态 inert 使旧焦点失效；activeElement 读取 native holder | 未宣称完整输入法、Pointer Capture 或无障碍树 |
| 模态 dialog | 单一 native modal stack；移出普通流、viewport 布局、最高层单次绘制、实际遮罩、对应命中；限制焦点和 Tab，Escape cancel/close，关闭恢复焦点 | 默认固定遮罩；author ::backdrop、popover/fullscreen top layer、多文档 modal 未完成 |
| WebCrypto | 真 SHA-1/256/384/512；HMAC SHA-256/384/512；HKDF/PBKDF2 deriveBits/deriveKey；AES-GCM；私有密钥状态与错误/输入快照验证 | 见[专门报告](expansion-crypto-storage-2026-09-09.md)：RSA/EC、generateKey、SHA-1 HMAC/KDF、短 GCM tag 等仍明确拒绝；不是全算法支持 |
| localStorage | 真 guest 文件后端，版本/长度/CRC/generation 双槽，写后读回再发布内存成功；损坏回退、失败保留旧内存；session 不落盘 | 单写者；read -1 无法区分两个文件均缺失和 I/O 故障；写失败并不承诺已提交磁盘回滚；IDB/Cache 持久化未完成 |

## 验证入口与负控

```sh
make BUILD=build-expand test-browser-expansion
make BUILD=build-expand build-expand/logit.iso build-expand/disk.img
make BUILD=build-expand build-expand/wpt_test
python3 -m http.server 18769 --directory tests/fixtures/engine-expansion
# QEMU 内访问 http://10.0.2.2:18769/index.html
```

统一门禁覆盖实际 parser/CSS/layout/painter、QuickJS bindings、原生焦点、存储失败注入。每个组件的负控是正门禁 prerequisite；不是只列在 CI 行上的建议。独立 WPT runner 重新链接验证依赖接线，**不等于 WPT 全集或真实站点通过率**。

| 门禁 | 正向 | 已实际观察的负控 |
|---|---|---|
| css-extra-cascade | 22/22；首次旧实现 11 项失败 | 恢复旧级联，11 项失败 |
| generated-content | 54/54，另 ASan | 禁用 pseudo 48 项失败；恢复旧 flex blockification 2 项失败 |
| event-path | 8/8；首次旧实现 7 项失败 | 恢复实时路径 3 项失败 |
| live-range | 33/33，含 C 原生编辑更新 JS Range | 禁用原生更新 12 项失败，含 native insert/remove/UTF16 replacement |
| native-mo | 23/23 | 禁用 native records 11 项失败 |
| rejection-checkpoint | 12/12 | 恢复立即报告 7 项失败，含已 catch 的拒绝误报 |
| inert-focus | 23/23 | 禁用 inert 15 项失败 |
| modal | JS 14/14，native paint/hit/focus 16 项，ASan | 保留普通流、普通绘制顺序、允许背景焦点三个控制均实际失败 |
| web-digest / web-crypto-ops | 50 / 152 个共享页面向量与拒绝断言 | 篡改输出字节，独立预期向量失败 |
| storage-persistence | 后端 61、JS 16；独立文件进程重开；ASan/UBSan | 跳过实际写入，新进程报告 `FAIL: new process restores exact local bytes` |
| storage 既有回归 | backend 37、platform 3 | session partition 与错误类型控制实际失败 |
| interaction-runtime | 六段原生鼠标/Tab/程序 focus 与 painter 联合路径 | 静态 pseudo 控制导致 hover/active/focus 可见断言失败 |
| CSSOM ABI | item=264、node=256、image=16 | 错误 item stride 被检测 |

统一构建时还修复了测试器：interaction fixture 在 fetch 借用期间被提前 free，实际返回 allocator 字节；原“零 listener”断言把浏览器自身 focus listeners 算成页面代码，现核对 hover 元素并观察真实 paint。WPT 的新 AES 源需要 CPU include 路径；模块 loader 的文件后端缺 prefetch seam 导致 fresh link 失败，现补无异步队列的明确文件读取合同。

## Guest 证据

首次合成镜像的普通页面显示 SHA 50/50、WebCrypto 152/152、Range 23/23、interaction 3/3、splitText MO PASS；页面确实绘制出独立颜色的 BEFORE/AUTHORED/AFTER。串口也记录全部失败列表为空。该次在最终 MO 顺序修复前完成，最终镜像复核记录在下面，不能把两次代码视为同一个二进制。

本报告不使用 host wall time 给 browser 做性能结论。没有本批所有功能的 guest 前后镜像对照，之前/之后的差异目前来自 host 旧实现与被观察失败的 controls。真实公共站点兼容性还需要按同一 corpus 再跑，不能由此页面推出“大网站都可用”。

## 后续未完成版图

原组件总表仍有效：流式 HTML/script 调度、统一资源取消与策略、完整 CSS layers/文本/书写模式、SVG 与动画、Selection/编辑生命周期、子文档/Worker、权限和安全上下文、媒体/字体、IDB/Cache 持久化等仍是实质工程。这次没有用成功 stub 填满接口，也没有将总表全部勾选。优先继续以现有普通页面与负控证明实际消费者，而非扩大构造器数量。

### 统一门禁与客体补充

统一 `test-browser-expansion` 全部通过，183个测试fragment的182个已接入，另1个为声明过的独立wrapper。WPT binary完成fresh link，但本机未找到corpus；运行明确输出 `corpus not present ... nothing to measure`，未做WPT结果声明。

客体modal实际绘制在页面最高z-index元素之上；点击背景按钮没有增加计数，Escape移除遮罩并恢复Open dialog蓝色焦点框，见 `evidence/expansion/modal-open.png` 与 `modal-closed.png`。Tab循环的完整断言证据来自host门禁；客体截图仅证明已观察的输入步骤。

localStorage首次客体重开失败，根因已定位为测试器与LogitFS读文件合同不一致：host允许读32字节header前缀，guest在buffer小于整个文件时返回-1，旧实现把两个现存快照都当成缺失。旧失败截图保留，修复和重验另记。

### 主目录最终验收与清理

最终源码合回主目录后，删除旧 `build/` 并从零执行 `make -j8 build/logit.iso build/disk.img` 成功；随后主目录 `make -j6 test-browser-expansion` 全部通过。主目录并发新增的 WeakMap 修复及其 Makefile 接线被保留。新的统一测试目录总数以主目录日志为准（比隔离树多一个 WeakMap fragment）。

最终 guest 使用主目录新镜像：SHA 50/50、WebCrypto 152/152、Range 23/23、interaction 3/3、splitText MO PASS 均重验通过。存储写入 `guest-1788944759450`，关闭 browser 进程、重新启动、Enter 加载恢复标签后仍读取同一 LOCAL 标记，SESSION 为 absent；页面打开只读，不重造预期值。见 [最终页面](evidence/expansion/final-workbench.png)、[重开存储](evidence/expansion/final-storage-reopened.png)、[模态关闭](evidence/expansion/final-modal-closed.png)。这是浏览器进程重启证据，未把它扩写成断电恢复实验。

首轮存储失败已修复并有真实失败控制：恢复32字节header读取会在遵守LogitFS整文件合同的host后端失败；修复后一次读取有界最大快照。原失败与修复日志均归档。

按用户清理指令，删除31个零散build目录（枚举时占用合计约6.60 GiB），保留主目录唯一 `build/` 并重建最终镜像；本轮隔离工作树在逐文件核验已合入后删除。其他历史工作树的源码和既有分支未删。清理清单见 `evidence/expansion/cleanup.json`。实现仍有前述边界，没有把浏览器全部能力标为完成。


## Subsequent wiring/performance correction

The expansion's host/guest feature evidence above remains bounded as stated.
The subsequent [wiring and load audit](wiring-and-load-audit-2026-09-09.md)
found missing consumers/lifecycle calls and corrected them, then measured and
removed repeated SVG rasterisation, bootstrap DOM wrapping and simple-selector
JS traversal. That report records the fresh final main-disk build, controls,
release guest checks and the features still not connected to layout/paint.
