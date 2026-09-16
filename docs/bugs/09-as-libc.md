# AetherScript A3 编译器（c/apps/as）与 freestanding libc（c/apps/libc）缺陷审计

- 日期：2026-09-16
- 范围：`c/apps/as` 的 cli/common/frontend/sema/ir/backend/llvm/runtime/editor 全部 .c/.h（不含 `legacy/`，按任务边界排除），`c/apps/libc` 的 include/src 全部 .c/.h，约 39k 行。
- 方法：先读 `c/apps/as/README.md`（A2→A3 迁移图）与 `docs/CODE_AUDIT.md`（2026-08-04 旧扁平结构审计，其 as/libc 条目已修的不重复上报），grep `CLAUDE.md` 的 OPEN BUG（仅 munmap TLB 一条，与本范围无关）。然后按优先级深读：当天新改的 `cli/eval.c`（14:26）与 `runtime/heap.c`（13:04）、frontend（lexer/parser/expression/text/layout/import/support）、sema（expression/check/generic/scope/optional/assignment/comprehension/types/constants）、backend/llvm（emit/calls/numeric/compound/loop/closure/support/resource）、runtime（heap/port/dict/range/region/region_borrow/command_launch/allocation/memory/buffer/bytes/format/object/closure/integer_parse）、editor/completion.c、common（numeric/version/snapshot）；libc 侧精读 malloc.c（sized arena 重写）、stdio.c（printf 引擎全读）、string.c、scanf.c、stdlib.c，dtoa.c 抽读头部设计说明，其余文件做固定缓冲/VLA/长度推导扫描。
- 佐证标准：每条发现给出 file:line 与代码引用；推理链不能落到行号的一律标 SUSPECTED。本次未运行任何门禁（任务限定只读不改），全部结论为静态通读所得。

## 发现（按严重度）

### [high] [CONFIRMED] 客户端求值器：字符串局部变量的别名在重新赋值后悬垂（UAF）

- 位置：`c/apps/as/cli/eval.c:315-323`（AN_NAME 返回非持有副本）、`eval.c:377-384`（赋值路径 ev_clear 释放旧持有者）、`eval.c:42-46`（ev_clear）
- 代码：

```c
/* eval_expr, AN_NAME: 返回别名副本，owned 置空 */
    Ev local = x->locals[n->symbol];
    local.owned = NULL;
    return local;

/* eval_stmts, AN_ASSIGN: */
    Ev v = eval_expr(x, n->b);
    if (!x->failed && n->a->symbol >= 0 && n->a->symbol < AT_LOCALS) {
        ev_clear(&x->locals[n->a->symbol]);   /* 释放旧缓冲——所有别名同时悬垂 */
        x->locals[n->a->symbol] = v;
        v.owned = NULL;
    }
```

- 触发与后果：求值器的 EV 字符串是"单一持有者 + 裸指针别名"模型。访客侧（`!__STDC_HOSTED__ && !llvm`，`cli/commands.c:452-461` 的 Studio Run 路径）跑下面这个完全合法、sema 全过的 A3 子集程序：

```
# aether: 3
def main() -> None:
    s = "hello"
    t = s          # t.s 指向 s 持有的缓冲，owned=NULL
    s = "world"    # ev_clear(locals[s]) free 掉 "hello"
    print(t)       # fwrite 读已释放内存
    return None
```

  `print(t)` 对已 free 的堆缓冲做 `fwrite`（eval.c:128-130）——求值器进程内 UAF 读，可崩溃可读到脏数据。这是今天刚改的 eval_expr 拒绝路径留下的所有权接缝：`owned=NULL` 的"借用"语义与"赋值即释放旧持有者"互相矛盾。
- 修复建议：赋值进入槽位时若 `v.owned == NULL`（来自名字读/字面量别名）则先 `strdup` 深拷贝再存入；或把 EV 改为带引用计数/每槽独占复制的语义。二选一即可，关键是保证"每个 `s` 指针要么由本槽 owned，要么其持有者在本槽生命周期内不可被 ev_clear"。

