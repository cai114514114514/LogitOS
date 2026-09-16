# text / audio / media / ime / image / rust 缺陷猎捕报告

- 日期：2026-09-16
- 分区：`c/lib/text`（ttf/cff/shape/otlayout/bidi/script/utf8/glyphras/fontcolor）、`c/lib/audio`（wav/mp3/flac/vorbis/aac(+SBR)/opus/opus_celt/opus_range/ogg/audio/afft/amath/amd5）、`c/lib/media`（mp4/mkv/demux/avi/ts/ps/flv/pes/avclock/subs）、`c/lib/ime/pinyin.c`、`c/lib/image`（jpeg/gif/exif/svg/img）、`rust/src`（png/webp/inflate/vp8*/bmp/ico/imgbuf/pngenc）。

## 方法与覆盖

1. 先读 `docs/CODE_AUDIT.md` 全文与 `CLAUDE.md`（grep "OPEN BUG"/known；line 445 的 OPEN BUG 是 `munmap` TLB，不在本分区），并核对相邻报告 07（video-gfx）与 10（gui/coreutils，其中 preview.c×`audio.c`/`wav.c` 的声道数不匹配已由报告 10 上报，本报告不重复）。
2. 逐文件通读：`ttf.c`/`cff.c`/`shape.c`/`otlayout.c`/`bidi.c`/`pinyin.c`/`vorbis.c`/`aac.c`/`opus_celt.c`/`mp3.c`/`flac.c`/`mp4.c`/`mkv.c`/`demux.c`/`avi.c`/`ts.c`/`ps.c`/`flv.c`/`jpeg.c`/`svg.c`/`webp.rs`/`png.rs`/`inflate.rs`/`vp8*.rs`/`bmp.rs` 全文逐行；`pes.c`/`avclock.c`/`img.c`/`exif.c`/`gif.c`/`opus.c`/`opus_range.c`/`audio.c`/`wav.c`/`ogg.c`/`fontcolor.c`/`glyphras.c`/`imgbuf.rs`/`ico.rs` 全文；`subs.c`（已解析但零调用方）与 `amath.c`/`amd5.c`/`afft.c`（教科书级实现+测试覆盖）按缓冲区模式扫读；`vp8_inter.rs` 未编译进任何默认构建（Cargo feature 关闭），只核对门控未逐行读。
3. 重点关注：字体表偏移/复合字形递归/cmap 二分、MP4 box 尺寸、EBML 变长与 lacing、JPEG 扫描与 progressive 网格、GIF LZW、inflate 输出、各解码器位读取器、整数溢出（样本数×通道×位深、时间戳×timescale）、Rust 侧可 panic 的切片索引（本 crate `#![no_std]` + `panic="abort"`，panic 即死循环挂死）。
4. 注释先读后定性：vorbis LSB-first 独立读取器、opus 拒 SILK、progressive JPEG 双路径、fontcolor.c "gated and DEAD"、`demux.c` 的 `md_ticks_to_ns` 饱和注释（fuzzer 已在同一类问题上咬过一次）均按既定设计对待，不作为发现上报。

总体印象：这批代码是全仓库被审计与 fuzzer 打磨得最狠的部分——C 侧统一走 `fr_*`/`br`/`abits` 等带粘滞错误的边界读取器，Rust 侧统一 `Option`/边界检查读取器，绝大多数"合理的第一眼 bug"都已有注释写明是被负向控制钉住的行为。以下为通读后仍然成立的发现。

---

## 发现

统计：critical 0，high 0，medium 1，low 4。

### [medium] [CONFIRMED] fontcolor.c sbix 'dupe' 跟随是无界递归，dupe 环即栈溢出；注释声称"follow it once"与代码不符

位置：`c/lib/text/fontcolor.c:313-318`（`sbix_lookup`）。

```c
    /* 'dupe' points at another glyph's bitmap; follow it once. */
    if (tag == FONT_TAG('d','u','p','e') && out->len >= 2) {
        uint16_t other = (uint16_t)fr_u16(&b, o + 8);
        if (other != gid) return sbix_lookup(f, other, want_ppem, out);
        return -1;
    }
```

