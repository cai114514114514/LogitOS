# HTML 与外部命名空间的属性大小写

SVG DOM 图像桥接验收暴露了共享 DOM 缺陷：HTML tree builder 已将 `viewbox` 调整并存储为 `viewBox`，但 `dom_attr` 查询再次统一转小写，读不到该值。普通 `setAttribute('viewBox', ...)` 也会产生第二个 `viewbox`。此前按原始 SVG 文本解码掩盖了这条状态分叉。

`dom.c` 的普通 get/set/remove 与 interned-name 查询现共享元素命名空间规则：HTML 查询/写入 ASCII 小写，SVG/MathML 保留精确拼写。新增 `dom_find_attr` 返回借用的属性条目供 JS 读取值及长度；`dom_set_attr_len` 保留此前嵌入 NUL 的值长度。普通 JS get/set/has/remove/toggle 及 getAttributeNode 均消费共享规则，不在 renderer 添加错误大小写兜底。parser 与 NS raw setter 继续保留原名，NS removal 用精确存储名删除，避免 HTML 上 raw 混合大小写属性无法删除。

这是 [DOM 标准普通属性访问](https://dom.spec.whatwg.org/#dom-element-getattribute) 的命名规则修复。现有 DOM 不区分 XML document-type，属性记录也没有完整 namespace 字段；本次没有声称补齐 namespace collision、QName 验证或 live Attr 对象。

`test-foreign-attributes` 已接 `ci-host`，正向必须先通过旧折叠策略负控制：31 项全过，旧策略 20 项失败。覆盖 parsed SVG viewBox / clipPathUnits、MathML definitionURL、更新无重复、大小写不同属性共存、精确删除、长属性名、HTML WIDTH、interned 查询、JS 属性节点及 NUL 值。原始修前现场保留 30 项 / 20 失败日志，随后补充 HTML raw-NS 删除得到最终 31 项。

相关 DOM ID 29/29、DOMParser 50/50、接口 58/58 通过。接口测试先出现 57/58：其自身在验证 window named property `d` 前用 `var d` 覆盖了它；旧折叠构建同样失败。只将该临时变量换成独立名称，原产品断言保留。`test-dom-bindings` 的旧 host 编译缺少 WEBAPI_HOST 接口配置，完整 html5lib tree-construction corpus 未安装，二者未记为通过。

证据目录：`build-ds-flex-fix/foreign-attributes/` 中 `before.log`、`old.log`、`gate.log`、`final.log`、`iface-old.log`、`parser.log` 与 `related.log`。本页工作未访问用户 VM、账号或真实站点。