### [high] [CONFIRMED] 客户端求值器：一元 `-`/`~` 对浮点与未支持运算符静默丢弃，违背"拒绝而非发明"契约

- 位置：`c/apps/as/cli/eval.c:324-339`
- 代码：

```c
    if (n->kind == AN_UNARY) {
        Ev a = eval_expr(x, n->a);
        if (n->op == T_MINUS && a.kind == EV_INT) {
            ...
        } else if (n->op == T_NOT) {
            ...
        }
        return a;      /* 其余情况原样返回操作数 */
    }
```

- 触发与后果：文件头注释写明 "unsupported nodes refuse instead of inventing a VM"，但这里的 else 路径不是拒绝而是**静默返回未取负的操作数**：
  - `print(-1.5)` 输出 `1.5`（sema 对浮点一元负号完全合法，`sema/expression.c:794-808` 只要求 Number）；
  - `print(~5)` 输出 `5`（T_TILDE 未处理，`sema/expression.c:804-806` 对整数 `~` 同样放行）；
  - `-` 作用于任何非 EV_INT 结果（float/str/None）同样被静默丢弃。
  这是访客侧唯一的执行路径（同上 commands.c），最基本的数值演示程序会得到确定性的错误答案——比崩溃更糟的一类。修复是一行 else `fail(x, "RuntimeError", op_name)`，与 eval_expr 其余拒绝分支对齐。
- 修复建议：补 `else if (n->op == T_MINUS && a.kind == EV_FLOAT) { a.f = -a.f; }`（语义补全），并对 `~`/其余组合走 `fail()`；两处都要，不能只加其一。

### [medium] [CONFIRMED] libc scanf：数值指令的收集上限 511 字节，width > 511 时与 glibc 行为分叉

- 位置：`c/apps/libc/src/scanf.c:123-138`（collect），`scanf.c:178`（width 解析上限 0x100000）
- 代码：

```c
static int collect(struct eng *e, char *buf, int cap, int width, ...)
{
    int n = 0;
    int limit = (width > 0 && width < cap - 1) ? width : cap - 1;   /* cap = 512 */
```

- 触发与后果：`sscanf("1...（1000 个 9）...9", "%1000d", &x)`：glibc 按宽度消费全部 1000 位；本实现最多消费 511 位就停止收集并转换成功，剩余 489 位留在输入流里给下一条指令。宿主门禁是"与 glibc 逐字节一致"（stdio.c:8 与 scanf.c:12 自述 `libc_diff_test` 覆盖），这是一条可稳定复现的偏差：数值结果碰巧相同（都饱和），但**输入消费量与后续指令可见的流位置**不同。`buf[512]` 本身没有越界（limit 钳在 511，`buf[n]=0` 安全），所以只是语义偏差，不是内存问题。
- 修复建议：数值指令按 width 分段收集/转换（每段重新累积），或把 buf 提升到与 width 上限同量级并保留截断诊断；至少在 `libc_diff_test` 里加 width>511 的用例，让偏差可见。

### [low] [CONFIRMED] eval_string：malloc 失败静默返回零值 Ev，print 把 OOM 打成 "None"

- 位置：`c/apps/as/cli/eval.c:112-123`
- 代码：

```c
static Ev eval_string(AtNode *n)
{
    int bytes = as_token_decode(n->token, NULL);
    char *s = malloc((size_t)bytes + 1);
    if (!s) {
        Ev v = {0};
        return v;      /* EV_NONE，x->failed 未置位 */
    }
```

- 触发与后果：字符串字面量分配失败时既不 `fail()` 也不设 `x->failed`，下游 `print_value` 打出 "None"，退出码 0。与同文件其它路径的 `MemoryError` 拒绝不一致（对比 eval.c:174-175 的拼接路径）。修复：`fail(x, "MemoryError", "string literal")` 后返回 `{0}`。

### [low] [CONFIRMED] as_typed_eval 不检查 main 的形参，与原生 AS3500 契约不一致