触发与后果：注释说的是"跟随一次"，实现却是对 `sbix_lookup` 的无条件尾递归，唯一守卫是 `other != gid`（只拒绝自指）。一个 sbix 表里 glyph A 的 dupe 指向 B、B 的 dupe 指向 A（两字节 each，完全合法可构造），首次查询即 A→B→A→… 无限递归，每层一个完整 `sbix_lookup` 栈帧——在 ring 0 是内核栈击穿，在宿主 fuzzer 下是确定性 stack-overflow。全部数据都过了 `fr_*` 边界检查，问题纯粹是"跟随"没有深度/已访问集合。
缓解现状（定性为 medium 而非 high 的原因）：`grep` 确认 `font_bitmap_lookup`/`sbix_lookup` 目前唯一的调用方是 `tests/unit/font_color_test.c:223-228` 与 `tests/unit/font_fuzz.c`（且后者是审计记录的"17 个没有 make 目标的测试源"之一），CLAUDE.md 明言 fontcolor.c "gated, green and DEAD"——今天没有生产路径可达。但一旦色彩字体路径接上（该文件存在的目的），这就是远程/磁盘可达的内核栈溢出；测试里的 dupe 用例只测了无环跟随，控制从未见过环。
修复建议：把递归改成带 `depth` 参数限 1（与注释对齐），或在 dict 里加一个 visited 位图/代数；一行改动即可。

### [low] [CONFIRMED] mkv.c CodecDelay/DefaultDuration 全链路有符号溢出 UB —— demux.c 已在同一类问题上被 fuzzer 咬过并加了饱和，mkv.c 漏了

位置：`c/lib/media/mkv.c:209`（DefaultDuration 读入）、`:218`（CodecDelay 读入）、`:504`（ns→ticks 换算）、`:509`、`:287`（块时间戳）、`:373`（lace 展开时间戳）。

```c
    t->delay_ticks = (long long)rd_uint(&v, e.size);   /* ns, for now */   /* :218 */
    ...
    m->tr[i].delay_ticks = (ns * (long long)timescale + 500000000LL) / 1000000000LL;  /* :504 */
    ...
    long long ts = cluster_ts + rel - (t ? t->delay_ticks : 0);                      /* :287 */
```

触发与后果：`rd_uint` 返回 `uint64`，文件里一个 8 字节的 CodecDelay（如 `0x8000000000000000`）转成 `long long` 即为 `LLONG_MIN`；默认 TimestampScale=1000（ffmpeg muxer 的出厂值）下 `:504` 的 `ns * timescale` 立即有符号溢出（UB），即使换算未溢出，`:287` 的 `cluster_ts + rel - delay_ticks` 与 `:373` 的 `ts + i * lace_ticks` 也会在负的巨值上溢出。同一文件树里 `demux.c:171-180` 的 `md_ticks_to_ns` 注释原话："SATURATE rather than overflow … (This is not hypothetical: the fuzzer found it, at scale 40, in a mutated stts run.)"——同一类值在 MP4 侧已被钉死，Matroska 侧的 CodecDelay/DefaultDuration 这两个入口没有享受同等待遇。实际后果是回绕成错误时间戳（A/V 失步、seek 落点错），下游 `md_ticks_to_ns` 的饱和救不回已经溢出的输入；无越界内存访问。
修复建议：读入处钳到 `[0, 1e12]` ns（任何真实值都远小于此），或复用 `md_ticks_to_ns` 的饱和算术于 `:504/:287/:373`。

### [low] [CONFIRMED] c/lib/text/utf8_next 没有边界参数，越过多字节尾字节最多读出缓冲区 3 字节；契约只写在隔壁文件里

位置：`c/lib/text/utf8.c:20-22`（连续字节循环无 end 检查）；调用面 `c/lib/text/shape.c:1194-1201`（按 `(p, len)` 使用）。

```c
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return s + 1; }  /* bad cont. */
        v = (v << 6) | (p[i] & 0x3F);
    }
```

触发与后果：`utf8_next(s)` 只收起点不收终点。`shape_line()` 以 `while (p < e)` 为界按长度解码：当 `len` 结束处恰是一个 2..4 字节序列的 lead 字节时，`:20` 的循环会读 `p[1..3]`，即越过调用方缓冲区末尾最多 3 字节。今天所有调用方（内核 `gui/text.c` 的标签串、`js_canvas.c` 经 `JS_ToCString` 的 NUL 结尾串）都恰好满足 NUL 终止，`c/apps/coreutils/logit_cells.h:95-104` 也明确写下了这一契约差异（"utf8_next relies on the terminating NUL to stop instead"）——但 `utf8.h` 本身没有写，API 形状（看起来像 `(ptr,len)` 可用）与真实契约（必须 NUL 结尾）不符，是一个等下一个调用方踩的静默陷阱。
修复建议：给 `utf8_next` 增加 `end` 参数（`lc_decode` 已是现成的有界参考实现），或在 `utf8.h` 把 NUL 终止要求写成显式契约。

