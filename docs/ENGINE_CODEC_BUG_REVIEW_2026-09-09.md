# 浏览器 engine 与解码库 bug 审查（2026-09-09）

本轮新增 **13 项：浏览器引擎/媒体接入 7 项，解码与容器库 6 项**。其中 **1 项 P1、12 项 P2**。与 [系统清单](BUG_REVIEW_2026-09-09.md)、[网络清单](NET_BUG_REVIEW_2026-09-09.md) 不重复。P1 优先处理高频路径的无效内存访问；P2 是明确的功能、像素、采样或时间戳错误。不是全项目无遗漏证明。

基线：`fe748e31e5d236cd74c1ca49f896b18170c7d54d` 加当前未提交工作树。三个子代理分查 DOM/CSS/layout、图片/视频、音频/容器，主代理检查 JS 定时器、媒体接入并复核证据。已读取 AGENTS.md、CLAUDE.md 并核对现有改动；**未修改实现，未提交代码**。

**证据范围：12 项有实际源码的主机探针结果，1 项为静态生命周期结论并经第二代理复核。** 其中 PCM24 验证使用从 Preview 自动摘取的原函数和实际 WAV 解码器，不等于运行整个 Preview。没有 guest 页面、真实声卡或完整浏览器端到端验收。测试只使用小型普通 DOM/CSS/媒体输入、本地缓冲和虚拟时钟，没有外部网络流量。

| ID | 优先级 | 模块 | 缺陷 |
|---|---|---|---|
| E01 | P1 | JS runtime | 一次性定时器释放后仍读取 `best->raf` |
| E02 | P2 | CSS bridge | 百分比 margin 丢失单位，变成裸像素 |
| E03 | P2 | layout | 负水平 margin 被当成 auto，双负边距反而居中 |
| E04 | P2 | DOM | 重复 ID 查询未返回当前树序第一项 |
| E05 | P2 | DOM | document.getElementById 错误查询 Shadow DOM 内部 |
| E06 | P2 | JS runtime | 严格模式定时器回调的 this 是 undefined |
| E07 | P2 | 浏览器音频接入 | 多声道 AAC 被以双声道源步长读取 |
| C01 | P2 | H264 | 左/上裁剪只改尺寸，输出像素起点未移动 |
| C02 | P2 | SVG | 子元素不能覆盖继承的 fill-opacity |
| C03 | P2 | JPEG/MJPEG | 拒绝合法的 FF marker 填充 |
| C04 | P2 | Matroska | 非整除 TimestampScale 回退纳秒时漏乘比例 |
| C05 | P2 | Matroska/Preview | PCM24 被当作 PCM16，帧数与波形错误 |
| C06 | P2 | Matroska | 忽略 BlockDuration，laced 帧获得重复 PTS |

## E01 — 一次性定时器释放后读取字段

位置：[js_page.c:767](/Users/wangzhe/system/LogitOS/c/apps/browser/js_page.c:767)、[js_page.c:770](/Users/wangzhe/system/LogitOS/c/apps/browser/js_page.c:770)，释放实现在 [js_page.c:608](/Users/wangzhe/system/LogitOS/c/apps/browser/js_page.c:608)。

普通 `setTimeout(callback, 0)` 或 requestAnimationFrame 到期后，`interval_ms==0` 路径调用 `timer_unlink(best)`、`timer_free(..., best)`。后者真正 `free(t)`，下一步却执行 `js_prof_label(best->raf ? ... : ...)`。setInterval 重挂路径不经过这次释放。

函数引用和回调参数已提前保留，但遗漏了还会使用的 raf 标记。**直接确认的失效读取仅用于 profiler 标签选择**；这是 use-after-free/未定义行为，不据此声称回调必定执行错误、guest 崩溃或存在可利用结果。关闭 profiler 也不能从源码上消除参数表达式，具体优化产物未核查。

