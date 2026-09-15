# Native Wasm call diagnostics

2026-09-11。仅 `JS_RUNTIME_DIAGNOSTICS` 构建在普通导出函数的 `wasm_invoke` 边界输出固定元数据。`tests/runtime_diagnostics.mk` 已把 `js_wasm.o` 接到现有 `RUNTIME_DIAGNOSTICS=1` 配置，默认构建不产生这些行。

```text
[runtime-diag] wasm-call id=1 phase=begin
[runtime-diag] wasm-call id=1 phase=end elapsed_ms=0 ok=1 failed=0
```

编号在进程内递增，最多记录 128 对、256 行；开始时预留整对，达到上限也不会单独隐藏已记录调用的 end。嵌套调用用 id 配对，end 不必按编号排序。时钟复用浏览器的 `js_page_now_ms`，没有读取 JavaScript 对象属性或异常内容。记录不含参数、返回值、函数/模块名称或索引、内存内容、URL 或页面数据。

`ok` 仅表示解释器返回 `WASM_TRAP_NONE`，`failed` 是其布尔反值；不代表后续 JavaScript 结果封装已成功。参数转换在 begin 之前，原内存/table 同步与异常返回在 end 之后，均未改动。耗时含 begin 日志开销，用于诊断是否仍在调用中以及何时返回，不作为性能基准；日志上限之后也不能用缺少 begin 判断没有调用。

验证命令：`make BUILD=build-parent-fetch-fix test-worker-wasm-diagnostics`。复用原有普通页面与两个 Worker 的加法 42 夹具，开关关闭与开启均 7/7；开启时得到 6 组匹配的调用记录，关闭时没有此类记录。checker 对整个新日志行做严格格式匹配，不允许额外字段。日志为 `build-parent-fetch-fix/worker-wasm-normal/diagnostics-off.log` 与 `diagnostics-on.log`。没有运行 trap、GC、故障、内存失效或站点用例，也不声称实际执行验证了 failed=1 分支。

独立只读审阅确认观察器仅使用饱和计数、本地时钟与固定格式输出，原执行器、`s->active` 恢复、内存同步、host exception 返回和生命周期没有变化。生产改动仅 `c/apps/browser/js_wasm.c` 的宏区与 `jw_call` 两侧；其他改动是构建宏接线和正常验收 gate。