- 位置：`c/apps/as/cli/eval.c:469-477`，对照 `backend/llvm/emit.c:734-742`
- 代码：

```c
        if (f->module == 0 && !strcmp(f->name, "main")) {
            main = i;
        }
    ...
    Ex x = {.p = p, .f = &p->functions[main]};
    eval_stmts(&x, x.f->body);
```

- 触发与后果：`main(a)` 带参在原生路径被 AS3500 拒绝；求值器照常执行，形参名在读到时变成运行期 "unbound name in guest execution"，错误类别与时机都与宿主不同。同函数还选**最后一个**同名 main（循环无 break），虽然同模块重名会被 check 拒绝、多模块场景已被 `f->module == 0` 收窄，但显式 `break` + `nparams==0` 校验能把契约对齐。

### [low] [SUSPECTED] frontend/lexer.c：`\r` 不在空白集合里，CRLF 源文件逐行报 "unexpected character"

- 位置：`c/apps/as/frontend/lexer.c:178-179, 221-223`（空白集合只有 `' '` 与 `'\t'`），对照 `common/version.c:14`（版本探测却把 `\r` 计入空白）
- 代码：

```c
            while (*p == ' ' || *p == '\t') {
                col += (*p == '\t') ? (8 - (col % 8)) : 1;
```

- 触发与后果：CRLF 保存的 .as 文件能通过 `as_source_version`（`\r` 被跳过）却过不了词法：行尾 `\r` 落入 default 分支报 `unexpected character '<0xD>'`。标 SUSPECTED 是因为本仓库的宿主/访客工具链均以 LF 为主，未找到明确承诺接受 CRLF 的文档；但"版本探测接受、词法拒绝"这两个事实本身就是一处不一致，值得在词法层把 `\r` 归一为行尾或显式报版本错误。

### [low] [CONFIRMED] backend/llvm/loop.c：字典 for 循环的 snapshot 根在 break 路径不清空

- 位置：`c/apps/as/backend/llvm/loop.c:129-133`
- 代码：

```c
    at_ir_jump(g, step);
    ...
    at_ir_mark(g, done);
    if (dictionary) {
        at_ir_emit(g, "  store ptr null, ptr %%snapshot%d\n", n->id);
    }
```

- 触发与后果：`store null` 只在自然落到 done 时执行；`break` 经 `at_ir_jump(g, done)` 直接进入同一块但**跳过了这条 store**（它在前驱步进块里）。快照键表在循环 break 后仍被 `at_ir_root` 注册的根指着，直到同节点 id 的下次循环覆写或函数返回 `at_gc_restore` 弹栈。仅内存滞留，非悬垂非错误；但与循环 cleanup 在 break 时主动运行（`at_ir_cleanup_to(g, g->loop_cleanup, 0)`，emit.c:639-641）的纪律不一致。修复：把清空挪进 done 块开头即可（done 只有一个汇合点，store 放块首两条路径都覆盖）。

### [low] [SUSPECTED] frontend/parser.c：数组长度 `atoi` 对超长数字串是未定义行为

- 位置：`c/apps/as/frontend/parser.c:99-106`
- 代码：

```c
            Token size = at_parse_expect(r, T_INT, "expected constant array length");
            char n[32];
            at_parse_name(size, n, sizeof n);
            count = atoi(n);
```

- 触发与后果：`Array[i8, 9999999999999999999999999999999]` 这类 31 位数字在截断后交给 `atoi`，标准上 `strtol` 溢出是 UB；实践中 glibc 饱和/回绕得到垃圾值，随后 `count < 1 || count > 65536` 报 AS3200 兜住。同文件其它字面量都走 `at_parse_i64_exact`（layout.c:25-37、check.c:39-75），唯独这里裸用 atoi。标 SUSPECTED 是因为实际后果被范围检查兜住，只剩标准层面的 UB 与一处"同一仓库两套字面量解析"的不一致。修复：换 `at_parse_i64_exact`。

### [low] [CONFIRMED] libc strerror：`strerror(INT_MIN)` 对 `INT_MIN` 取负是 UB，产出乱码消息

