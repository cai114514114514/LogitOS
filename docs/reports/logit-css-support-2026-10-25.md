# 两个 CSS 辅助库的替换与完整性核对

当前主树已用独立实现替换 libwapcaplet、libparserutils，并移除两套旧源码。
LibCSS 本身保留。新实现已补齐原公开头中的 55 个 parserutils 函数入口（包含
peek_slow）和 12 个字符串操作；完整构建、现有检查及真实 guest 页面通过。

**这不等于所有边界都已证明正确。** 本文分别记录接口、实际观察和未验证部分。
初次交付没有提交或推送；用户随后明确授权分开提交。实际提交及其校验见文末。

## 为什么重新核对

第一轮原说法是“保留当前浏览器用到的功能”，并主动省略了独立 codec、UTF-16
工具。用户指出，这不能叫完整替代。该批评成立；不能拿浏览器能运行来证明整个
库的公开行为完整。

重新核对发现并修正了下列实际问题：

| 问题 | 当前处理 |
|---|---|
| 缺少独立编解码、UTF-16 工具 | 补齐 6 个 codec 和 7 个 UTF-16 函数 |
| 只识别少量编码名称 | 独立查询 IANA 数据；名称查询与 codec 可用性分开 |
| 新表遗漏 ASCII、cp125x 等调用方别名 | 补回常用别名和历史可查询编号，恢复 ASCII 标点归一化 |
| 未定义单字节映射被当成零字符 | 按现有数据的实际零哨兵处理；统一正反向映射判定 |
| 空栈、空向量错误码不符 | 返回 INVALID；错误描述恢复为可读文本 |
| buffer_randomise 只换地址，没有改变对齐 | 分开保存分配起点和有效内容地址 |
| 公开 enum/union 标签及函数指针类型不兼容 | 恢复名称与参数类型；补上 C++ linkage 和可读参数名 |
| 通用输入流擅自替换 U+0000 | 普通追加与插入都保留合法零码点 |
| peek 指针被扩容释放、被压缩覆盖 | 独立 window 模块保留旧窗口，直到对应内容被消费 |
| LibCSS 吞掉分块 @charset 的 NEEDDATA | 候选前缀真正匹配时等待后续字节，声明完整后再决定编码 |

“数据表完整”曾被误当成“别名行为完整”；“当前消费者通过”曾被误当成
“公开接口完整”。两者都已经分别核对，不再混用。

## 实现如何分工

字符串库用开放寻址索引保存稳定对象指针；删除条目时回移探测链。大小写无关
比较使用创建时算好的 ASCII 折叠哈希，不创建旧实现的缓存小写伙伴。
LibCSS 对旧 insensitive 字段的访问已改为公开操作。

解析库按职责分文件：字节缓冲、分页容器、UTF-8/16、名称查询、单码点转换、
codec 输入与输出、输入流、窗口寿命。两库共 17 个 C 文件。第一轮报告的
“10 个对象”只是当时的范围，不能用于描述补齐后的源码。

window 模块保留每个被替换的旧内存块。advance 消费完该块覆盖的内容后才释放；
插入新内容会相应推迟释放。新数据和记录都分配成功后才发布状态，失败时原窗口
与借出的指针仍有效。此分配失败路径经过审查，未进行 malloc 故障注入。

codec 的 UCS-4 端固定为大端字节；独立 UTF-16 工具和无后缀 UTF-16 codec
采用主机字节序。输入不完整就暂存，输出空间不足就保留完整输出字符。
零长度 codec 输入用于排空输出，不表示 EOF；输入流的 append(NULL, 0) 才表示 EOF。

## 保留什么，改变什么

保留调用方接口和有用行为，不复制旧对象布局、缓存机制或实现代码。
公开头及其内联定义用于核对契约，保留的基线对象用于直接调用比较；
没有把旧 .c、私有编码表或生成器作为实现模板。这不是法律意义上的 clean-room 认证。