### [low] [CONFIRMED] vorbis lookup1_values 的 65535 兜底对 dim=1 返回错误值 —— 合法（若病态）codebook 解出错误音频

位置：`c/lib/audio/vorbis.c:87-101`（`lookup1_values`），消费点 `:280-282`。

```c
static uint32_t lookup1_values(uint32_t entries, int dim)
{
    uint32_t r = 1;
    for (;;) {
        ...
        r = next;
        if (r > 65535) return r;      /* ← dim==1 且 entries > 65535 时，真值是 entries */
    }
}
```

触发与后果：spec 定义 `lookup1_values(entries, dim) = floor(entries^(1/dim))`。对 dim=1、entries > 65535 的 codebook（Vorbis 语法允许，entries 上限本文件放宽到 2^20），该函数在 r=65536 时提前返回，得到 65536 而非 entries；随后 `c->vq` 的 VQ 展开按 `off = (i / indexdiv) % lookup_values` 取错 `mult` 表项——解码不越界（mod 封住）、不报错，输出是错误样本。纯正确性缺陷，触发需要一个精心构造、现实中不存在的 codebook。
修复建议：兜底改为 `if (r > entries) return entries;`（dim=1 的循环自然止于 entries），既保留终止保证又消灭特例。

### [low] [CONFIRMED] ico.rs 目录条目截断使整个文件失败，与该函数自己的注释相矛盾

位置：`rust/src/ico.rs:34-48`（`entries`）。

```rust
        // A directory that claims more entries than the file holds is not fatal
        // as long as at least one earlier entry was usable.
        let w = match p.get(e) { Some(&0) => 256u32, Some(&v) => v as u32, None => break };
        ...
        let len = le32(p, e + 8)? as usize;     /* ← ? 把截断变成整文件失败 */
        let off = le32(p, e + 12)? as usize;
```

触发与后果：宽/高两字节用了优雅的 `p.get(...)...break`，长度/偏移两个 4 字节却用 `?` 直接把 `None` 传成整个 `entries()` 的 `None`——ICO 目录在最后一个条目中间被截断（fuzzed favicon 的常见形状）时，前面所有可用条目全部作废，整张图标解码失败。注释原话与实现相反。无内存安全问题，纯健壮性/可用性。
修复建议：`le32(p, e+8).unwrap_or(0)` 后 `if len == 0 { break; }`，与宽/高的处理对齐。

---

## 已知问题（未重复上报）

以下各项在既有文档中已有记录或属已声明的取舍，本次核实仍如故，不作为新发现：

- `rust/src/inflate.rs` stored 块对齐回退（`:174`）、NLEN 校验（`:179`）、adler32 校验——均为 CODE_AUDIT.md H-25 及 Rust 节中危项的已落地修复，本次通读确认在位。
- `rust/src/png.rs` 宽松接受项（interlace 不限 0/1、filter type>4 当 None、不查 IHDR 是否首块、chunk CRC 全跳过）与 8192×8192 大额分配面——CODE_AUDIT.md "Rust 组件"节已记录为已知取舍。
- `rust/src/lib.rs` panic handler 为 `spin_loop` 死循环、dev profile 与 release 行为差异——审计已记录；本次以此为由把 Rust 侧任何潜在 panic 都按"挂死"级对待，通读未发现可信输入可触发的切片越界（所有攻击者索引用 `get`/手工边界或掩码）。
- `c/lib/text/ttf.c` 的栈 VLA、`c/lib/image/jpeg.c` 的 DC 类别封顶、`gif.c` 的 first-code-equals-next 拒绝、`utf8.c` 的 overlong 拒绝——CODE_AUDIT.md 记录的已修复项，本次确认修复在位（ttf.c `emit` 的 flags 已改 scratch 尾部 `:470`；jpeg `:312` `t > 11` 拒绝）。
- `c/lib/audio/audio.c`/`wav.c` 允许最多 8 声道而 Preview 按 2 声道定长缓冲——报告 10（`docs/bugs/10-gui-coreutils-studio.md`）的 high 发现，本分区只提供事实（`wav.c:78`、`audio.c:275-310` 按解码器声道写出），不重复上报。
- `c/lib/media/subs.c` 整文件"parsed and unreachable"（`media.h:99` 注释自认）、`vp8_inter.rs` feature 关闭无消费者、VP9 门空——CLAUDE.md 已记录的架构现状。
- fontcolor.c 整体无生产调用方（CLAUDE.md "gated, green and DEAD"）——本报告 medium 项的可达性以此为准。
- opus 只解 CELT、按名拒绝 SILK/hybrid；AAC SBR 为 vendored FFmpeg n4.4 窄导入——既定范围声明，非缺陷。