- 位置：`c/apps/libc/src/string.c:279-284`
- 代码：

```c
    int v = e; char t[16]; int k = 0;
    if (v < 0) { unknown[n++] = '-'; v = -v; }      /* v == INT_MIN 时 UB */
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
```

- 触发与后果：`strerror(INT_MIN)`（如把符号相反的返回值原样传入）在补码机上 `-INT_MIN` 回绕为自身，`v % 10` 为负，`'0' + 负数` 产出非数字字节，静态缓冲里留下乱码。glibc 会打 "Unknown error -2147483648"。修复：`unsigned` 取模（同 malloc.c/heap.c 已用的 `0 - (uint64_t)v` 手法）。

### [low] [SUSPECTED] runtime/format.c：str(range) 逐元素物化，无长度上限

- 位置：`c/apps/as/runtime/format.c:249-272`
- 代码：

```c
        int64_t count = at_range_len(range);
        if (count < 0 || !literal(builder, "[")) {
            return 0;
        }
        for (int64_t index = 0; index < count; index++) {
```

- 触发与后果：`print(range(0, 9000000000000000000))` 一行让格式化器按元素循环拼接（TextBuilder 每次 realloc 翻倍），实际表现是数小时/数年量级的计算加直到 OOM 才止的内存增长。语言本身允许无限循环所以不算崩溃，但 Python 对 range 的 repr 是 O(1) 的 `range(0, ...)`，而本实现的 `len` 又精确支持全宽计数（runtime/range.c:45-48 有 `> INT64_MAX → -1` 的守卫，说明作者已考虑巨型 range），唯独 str 路径没有。标 SUSPECTED：没有内存越界，是否算缺陷取决于产品对 str(range) 的承诺。修复：元素数超过阈值（如 256）时格式化为 `Range[start, stop, step]`。

## 已知问题（未重复上报）

以下问题在既有文档中已有记录且本次复核仍未修/属既定取舍，均不重复计入：

- `docs/CODE_AUDIT.md`（2026-08-04）及其"修复状态"一节覆盖的 as/libc 条目：旧 `as/object.c`、`as/vm.c`、`as/compiler.c`、`as/as_bc.c` 等 A2 扁平文件的问题（GC 标记 OOM 反向、`.la` verifier、INT64_MIN 除法/取负等）——A2 引擎已按 README 迁出编译路径，其遗留文件现属 `c/apps/as/legacy/`，不在本次范围。
- `c/apps/libc/src/malloc.c` 旧审计条目（H-17 sentinel 越界读、free 无 arena 范围校验）：sized-arena 重写后逐条复核，`heap_init` 的 `arena_size - 2*HDR` + `next_hdr` 的 `>=` 判定已修 H-17；`hdr_of` 的 tag+checksum 防护取代了旧的 free 范围校验（恶意指针退化为 no-op + rebuild + broken 闩），属声明的取舍。
- `stdio.c` 旧审计中/低危条目（Inf/%f 死循环、`%f` 对 ≥2^64 的强转 UB、fread/fwrite 的 `sz*n` 溢出、width 解析溢出、`abs(INT_MIN)`）：复核确认修复/已由 `n > (size_t)-1 / sz` 与 width clamp 覆盖；`abs/labs/llabs(INT_MIN)` 已改为 unsigned 取负（stdlib.c:77-80）。
- `CLAUDE.md:445` 的 OPEN BUG（`munmap` 不做跨核 TLB shootdown）：内核 MM 范围，与本报告范围无关。
- 既有文档化的限制（非缺陷）：`README.md` 明言 region 的源级 borrow/转移、C ABI 调用、完成容器 API 等仍是迁移中工作；`eval.c` 头注释声明该求值器"不是第二门语言"，故其对 AN_FIELD/AN_INDEX/AN_CALL 的拒绝本身不算缺陷（但见上文 high #2：拒绝必须真的是拒绝）。
- `eval.c` 的 `while true:` 无步进上限会挂住 Studio Run：与语言允许无限循环一致，未单列。
