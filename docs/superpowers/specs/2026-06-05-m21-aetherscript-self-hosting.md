# M21 — AetherScript 自举 + LibAether（dict → 标准库 → 自举编译器）

## 动机
AetherScript（M20）已经是一门有栈字节码 VM、有 import 模块的小语言（`src/apps/as/`，
`/bin/as`）。下一步是让它**自举**——用 AetherScript 自己写它的编译器，让 `as` 能编译
自己的源码。这是 "from scratch" 项目的成人礼:语言强到能实现自己。

**自举的精确含义(已定)**:**编译器用 AetherScript 写,VM 仍是 C**。VM 是运行时(它定义
机器),自举运行时既不可行也无意义;自举**编译器**(源码→字节码这步)才是真本事——这也是
rustc/Go 等语言"自举"的标准含义(编译器用自己写,运行时/底座可以是别的语言)。

## 架构决策(已定)
**分三阶段,各自独立 spec → plan → implement。本文档详设阶段 1(dict),阶段 2/3 给出
设计骨架,落地前各自补细节 spec。**

为什么是这个顺序:写一个编译器需要符号表/关键字表/opcode 表 —— 这些是 `name -> 值` 的
映射,没有 `dict` 只能用并行 list 硬凑,极其痛苦。所以 **dict 是阶段 1 的前置**。有了 dict
才好写 **LibAether 标准库**(阶段 2),有了标准库才好写 **compiler.as**(阶段 3)。

```
阶段1 dict  ──►  阶段2 LibAether  ──►  阶段3 自举编译器
(语言特性)      (用as写的stdlib)   (compiler.as + .aqc字节码 + 定点测试)
```

---

## 阶段 1 — `dict`(本里程碑的详细设计)

### 值模型
新对象类型 `O_DICT`(`src/apps/as/as.h` 的 `ObjType` 加一项),开放寻址哈希表:
```c
typedef struct { ObjStr *key_str; int64_t key_int; uint8_t kind; uint8_t used; Value val; } DictEntry;
typedef struct { Obj obj; DictEntry *entries; int count, cap; } ObjDict;  /* cap 是 2 的幂 */
```
- **键类型:string + int**(`kind` 区分)。编译器要的关键字表/符号表/opcode 表只需这两种。
  通用键(任意 Value 作键)需要值哈希 + 值相等,YAGNI,本阶段不做。
- 哈希:string 用 `ObjStr.hash`(已存在,FNV-1a);int 用一个整数混淆函数。开放寻址 +
  线性探查,负载因子 >0.75 时翻倍 rehash。删除用墓碑(tombstone)或 backward-shift。

### 语法 + 字节码
- 字面量 `{}`、`{"k": v, "k2": v2}`、`{1: "a", 2: "b"}` → 新 `OP_MAKE_DICT n`
  (栈上 n 对 key/value,弹出建 dict 压栈)。编译器在 `{` 处分流:`:` 出现即 dict 字面量。
  (注意:`{` 当前未用作语句块——AetherScript 是缩进分块,所以 `{` 自由可用。)
- 读写 `d[k]` 复用现有 `OP_INDEX_GET` / `OP_INDEX_SET`:在 VM 里按被索引对象类型分流
  (list → 现有整数下标;dict → 按 key 查/插)。dict 缺键读 → 运行时错误(像 list 越界);
  用 `.get(k, default)` 安全读。
- 方法走现有 `OP_INVOKE`(同 list 的 `.append`):`.keys()`→list、`.values()`→list、
  `.has(k)`→bool、`.get(k, default)`→value、`.remove(k)`。
- `len(d)` → 现有 `OP_LEN` 加 dict 分支。
- `k in d` → 关键字 `in` 当前只用于 `for x in ...`;**新增中缀 `in` 运算 + 新 `OP_IN`**
  (VM 按右操作数类型分流:dict 查键、list 查成员、str 查子串)。选 OP_IN 而非编译成
  `.has`,因为 `in` 要对 list/str 也成立,不只是 dict。
- `for k in d` → 遍历键(同 Python)。现有 for-in 在 VM 里按可迭代类型分流,加 dict→键迭代。

### 文件改动(阶段 1)
- `as.h`:`O_DICT`、`ObjDict`/`DictEntry`、`IS_DICT/AS_DICT`、`OP_MAKE_DICT`、`OP_IN`、
  `as_dict_new/get/set/has/remove/len` 原型。
- `object.c`:dict 对象的分配、哈希探查、rehash、释放(`as_free_objects` 加分支)。
- `value.c`:`as_print_value` 加 dict 打印(`{k: v, ...}`);dict 相等**按引用(同一对象)**,
  不做深比较(YAGNI;编译器用不到 dict 值相等)。