修复方向：释放前保存所需标记或先计算标签，回调期间继续持有当前已有的独立 JS 引用。不能仅把 timer_free 移到回调后，因为回调本身可以取消 timer。此项为两名代理独立静态核查，未执行一次性定时器的无效读取复现。

## E02 — 百分比 margin 在 CSS 到布局的转换中丢失

位置：[css_engine.c:1070](/Users/wangzhe/system/LogitOS/c/apps/browser/css_engine.c:1070)、[css_engine.c:1178](/Users/wangzhe/system/LogitOS/c/apps/browser/css_engine.c:1178)。

四个 margin 调用 `len_px(..., NULL)`。遇到百分比时，该函数返回百分比裸数值，调用者没有保存单位，layout 按像素使用。普通父容器内容宽 400/800px、边框 1px，子元素 `margin:10%;width:100px;height:20px`：

```text
percent400: x=11 y=11 w=100 h=20 ml=10 mr=10 mt=10
percent800: x=11 y=11 w=100 h=20 ml=10 mr=10 mt=10
```

正确坐标分别应为 41、81。包含块宽度翻倍，边距不变，排除了四舍五入解释。[CSS Box Model](https://www.w3.org/TR/css-box-3/#margin-physical) 规定百分比 margin 依据包含块的逻辑宽度解析。

产品链：browser.c 的 `css_apply → layout_page`，CSSOM 也读取布局框。主机探针链接实际 CSS 选择、桥接和 layout；案例没有文本/图片，不依赖测试字体替身。建议保留百分比及精度，在包含块尺寸已知时计算，而不是复用像素整数。

## E03 — 负水平 margin 与 auto 共用表示并被错误消费

位置：[layout.c:2714](/Users/wangzhe/system/LogitOS/c/apps/browser/layout.c:2714)、[layout.c:672](/Users/wangzhe/system/LogitOS/c/apps/browser/layout.c:672)。

布局把任何负左边距当 0，左右都负时直接执行 auto 居中；宽度计算也忽略负值。同一 400px 父容器、1px 边框、100px 子元素，实际结果：

```text
margin:0       x=1    ml=0   mr=0
margin:10px    x=11   ml=10  mr=10
margin:-10px   x=151  ml=-10 mr=-10
margin:auto    x=151  ml=-1  mr=-1
```

负边距应使子元素左移到 -9，却居中了。计算样式中 -10 仍存在，缺陷发生在布局消费端。[CSS 允许负 margin](https://www.w3.org/TR/css-box-3/#margin-physical)。

修复不能只把 `<0` 改成 `==-1`：桥接层用 -1 表示 auto，会与合法 -1px 冲突。应单独保存 auto 状态，并在宽度、定位、flex/grid 等消费处统一区分。实际验证为盒几何，未做 guest 像素验收。

## E04 — ID 索引顺序替代了 DOM 树顺序

位置：[dom.c:330](/Users/wangzhe/system/LogitOS/c/apps/browser/dom.c:330)、[dom.c:1121](/Users/wangzhe/system/LogitOS/c/apps/browser/dom.c:1121)，JS 入口 [js_dom.c:470](/Users/wangzhe/system/LogitOS/c/apps/browser/js_dom.c:470)。

新增 ID 节点插入 hash 桶首；查询遇到第一个连接中的匹配项即返回，没有按当前树序比较。用普通 DOM 操作创建 A/B，设置相同 ID 并依次追加，实际树序 A 在前，但查询返回 B；将 B 移至前面时恰好正确，再将 A 移回首位，查询仍是 B。

```text
duplicate_id first_is_a=1 lookup_is_a=0 lookup_is_b=1
duplicate_id reordered_first_is_b=1 lookup_is_b=1
duplicate_id restored_first_is_a=1 lookup_is_a=0
```

页面克隆组件或重排时可能临时产生重复 ID；即使文档作者应保持 ID 唯一，[DOM 查询规范](https://dom.spec.whatwg.org/#dom-nonelementparentnode-getelementbyid)仍明确要求返回匹配后代中树序第一项。建议候选按树序选择，或在所有树变更路径维护顺序正确的索引。已验证实际 C DOM；JS 入口由调用链核查，未运行完整浏览器。

## E05 — document 级 ID 查询误入 Shadow DOM

位置：[dom.c:1092](/Users/wangzhe/system/LogitOS/c/apps/browser/dom.c:1092)、[dom.c:1125](/Users/wangzhe/system/LogitOS/c/apps/browser/dom.c:1125)，attachShadow 入口 [js_dom.c:3997](/Users/wangzhe/system/LogitOS/c/apps/browser/js_dom.c:3997)。

shadow root 的 parent 指向 host，文档级索引查询只沿 parent 判断是否连通。因此位于 shadow tree 内部的唯一 ID，也被当作文档普通后代返回。

```text
shadow_id found_in_document=1
detached_shadow_id_found=0
```

探针把 open shadow root 挂在 body 内的 host 上，内部放置唯一 ID；移除 host 后查询才消失。[DOM getElementById](https://dom.spec.whatwg.org/#dom-nonelementparentnode-getelementbyid)的作用域是普通后代树，不是 shadow-including 后代树。

旧注释认为沿 shadow root→host 判断连接性也适合 getElementById；应保留这个旧理由并纠正：isConnected 与查询作用域是不同问题。建议文档查询遇到 shadow 边界停止，shadow root 查询在自己的树内进行。本项与 shadow 内容尚未完整绘制的已知限制无关；已验证实际 DOM 查询结果。

## E06 — 严格模式 timer 回调没有以 window 为 this

位置：[js_page.c:772](/Users/wangzhe/system/LogitOS/c/apps/browser/js_page.c:772)。

`JS_Call` 对 timer 回调传入 `JS_UNDEFINED` 作为 this。宽松模式函数可能由 JS 引擎自动转换为 global，这会掩盖问题；严格模式保留 undefined。[HTML 定时器算法](https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timer-initialisation-steps)要求 Window 定时器回调的 this 为对应 WindowProxy。

实际 js_page + QuickJS 探针，注入时钟，运行一次 interval 并在回调中取消自身：

```javascript
window.id = setInterval(function () {
  'use strict';
  window.seen = this === window;
  clearInterval(id);
}, 10);
```

```text
strict_interval_this_is_window=false
callbacks=1 pending=0
```

回调确实执行且成功取消，不是没有触发 timer。该探针只使用 interval，不经过 E01 的一次性释放分支。建议按回调接口传入正确 global this，并核对 window/worker 两类环境；不要靠函数是否严格模式决定浏览器接口行为。

## E07 — AAC 多声道输出在浏览器转换中被错误交错

位置：[js_media_src.c:1463](/Users/wangzhe/system/LogitOS/c/apps/browser/js_media_src.c:1463)、[js_media_src.c:1491](/Users/wangzhe/system/LogitOS/c/apps/browser/js_media_src.c:1491)，解码器契约 [aac.h:55](/Users/wangzhe/system/LogitOS/c/lib/audio/aac.h:55)。

AAC 支持最多 8 声道，PCM 为 `nsamples * channels` 的 interleaved 数据。浏览器将局部 ch 截为 2，随后却用截后的 ch 作为源数组步长读取 `f[i*ch+c]`。ASC 打开处未拒绝多声道，后续也没有 downmix 或步长修正。

主代理生成 0.1 秒 48kHz、六个不同频率的 5.1 AAC/M4A，链接实际媒体接入、容器、音频/视频、图片/Rust 库，以内存声卡捕获首个输出块，并和同一实际 AAC 解码器直接输出比较：

```text
load=0 decode_channels=6 device_channels=2 capture_samples=2048 compare=2048
flat_prefix_mismatches=0 first_two_channels_mismatches=2021
```

实际输出逐样本等于六声道数组的前 2048 个连续元素，被误认为 1024 个双声道采样时刻；它既非按原始步长取前两声道，也没有进行混音。3–8 声道受影响，单/双声道不受此步长缺陷影响。不是越界读，也不仅是“丢弃环绕声道”。

建议分开保存源声道数与输出声道数，使用明确的声道映射/混音规则；不能完成时应拒绝该布局。已验证实际送入声卡接口的 PCM，未运行真实声卡或做听感测试。

## C01 — H264 左/上 crop 没有移动输出平面起点

位置：[h264.c:498](/Users/wangzhe/system/LogitOS/c/lib/video/h264.c:498)、[h264_dpb.c:538](/Users/wangzhe/system/LogitOS/c/lib/video/h264_dpb.c:538)。

解码器按四边 crop 总量缩小 width/height，却直接输出未偏移的 `p->y/u/v`。API 已承诺返回应用 SPS crop 后的可见区域，消费者没有另外拿到应偏移的裁剪信息。

用同一个 32×32 Baseline 4:2:0 编码帧，FFmpeg `h264_metadata` 仅增加合法 `crop_left=16:crop_top=2`，可见尺寸 16×30。实际解码器链接现有 h264_diff，与 FFmpeg 输出比较：

```text
plain:   frames 1, bad frames 0, bytes wrong Y=0 U=0 V=0 total=0
crop-lt: frames 1, bad frames 1, bytes wrong Y=480 U=120 V=120 total=720
```

尺寸正确，但全部输出字节来自错误区域。Preview 和浏览器 `h264_decode_pts → mel_emit` 均使用这些平面。建议按每张输出 picture 对应的 crop 信息调整平面起点，保留原 stride，并确保重排序时参数仍属于该 picture。未据此声称只有右/下裁剪也出错；主机像素差分已证实，未运行 guest 视频。

## C02 — SVG 把继承属性提前乘进颜色 alpha

位置：[svg.c:453](/Users/wangzhe/system/LogitOS/c/lib/image/svg.c:453)、[svg.c:468](/Users/wangzhe/system/LogitOS/c/lib/image/svg.c:468)。

`apply_opacity` 乘上已继承的 paint.a；子元素显式声明 fill-opacity 时又乘一次，无法替换父属性。合法 2×2 SVG 根元素 `fill-opacity="0.5"`、红色矩形子元素 `fill-opacity="1"`：

```text
flat-fill-opacity:     rc=0 px=255,0,0,255
override-fill-opacity: rc=0 px=255,0,0,127
```

平面等价对照为不透明红，继承版本却半透明。[SVG fill-opacity](https://www.w3.org/TR/SVG11/painting.html#FillOpacityProperty)是可由子元素覆盖的继承属性。浏览器内联/外部 SVG 与 Preview 都通过 img_decode 使用此实现。

建议颜色自身 alpha、fill/stroke opacity 与组 opacity 分开保存，完成继承/覆盖后再按绘制与组复合语义应用。父 transparent 被子 red 覆盖仍透明的现象也被观察到，但按同一状态表示问题合并，没有增加计数。已验证实际 img/svg/gfx 输出像素；未做参考浏览器或 guest 渲染。

## C03 — JPEG/MJPEG marker 遍历漏掉合法 FF 填充

位置：[jpeg.c:710](/Users/wangzhe/system/LogitOS/c/lib/image/jpeg.c:710)、[mjpeg.c:84](/Users/wangzhe/system/LogitOS/c/lib/video/mjpeg.c:84)。

解析器认为第一个 FF 后的字节必定是 marker 类型，没有跳过 marker 前可重复出现的 FF 填充，进而从错误位置取段长度并拒绝文件。[ITU-T T.81 Annex B.1.1.2](https://www.w3.org/Graphics/JPEG/itu-t81.pdf)允许这种填充。

普通 32×32 JPEG，只在 SOI 后、APP0 marker 前增加一个 FF。libjpeg `djpeg -nosmooth -dct int` 均成功，两个输出 PPM 用 cmp 比较完全一致。实际 LogitOS 结果：

```text
regular.jpg:     IMAGE rc=0  size=32x32; ANIM rc=0;  MJPEG framing=794 start=0
fill-marker.jpg: IMAGE rc=-1 size=0x0;   ANIM rc=-1; MJPEG framing=-1 start=-1
```

浏览器和 Preview 图片入口都受影响，MJPEG 帧边界 API 也有同类遗漏，合并一项。建议统一处理 marker 前的 FF run，并保持熵编码数据中的 FF00 stuffing 与 restart marker 规则独立。没有内存破坏或 guest 崩溃结论。

## C04 — Matroska 回退纳秒时只改单位标签，未转换数值

位置：[mkv.c:495](/Users/wangzhe/system/LogitOS/c/lib/media/mkv.c:495)、[mkv.c:287](/Users/wangzhe/system/LogitOS/c/lib/media/mkv.c:287)、[demux.c:312](/Users/wangzhe/system/LogitOS/c/lib/media/demux.c:312)。

TimestampScale 不能整除 1e9 时，代码将 timescale 设为 1e9 并宣称使用 ns；cluster/block 时间戳却仍保存原 tick 数值。普通合法文件的 TimestampScale=3,000,000ns、cluster timestamp=1000：

```text
control scale=1000000: ticks=1000 pts_ns=1000000000
case    scale=3000000: ticks=1000 pts_ns=1000
ffprobe case: 3.000000 seconds
case container duration_ns=3300000000
```

文件时长正确为3.3秒，帧时刻却只剩1000ns。[Matroska 时间单位说明](https://www.matroska.org/technical/notes.html)要求按 TimestampScale 缩放。Preview 视频消费 `s.pts_ns → avclock_frame`，错误通用变换会影响播放调度。

保留并纠正旧注释“fall back to nanoseconds and let the timestamps be ns”：当前只改了单位，没有完成转换。建议统一有理时间基或把 block/cluster/default duration/delay 全部规范化，避免只修最终某个乘法。本轮 PCM 样本证实 API 时间变换，不等于已复现 guest 视频不同步。

## C05 — PCM24 Matroska 被伪装成 PCM16 交给 WAV 解码

位置：[mkv.c:160](/Users/wangzhe/system/LogitOS/c/lib/media/mkv.c:160)、[mkv.c:256](/Users/wangzhe/system/LogitOS/c/lib/media/mkv.c:256)、[preview.c:486](/Users/wangzhe/system/LogitOS/c/apps/gui/preview.c:486)。

容器读到 BitDepth=24，仍把 `A_PCM/INT/LIT` 一律标记为 PCM_S16LE。Preview 仅按 codec 接受，再固定生成 bits=16、blockAlign=channels*2 的 WAV 头，复制原始24位数据。

ffmpeg 生成 8kHz mono、0.1秒 PCM16/24 对照。动态摘取 Preview 原 `put32` 至 `aplay_assemble` 函数，链接实际 demux 与 wav.c：

```text
pcm16: declared_bits=16 assembly_rc=0 wav_rc=0 decoded_bits=16 frames=800  duration_sec=0.100000
pcm24: declared_bits=24 assembly_rc=0 wav_rc=0 decoded_bits=16 frames=1200 duration_sec=0.150000
```

2400字节的800个24位采样被解成1200个16位采样，波形错位、时长增加50%。这不是单纯未支持 PCM24，而是错误接受并解释内容。建议 codec/样本格式与 BitDepth 一致，消费者验证契约，正确转码或明确拒绝。

实际产品链为 `play_container → aplay_open_track → aplay_assemble → adec_open`。探针使用原函数摘取与只包含所需字段的 aplay 结构，未运行整个 GUI 或声卡；不能标为 Preview 端到端测试。

## C06 — 忽略 BlockDuration，laced 帧时间戳不递增

位置：[mkv.c:404](/Users/wangzhe/system/LogitOS/c/lib/media/mkv.c:404)、[mkv.c:410](/Users/wangzhe/system/LogitOS/c/lib/media/mkv.c:410)、[mkv.c:373](/Users/wangzhe/system/LogitOS/c/lib/media/mkv.c:373)。

BlockGroup 只处理 ReferenceBlock 与 Block，跳过 BlockDuration。lacing 帧时间只能依赖 Track DefaultDuration；缺少它时步长为0，即使块已声明合法总时长。

合法固定 lacing PCM：3个64字节帧，8kHz/16bit mono，每帧4ms，BlockDuration=12 ticks。实际库得到 1.000/1.000/1.000秒，ffprobe 为1.000/1.004/1.008秒。改成 Track DefaultDuration 的控制样本，本库也正确递增。

[Matroska BlockDuration](https://www.matroska.org/technical/elements.html#BlockDuration)提供块的时长。建议读取并使用块时长/各帧时长关系，在缺失 Track DefaultDuration 时仍正确赋予 laced 帧时间戳，并统一时间单位。

本项已确认的是 demux API 返回错误 PTS。**当前 Preview 拼音频只取数据、不使用音频 PTS，所以不能把这份 PCM 探针写成可听故障。** Browser MSE 入口目前按 MP4 box 扫描，也不能借它声称 Matroska 已在浏览器播放失败。

## 验证与覆盖边界

完整临时证据根目录：[logitos-engine-codec-20260909](/tmp/logitos-engine-codec-20260909)。

- `browser/probe.c`、`repro.sh`、`probe.log`、`sources.json`：DOM/CSS/layout 实际源码探针；现有 layout_box_test 为 **51 checks、0 failures**，没有覆盖新增边界。
- `main/timer_probe.c`、`audio_stride_probe.c`、`audit.mk`、`repro.sh`、两个功能日志及 `sources.json`：实际 runtime/AAC 消费验证。make 变量读取前已合并续行；隔离使用 `BUILD=build-audit-runtime-0909`，Rust 也使用独立 target-dir。
- `image-video/reproduce.py`、`results.json`、`reproduce.log`、`source-manifest.json`：JPEG/SVG/H264 样本生成、完整编译命令、参考输出及源码指纹。H264 裁剪差分 **exit=1**，无裁剪对照 **exit=0**，失败被确实观察到。
- `audio-media/reproduce.sh`、`probe.log`、`preview_probe.log`、`ffprobe.log`、`SHA256SUMS`：Matroska 时间/PCM 对照、Preview 函数摘取与 WAV 解码结果。

观察型探针退出0表示完成观测，不表示实现通过正确性检查。主代理复跑了 timer 和 AAC 消费探针，结果一致；合并报告时复核源码指纹，避免把不同工作树时刻的结果混在一起。临时文件可能被系统清理，关键触发与输出已保留在本文。

**范围限制：**这是沿实际调用链的定向审查，不是每个解码器、每个 profile 或所有 QuickJS 执行路径的完整覆盖。图片探针没有实现 Rust PNG/BMP/ICO/WebP 注册，不能用它评价这些格式；AAC 播放探针虽链接全媒体库，也不等于已测试所有 codec。未运行完整 ISO/磁盘构建、全量 CI、WPT 或 guest 像素/音频测试。

已排除 GIF 循环次数候选（参考行为未支持最初假设）、明确记录为不支持的 Opus SILK/hybrid 和其他格式能力，以及已说明的 Vorbis/MP3 首尾 trimming 缺失。本轮没有用这些已知限制增加 bug 数量，也未把 H265/VP9 的旧文档状态当作新发现。
