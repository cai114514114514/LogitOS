# M17 — LibCSS + 渲染管线移到 ring-3（CSS 换第三方，HTML 加深）

## 动机
自写的 `net/css.c` 太简陋，要换成像样的引擎；HTML 解析（`net/dom.c`）也要加深。
唯一像样的**纯 C** CSS 引擎是 NetSurf 的 **LibCSS**（litehtml 是 C++，本环境难）。

## 架构决策（已定）
**把整个渲染管线从内核搬到 ring-3 browser app**（方案 B）：
- `browser.aex` 里：`dom.c`(加深) + LibCSS + `layout.c` + paint(走 gui_*) + QuickJS。
- 内核退化为原语提供者：网络 fetch、字体度量、绘制（gui_*）。
- **一举解锁 JS↔DOM 绑定**（DOM 和 JS 同在 ring-3），并直接复用已有的 mini-libc。

## 已验证（本里程碑起步）
- LibCSS 依赖链：**libwapcaplet**(292 行) + **libparserutils**(5369 行) + **libcss**(40901 行)。
- 系统头依赖 `string/assert/stdio/stdint/stdarg/inttypes/sys/types` —— mini-libc 全覆盖
  （补了 `user/libc/include/sys/types.h`）。
- **libwapcaplet 与 libparserutils 核心文件已用 mini-libc 成功编译**（`third_party/css/`）。
- 代码生成：libcss 用 `build/mkprops.pl`（host perl）生成 property 表，构建期跑。

## 分阶段
- **L1 — 管线下放脚手架（不换引擎，先验证可行）**：新 syscall `SYS_HTTP_BODY`
  (把 raw HTML body 拷给 app) + `SYS_TEXT_MEASURE`(字体度量)；browser app 链接现有
  `net/dom.c`+`net/css.c`+`net/layout.c`，paint 用现成 `gui_rect/gui_text/gui_blit`
  重写。目标：example.com 在 ring-3 管线渲染，和现在内核渲染一致。内核 SYS_PAGE_* 退役。
- **L2 — 集成 LibCSS**：把 libwapcaplet+libparserutils+libcss 编进 browser（含 perl
  代码生成的 Makefile 规则）；写 `css_select_handler` 把我们的 DOM 适配给 LibCSS；
  `layout.c` 改从 `css_computed_style` 读样式，删 `net/css.c`。
- **L3 — 加深 HTML（自写）**：`dom.c` 增强到更完整的 HTML5（隐式 tbody/head/body 插入、
  错误恢复、更多具名实体、属性解析），喂饱 LibCSS 选择器（class/id/属性选择器）。
- **L4 — JS↔DOM 绑定**：QuickJS 里建 document/element 对象，`getElementById`/
  `querySelector`/`textContent`/`innerHTML`/`style`，改 DOM 后触发重排重绘。

## 风险 / 注意
- `text_measure` 若每个词一次 syscall 会很慢 → 要么批量度量，要么字体度量也下放 ring-3
  （把 TTF 引擎 `lib/ttf.c`+`kernel/raster.c` 编进 app，用 SYS 读字体文件）。后者更彻底。
- LibCSS 的 perl 代码生成要进 Makefile（host 依赖 perl）。
- 管线下放后内核 `net/{dom,css,layout,paint}.c` 退役（删或留作内核内自测）。
- browser.aex 会更大（已含 QuickJS ~1MB，再加 LibCSS）。disk/内存够（QEMU 512M）。

## 验证
每层：example.com / wikipedia 在 ring-3 管线渲染对比内核版；host 端 LibCSS 选择器/级联
单测（用 LibCSS 自带 test/ 数据）；JS 改 DOM 后页面可见变化。
