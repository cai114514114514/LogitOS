# SVG 动态 DOM/CSS 桥接的真实 guest 验收

本项使用主任务冻结的 `build-ds-render-fixed/snapshot` 渲染版本，在独立 QEMU 中运行普通本地 HTML。初次显示后，真实鼠标点击修改 DOM 属性和 CSS；不调用 getBoundingClientRect、offset/client 几何读取、JS 滚动或专用 reflow。新旧两次运行使用同一份 fixture 字节。

新增可复用驱动 `tests/qmp/svg_dom_guest.py`。页面 4×4 独特色块定位实际 viewport，像素采样不依赖窗口边框高度；同一个 QMP Session 用串口指针位置确认点击。QEMU 启用 `-snapshot`，使用独立 socket，`restrict=on` 加唯一 `guestfwd`，只有脚本启动的 localhost HTTP fixture 可达。该配置依据 [QEMU guestfwd 文档](https://www.qemu.org/docs/master/system/invocation.html)，不请求 DS 或访问用户 VM。

## 结果

| 场景 | 当前冻结版本 | 旧冻结版本 |
|---|---|---|
| 静态 SVG 正控制 | 绿色 | 绿色 |
| JS createElementNS + setAttribute | 完整绿色 32px 图标 | 空白 |
| 已解析 rect 的 fill 改色 | 红色变绿色 | 仍红色 |
| 改 class 后 CSS fill 覆盖 presentation fill | 红色变绿色 | 仍红色 |
| CSS color 改变 currentColor | 红色变绿色 | 黑色 |
| 精确 viewBox 更新及自动重绘 | 完整蓝色 32px 图标 | 仍为原小图形 |
| 24px 零 padding 原生 button 内 SVG | 完整 24px | 缩小并偏移 |
| 12px 零 padding 原生 button 内 SVG | 完整 12px | 几乎不可见 |

当前 8/8 场景、16 个采样像素全部命中。旧版本 1/8，7 项失败；反控额外要求静态 SVG 在点击前后都正确，以及动态创建与 fill 改色明确失败。两次初始截图都确认待变更图形原为红色，防止一个已是最终颜色的 fixture 冒充重绘成功。点击位置均为屏幕 `(238,488)`。

viewBox 的普通 JS 读回同时检查更新值、没有小写 alias、属性数量未增加：当前 PASS，旧版本 FAIL。这个检查只读取 DOM 属性，不会强制布局。实际截图也经人工查看，确认新版本的完整图标、修改前后的红绿变化、旧版本的空白与缩小图形。

## 复用与证据

```sh
python3 tests/qmp/svg_dom_guest.py --snapshot build-ds-render-fixed/snapshot --out build-ds-render-fixed/svg-dom-guest-final
python3 tests/qmp/svg_dom_guest.py --snapshot build-terms-layout-evidence/snapshot-final --out build-ds-render-fixed/svg-dom-guest-old --expect-old
```

两处输出目录各含 `fixture.html`、`before.png`、`after.png`、`serial.log`、`qemu.log` 和 `results.json`。结果包含 fixture 和输入文件 SHA256、真实点击位置、逐项像素和私有进程停止状态。两次进程均已终止，冻结 ISO/disk 的前后哈希一致。首轮未接 guestfwd 的测试装置启动已主动结束，未作为产品正反证据；上表只使用完整配置的 final/old 两次结果。

当前 browser.aex SHA256：`ab4caaa03e8210d7dd0ba2c3f5f955410b341b3e02504b4eb396f80918c669ad`。本页仅证明普通本地 JS → DOM/CSS dirty → layout → SVG 像素的自动显示链，以及这两个原生按钮尺寸；不证明真实 DS 请求、登录或聊天已成功。相关 HTML/SVG 属性大小写修复见 `foreign-attribute-case-fix-2026-09-11.md`。