以下差异是明确选择：

- 旧 lwc 内部断言宏、缓存小写对象辅助函数和手动减引用后的 destroy 入口不重建；
  对象不透明，使用 ref/unref。旧二进制布局不兼容，调用方需要重新编译。
- 拒绝代理项、超过 U+10FFFF 的码点及过时五、六字节 UTF-8，不复现旧 Unicode 错误。
  UTF-8 范围依据 [RFC 3629](https://www.rfc-editor.org/rfc/rfc3629.html)。
- ISO-8859-11 按当前 IANA 注册别名解析为 2259；旧 4014 仍可反查名称。
  其他历史非 IANA 编号单独记录，不冒称标准编号，也不伪造 decoder。
- TRANSLIT 使用该接口允许的宽松替换回退，没有实现语言转写表。
- 未知错误名称返回 BADPARM，不保留旧实现对未知名称返回 OK 的行为。

IANA 名称数据共 258 条，采用 [IANA 的 CC0 数据声明](https://www.iana.org/help/licensing-terms)；
生成文件记录源地址和 SHA-256，正常构建不联网。算法是项目 GPL-3.0-or-later
代码。字符映射复用浏览器已有 WHATWG 数据，未复制旧库私有表。
CP1255 的 0xCA 按[原始映射表](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP1255.TXT)
视为未定义，没有改动浏览器数据本身。

此前 third_party 目录的同一统计口径由 11 项降至 9 项，代码组件由 8 项降至 6 项。
这只说明移除了两个第三方代码组件，不能说整个项目只剩 9 个外部来源；
新 IANA 数据也已单独登记在 THIRD_PARTY.md。

## 实际验证结果

按用户要求，没有新增或修改仓库测试文件。一次性诊断通过命令行直接调用当前
实现及保留的基线对象；它们不是持久回归套件。

| 核对内容 | 实际结果 |
|---|---|
| 原库支持的 codec 名称 | 27 个均可创建 |
| 单字节解码与基线比较 | 25 种 × 256 字节 = 6,400 项一致 |
| UTF-16 对照 Python 编解码 | 1,112,064 个合法 Unicode 码点全部往返一致 |
| Unicode codec 输入块 1–7 字节、输出容量 0–7 字节 | 672 组转换通过；另检查严格模式、恢复、reset 和非法 UTF-8 |
| 编码名称 | 基线全部 262 个编号仍可查询；独立复查的 282 个旧库认识的标准库别名仍可查询 |
| @charset 分块 | ASCII、UTF-16/32 大小端共 259 个未完成边界返回 NEEDDATA；33 个实际流分块正确解码 |
| BOM、分块、EOF、零码点 | 94 组通过；180,000 字节混合 Unicode 流反复压缩后输出逐字节一致 |
| peek 地址寿命 | ASan/UBSan 下检查扩容、插入、部分消费及压缩；独立复查也通过 |
| 容器和字符串 | 自插入、40 次改变对齐和扩容、500 元素分页容器；5,000 个字符串随机删除后 2,500 次身份核对通过 |
| 公开头文件 | 14 个头独立通过 C11/C++17 编译；公开 enum/union 类型可用 |
| 已有 CSS 声明、选择器、CSSOM | 85、60、149 项通过 |
| 上述三个目标以 ASan/UBSan 编译库和调用方 | 通过；macOS 使用 detect_leaks=0，不声称 LeakSanitizer 覆盖 |
| 已有布局、嵌套、背景图检查 | 164、72、65 项通过 |
| 真实 guest 组件页面 | 19 项、0 失败；最终截图已查看 |
| 内核 ISO 与完整 disk.img | 构建退出码 0 |
| test-mk-wired | 422 fragments，421 reachable，1 declared |

已有负控确实运行并报错，再由正向实现通过。例如选择器大小写负控为
60 项中 11 项失败，正向实现为 0 失败。这些是已有消费者的负控，
不能算作本次新增的库专项负控。

**覆盖更正：** 第一轮提到的 test-cssparse 共 14,902 项只链接 canon.c，
没有链接这两个库，因此不作为替换库的覆盖数字。

最终构建命令：

```sh
make BUILD=build-logitlibs -j8 test-cssdecl test-selectors test-cssom test-css-components-guest build-logitlibs/logit.iso build-logitlibs/disk.img
ASAN_OPTIONS=detect_leaks=0 make BUILD=build-logitlibs-san CC='clang -fsanitize=address,undefined -fno-omit-frame-pointer -g' -j6 test-cssdecl test-selectors test-cssom
make BUILD=build-logitlibs test-mk-wired
```

最终日志为 build-logitlibs/verified.log、build-logitlibs-san/verified.log、
build-logitlibs/wiring-verified.log。直接诊断记录为 contract-check.log、
codec-check.log、stream-check.log、stream-verified.log、container-check.log；
其中 stream-check.log 是零码点更正前的记录，当前行为以 stream-verified.log 为准。

最终截图：build-logitlibs/css-guest/components-dee9ea9580014f6987872e602c9a31eb/screen.png。
ISO 为 9,560,064 字节，disk.img 为 536,870,912 字节。已确认旧库的专有归档成员
不存在，新库没有进入 kernel.map，实际浏览器构建链接新实现。

## 仍然不能声称的事

没有进行分配失败注入、长期多线程压力、所有编码别名的外部穷举或所有错误返回
路径组合检查。独立审查和上述诊断补足了具体缺口，但不能证明任意输入和资源
故障下都绝对正确。没有新增长期专项测试是用户的当前开发政策，不应包装成覆盖。

## GenFS 与提交边界

用户明确要求删除 GenFS 实现和测试时，主树已没有 c/fs/genfs 实现。删除的实际
残留是 tests/genfs.mk、tests/unit/genfs_host_test.c 和对应设计文档，三者原本均
未被 Git 跟踪，因此不会产生可单独提交的 Git 删除补丁。其他测试没有变更，
测试接入检查已经通过。

初次交付时没有暂存、提交或推送。用户随后要求“提交，分着”，因此按测试忽略策略、
字符串库替换、解析库替换、LibCSS 探测修复、核对文档组织本地提交。
共享 Makefile 和通知文件也按对应功能拆分暂存，没有因为同处一个文件而混在一起。
没有改变其他工作树，没有推送。

## 分批提交记录

| 提交 | 独立内容 |
|---|---|
| b9810b02e | 忽略新增测试文件，保留已有 4,474 个受跟踪测试文件 |
| 617fa4678 | 字符串库替换及其构建、调用方和通知迁移 |
| 0b3ddd906 | 解析库替换及其构建、生成器和数据来源说明 |
| dd9a2d033 | LibCSS 分块编码探测修复 |
| 本文所在提交 | 设计、执行记录和核对报告 |

字符串库提交的暂存快照仍使用原 libparserutils，解析库提交的快照仍使用原编码
探测器。两者分别通过声明 85 项、选择器 60 项、CSSOM 149 项，以及内核 ISO
和完整 disk.img 构建，证明中间提交没有借用后续尚未提交的源码。
快照导出到本任务临时目录，没有创建或变更 Git 工作树。

探测器提交另行观察到部分 @cha 从提前 OK 变为 NEEDDATA，259 个分块边界和
5 种完整声明通过；再次通过产品构建、上述 host 检查和 guest 的 19 项检查。
最新 guest 截图已查看，保存在 build-logitlibs/commit-guest.png。

此次分批验证日志为 build-logitlibs/commit-strings.log、commit-parser.log、
commit-detector.log、commit-detect-check.log、commit-wiring.log。没有新建或修改
测试源码；文档提交只改变这三份说明，不改变已经验证的程序。
