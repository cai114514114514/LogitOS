# LogitOS 官网

独立的中文静态官网，无运行时依赖。使用 Node.js 18+。

```sh
cd website
npm run dev
# http://127.0.0.1:4173
```

```sh
npm run check
npm run build
npm run preview -- --port 4174
```

`dist/` 可用于静态托管。开发与预览服务只监听本机，并只提供官网文件。
本次不修改操作系统构建入口，不启动或操作 QEMU，不发布网站。

## 目录

- `index.html`：真实文案和语义结构，禁用 JavaScript 时正文仍可读。
- `sections/`：桌面展示、架构、图形、语言、系统能力、项目方式及验证说明的独立 HTML 章节。
- `verification.html`：原生任务与各子系统证据的阅读入口。
- `styles/tokens.css`：颜色、字体、可访问性基础。
- `styles/layout.css`：导航、章节、响应式布局。
- `styles/product.css`：应用导览、任务流程、终端与弹层。
- `styles/chapters.css`：图形展示、语言源码、能力目录及原生 FAQ。
- `tour.js`：应用导览数据、键盘切换、截图放大。
- `app.js`：复制命令和章节导航状态。
- `motion.js` / `styles/motion.css`：分组入场、阅读进度和交互过渡；动态偏好切换时取消正在运行的动画。
- `scenes.js` / `styles/scenes.css`：固定视口的桌面展开、任务侧栏特写和架构分层；实际滚动位置决定画面，支持反向滚动、阶段按钮及完整静态布局。
- `scripts/`：Node 标准库构建与本地预览。
- `assets/README.md`：真实系统截图来源及公开发布前的素材事项。
- `DESIGN.md`：视觉与内容约定。

开发服务与构建共用 `scripts/render.mjs`，把 `<!-- include:章节名 -->`
展开成完整 HTML。最终产物无需 JavaScript 才能阅读这些章节，也不需要线上 Node
服务。缺少章节会让构建明确失败。请通过开发服务或 `dist/` 预览，不要直接打开
尚未展开的源 HTML。

## 验证范围

官网交互需要在宿主浏览器检查；这不等同于 LogitOS 自带浏览器兼容性验证。
系统能力文案参考当前 README、CLAUDE.md 和原生模型任务验收报告，未重新运行
操作系统完整测试。没有虚构可下载版本或将实验系统标为正式发布。