- `lexer.c`:`{` `}` `:` token(`:` 已用于 def/if/while/for 的块引导;dict 里复用同 token,
  靠语法位置区分)。
- `compiler.c`:`{...}` 字面量解析(主表达式)、`in` 中缀、`d[k]` 复用 index、`.method` 复用 invoke。
- `vm.c`:`OP_MAKE_DICT`、`OP_INDEX_GET/SET`/`OP_LEN`/for-in/`OP_INVOKE` 的 dict 分支。

### 验证(阶段 1)
- host `make test-as`:tools/t/as_test.c 加 dict 用例 —— 建/读/写/缺键、`.keys/.values/.has/
  .get`、`len`、`k in d`、`for k in d`、int 键、rehash(插 >cap 个键)、覆盖写、删除。
- on-Aether `make test-as-os`:加一个 `dict.as` 例子,串口断言输出。
- 不回归:现有 as 测试全绿。

---

## 阶段 2 — LibAether 标准库(设计骨架,落地前补 spec)

用 AetherScript 写、放 `fsroot/as/lib/`(或 `/usr/as/`)、经 `import` 使用的标准库。范围按
**"够写编译器 + 日常脚本"**裁剪,不堆砌:
- `string`:split / join / find / startswith / slice / strip / ord / chr / isdigit / isalpha …
- `list`:map / filter / reduce / sort(比较器)/ reverse / contains。
- `dict` 辅助:items()、按需的 setdefault 之类(基础操作已是语言内建)。
- `io`:读/写整个文件 —— 需要给 as 暴露 open/read/write/close(经 mini-libc fopen 的
  native 桥,或 `syscall(SYS_*)`,A3 已有 syscall 内建)。
- `test`:assert_eq / assert_true + 计数汇总(阶段 3 的自举测试要用)。

风险:LibAether 自身要有 host 测试(在 host 上跑 .as 断言);可能暴露语言 bug,顺带修。

## 阶段 3 — 自举编译器(设计骨架,落地前补 spec)

### 序列化字节码 `.aqc`
当前编译产物是内存里的 `ObjFn`(code 字节 + 常量池),没有落盘格式。自举需要 compiler.as
产出 **C VM 能加载执行**的东西 → 定义 `.aqc`:
- 头(magic + 版本)、常量池(int/float/string/嵌套 fn 递归编码)、code 字节、每个 fn 的 arity/
  name/局部数。
- C 侧:`as_dump(ObjFn*) -> bytes`、`as_load(bytes) -> ObjFn*`;VM 能跑 load 回来的 fn。
- `as` CLI:`as -c x.as -o x.aqc`(只编译、emit 字节码)、`as x.aqc`(直接跑字节码)。
  现有 `as x.as`(编译+跑)不变。

### compiler.as
用 AetherScript(+ LibAether + dict)重写 **lexer + compiler**,emit `.aqc` 字节流(和 C 的
`as_dump` 同格式)。VM 不动(仍 C)。

### 自举证明(定点测试)
1. stage0 = 现有 C 的 `as`。
2. C-as **跑** `compiler.as`,让它编译 `compiler.as` 自己 → `stage1.aqc`。
3. 用 `as stage1.aqc` **跑** stage1(它也是个编译器),再编译 `compiler.as` → `stage2.aqc`。
4. **断言 `stage1.aqc == stage2.aqc`(字节级定点)** —— 编译器编译自己产出稳定 → 自举成立。
5. 再抽查:stage1 编译几个 LibAether/示例,输出与 C-as 编译运行一致。

---

## 总体风险 / 注意
- **无 GC**:dict 和编译器都在 arena 上分配,脚本退出回收;长跑 REPL 会泄漏(M20 已知,延后)。
  编译器是一次性进程,可接受。
- **递归深度**:compiler.as 的递归下降在 1MB CLI 栈上,深表达式可能压栈 → 必要时调大
  as 栈(M18 `exec.c` 的 CLI 栈页数)。
- **dict 删除**:开放寻址删除要么墓碑要么回移,选回移避免墓碑堆积(编译器会频繁查不删,影响小)。
- **`in` 的二义**:`in` 既是 `for x in` 的关键字,又要做中缀成员运算 —— 编译器靠语法位置区分,
  写测试覆盖 `for k in d` vs `if k in d`。
- 阶段 2/3 各自落地前补独立 spec(本文档只定方向 + 阶段 1 详设)。

## 验证(里程碑级)
- 阶段 1:`make test-as`(host)+ `make test-as-os`(QEMU)dict 用例全绿,旧用例不回归。
- 阶段 2:LibAether 各模块 host 断言测试。
- 阶段 3:定点测试 `stage1.aqc == stage2.aqc` 通过;`make test` / `make test-shell` 不回归。
