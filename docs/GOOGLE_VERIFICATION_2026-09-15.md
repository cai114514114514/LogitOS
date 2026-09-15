# Google 验证与搜索：未通过验收（2026-09-15）

## 目标与当前结果

目标是 LogitOS 自己的浏览器显示可操作的人机验证，用户手动完成后显示真实搜索结果。本轮**没有完成这个目标**，没有验证码通过、搜索结果或整站兼容的证据。

复测采用正常 User-Agent、原有网络和普通 `q=LogitOS` 搜索。未修改标识、Cookie、验证代码或服务器返回值，未代答验证码。不得把 HTTP 200、像素变化、少一个异常或单元测试通过当作验证通过。

## 已实现的通用修复

`browser.c` 原先将所有经典脚本（包括 async/defer）按文档顺序执行，再执行模块。本地普通回调页面证明：外部 async 的 load 处理器可能在后面的解析期脚本声明回调之前运行；经典 defer 也被错误提前。

- 资源准备时记录执行阶段，不让后续属性修改改变已准备任务。
- 当前整批加载器先执行阻塞脚本与 import map，再执行已就绪 async 批次，最后按文档顺序混排经典 defer 和非 async 模块。
- 内联经典脚本忽略 async/defer；外部经典脚本的 async 优先于 defer。
- 新的私有解析完成回调在延迟执行阶段前更新 `readyState=interactive`，不提前派发 DOMContentLoaded；回调引用在页面关闭时释放，调用经过现有 watchdog 和关闭检查点。

范围限制：仍先解析、整批获取资源，尚非流式解析与按下载完成时间调度的完整 async 实现。此次未实现独立的联网 iframe 脚本环境。

标准依据：[HTML script 处理模型](https://html.spec.whatwg.org/multipage/scripting.html#the-script-element)。

## 本地与构建证据

```sh
make BUILD=build-google-home/work test-parser-script-order
make BUILD=build-google-home/work test-parser-script-events test-frame-bootstrap-wiring test-legacy-home
make BUILD=build-google-home/work test-mk-wired
make BUILD=build-google-home/work build-google-home/work/browser.aex
```

`tests/unit/parser_script_order_test.c` 驱动真实 app_main / DOM / QuickJS / 加载器，网络为普通本地 fixture。修改前 5 项失败；最终 9 项全部通过，含属性修改、移动已准备的延迟脚本、回调一次性与生命周期。强制旧顺序的负对照仍准确失败 5 项，并作为正测试前置条件。

既有脚本事件、iframe 启动、首页布局 95 项通过，各自负对照按预期失败。`test-mk-wired` 本次为绿：362 个片段，361 可达，1 个显式例外（共享工作树中的接线状态已不同于上一轮记录）。

浏览器与隔离系统盘已重建；仍使用前一轮已验证的 ISO，不宣称当前全仓库/内核构建通过。mkfs 保留 5 个用户状态 inode。日志位于 `build-google-home/verification/`。

## 真实客机观察

1. `guest.json`：修改前重新启动客机，仍有 `solveSimpleChallenge` 未定义与 passive iframe 的两个异常。
2. `after.json`：修改后第一次测试在联网前出现 `no source address for any dst`；这是 FETCH-FAIL，不是 Google 拒绝，也不是成功。
3. `retry.json`：重启客机后联网成功。首先收到 HTTP 200 的脚本中间页，网页自身捕获 `U is not a function (it is the number 0)` 并发起带错误信息的导航。随后收到异常流量拒绝页面，内容要求稍后重试；该页面未提供可操作的验证码，且仍有 `solveSimpleChallenge` 未定义。已查看最终截图。

**最终 1 个异常不是“修掉了其中 1 个”的证据**：服务端返回了不同页面。当前不能把本地脚本顺序修复认定为已解决 Google 中间页错误；未尝试逆向或改写服务端验证程序。

截图：`build-google-home/verification/google-search-order-retry.png`。复现命令：

```sh
python3 tests/qmp/qmp_site.py \
  --iso build-google-home/work/logit.iso --disk build-google-home/work/disk.img \
  --name google-search-order-retry --url 'https://www.google.com.hk/search?q=LogitOS' \
  --out build-google-home/verification/retry.json --boxes --images --keep
```

## 尚未实现的必要能力

`passive_frame.c` 明确只实现独立 DOM、CSS、布局与图片预览，禁用脚本、子页面表单及输入。`js_frame.c` 的同源兼容 realm 不能代替它：缺少完整 DOM、网络脚本、计时器、可见绘制及跨源通信。

需要真正的子页面执行所有权、生命周期与事件循环，然后接入绘制/命中测试/键盘路由，以及有来源检查的 WindowProxy 与 postMessage。不能把子页面交给父页面全局 DOM，不能把 contentWindow 的拒绝改成父 window，更不能制造验证成功回调。同源/跨源、sandbox、导航后失效和 teardown 必须有独立负对照。

即使这些兼容能力完成，最终还需要 Google 实际提供验证码、用户亲自操作及服务器返回搜索结果，才能验收。当前拒绝页不是用户可接手的验证界面。

## 本轮最终产物 SHA-256

- `build-google-home/work/browser.aex`: `25ee2aa2305276a515d7092c3f50ac5cc07b95b02187ec9a0bc69c331b421a23`
- `build-google-home/work/disk.img`: `9fd7c6b2ec2898953fa57a979093d6f2659bea169dadc34a130e5be2a7e1953e`
- `build-google-home/work/logit.iso`: `0f80979f37e8a17e590b45e7579f26231ae137607f99047c2f9926784190ec73`

上一轮 GOOGLE_HOME 文档中的 work 指纹是历史验收值；该工作目录本轮已更新，历史截图与 JSON 保留。
