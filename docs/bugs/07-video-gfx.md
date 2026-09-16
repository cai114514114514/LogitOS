# 07 · 视频解码器（c/lib/video）+ 2D 引擎（c/lib/gfx）+ 软件 3D（c/lib/gfx3d）缺陷审计

- 日期：2026-09-16
- 审计人：bug 猎手分区 07
- 范围：`c/lib/video`（H.264/H.265/MPEG-1/2/MPEG-4 头层/MJPEG/QTRLE 等遗留解码器，约 2.0 万行）、`c/lib/gfx`（约 7.1 千行）、`c/lib/gfx3d`（约 3.3 千行）的全部 `.c/.h`
- 方法：先读 `docs/CODE_AUDIT.md` 与 `CLAUDE.md` 的已知问题清单（video/gfx/gfx3d 三个目录不在既有审计范围内，属首次逐行审计）；再按优先级深读——H.264 的 DPB/参考帧管理/mbinfo 邻域推导、CABAC/CAVLC 越界、H.265 的 CTU/四叉树/RPS、MPEG slice 边界、gfx 光栅化累加器（`g_acc`/16.16 倒数表）与裁剪、gfx3d 的齐次裁剪（w≤0）/深度/插值/资源版本。本树注释文化极强，凡注释已声明的取舍（mvp 的 A/B/C 推导、ref_pic 与 ref_idx 双轨、`span_add` 截断史、apply_reorder 的重复槽位等）均先读注释再定性，不在无证据时推翻。

## 方法与覆盖

逐行读完以下全部文件（含交叉验证被调方的边界检查后才定性）：

- **H.264**：h264.c、h264_int.h、h264.h、h264_nal.c、h264_dpb.c、h264_mb.c、h264_cabac.c、h264_cabac_tables.h、h264_cavlc.c、h264_tables.h、h264_pred.c、h264_mc.c、h264_deblock.c、bs.h
- **H.265**：h265.c、h265_int.h、h265_nal.c、h265_cabac.c、h265_pred.c、h265_mc.c、h265_deblock.c、h265.h
- **MPEG 家族**：mpeg12.c、mpeg12_int.h、mpeg12_slice.c、mpeg12_mc.c（`m12_pred_edge` 边界钳制）、mpeg4_hdr.c、mpeg4_int.h、mpeg4_mc.c、mjpeg.c、legacy_qtrle.c（1/2/4/8/16/24/32bpp 全部分支的 `CKPTR`/`NEED` 核对）
- **gfx**：raster/gfx_raster.c、raster/gfx_paint.c、raster/gfx_mask.c、geometry/gfx_path.c、geometry/gfx_math.c、geometry/gfx_stroke.c、core/openlogit.c（关键路径）、adapters/openlogit_display.c（blit/blur/glass 全部写点）、include/gfx.h（LIMITS）
- **gfx3d**：render/ol3d.c、render/ol3d_resources.c、scene/ol3d_transform.c、scene/camera.c、scene/mesh.c、scene/skin.c、material/ol_material.c、shader/runtime/ols_vm.c、shader/ir/ols_validate.c

发现均回到源码核对过行号与逻辑；无法构造完整触发链的一律标 SUSPECTED 并写明缺口。未修改任何源码。

---

## 发现（按严重度）

### [high] [CONFIRMED] H.264 `first_mb_in_slice` 有符号比较绕过——负值直接变野指针写

- 位置：`c/lib/video/h264_nal.c:448`（解析）、`c/lib/video/h264.c:550`（失守的比较）、`c/lib/video/h264.c:583`（进入循环）、`c/lib/video/h264_mb.c:1211-1212`（野指针写）

```c
/* h264_nal.c:448 -- bs_ue 可返回 0xFFFFFFFF，(int) 强转后为负 */
sl->first_mb_in_slice = (int)bs_ue(bs);

/* h264.c:550 -- 有符号比较：INT_MIN >= total 为假，检查被绕过 */
if (sl->first_mb_in_slice >= total) return H264_ERR_CORRUPT;

/* h264.c:583 -> h264_mb.c:1211 */
int addr = sl->first_mb_in_slice;          /* 负值 */
while (addr < total) { ... h264_decode_mb(d, bs, sl, addr, &qpy); ... }
mbinfo_t *mi = &d->mb[addr];               /* d->mb + INT_MIN*sizeof(mbinfo_t) */
memset(mi, 0, sizeof *mi);                 /* 野指针写，必崩 */
```

- 触发与后果：`bs_ue` 的返回值域是 0..2^32-1（`bs.h:70-81` 无上限语义，前导零 31 位时可达 0xFFFFFFFF）。攻击流只需：一个合法的首 slice 建起 `d->cur`，随后第二个 slice NAL 携带 `first_mb_in_slice ≥ 0x80000000`。`== 0` 为假走 else 分支，`>= total` 对负值恒真放行，`addr` 以 `sizeof(mbinfo_t)≈384 字节 × 20 多亿` 的偏移参与 `&d->mb[addr]`——CAVLC 路径先经 `h264_decode_skip_mb`（同样 `&d->mb[addr]` 后 memset），即一次确定性的进程内野写/段错误。附带一处：`run > (uint32_t)(total - addr)` 在 `addr=INT_MIN` 时 `total - addr` 本身是有符号溢出 UB（`h264.c:590`）。
- 对比：H.265 同位置的 `segment_address` 是无符号读出后再 `>= sps->ctb_count` 拒绝（`h265_nal.c:651-653`），没有这个洞——两套解码器的约定不一致恰好说明 H.264 侧是遗漏而非设计。
- 修复建议：解析处钳制 `if ((uint32_t)sl->first_mb_in_slice > (uint32_t)(d->mbw * d->mbh - 1)) return H264_ERR_CORRUPT;`（以无符号比较），或 `decode_slice` 的检查改为 `if (sl->first_mb_in_slice < 0 || sl->first_mb_in_slice >= total)`。一行修复，建议同时给 `tests/` 补一个负向用例并让它先红后绿。

### [high] [CONFIRMED] H.265 `apply_rps` 的 `keep[]` 栈缓冲溢出——RPS 条目数上界之和超过表容量

- 位置：`c/lib/video/h265.c:276`（34 项数组）、`:292`、`:299`、`:313`（三处无界 append）；容量依据在 `c/lib/video/h265_nal.c:224`（`nneg + npos > H265_MAX_REFS` 拒绝，短期的上界是 16）与 `c/lib/video/h265_nal.c:688-689`（`nsps + npics > 32` 拒绝，长期的上界是 32）

```c
/* h265.c:276 */
pic_t *keep[H265_MAX_DPB * 2];             /* 17 * 2 = 34 项 */
...
for (int i = 0; i < r->num_negative; i++) {      /* ≤16 项 */
    pic_t *p = find_poc(d, d->poc + r->delta_poc[i], 0);
    if (p) { p->reference = 1; keep[nkeep++] = p; }   /* :292 */
}
...
for (int i = 0; i < sl->num_long_term; i++) {    /* ≤32 项 */
    ...
    p = find_poc(d, poc & (d->max_poc_lsb - 1), 1);   /* 按 LSB 匹配，可反复命中同一张图 */
    if (p) { p->reference = 2; keep[nkeep++] = p; }   /* :313，不去重 */
}
```

- 触发与后果：三个循环的 append 次数上界是 16（短期，负+正合计被 parse_strps 钳到 16）+ 32（长期）= **48**，而 `keep` 只有 34 项。长期条目按 LSB 匹配（`find_poc(..., lsb_only=1)`）且**不去重**：攻击者构造 16 张已被标为短期参考的图片，再发一个 slice 头携带 19+ 个 LSB 全部指向这些图片的 long-term 条目，`nkeep` 即达 35+，`keep[34..47]` 越界写越出栈数组、砸坏 `apply_rps` 调用帧（写入值是 `pic_t*`，非完全受控但仍可破坏返回地址/寄存器保存区）。这是纯栈越界写，ring-3 下是媒体进程崩溃乃至潜在控制流劫持；对"每个输入字节不可信"的自家约定是一次正面击穿。`lt_curr[32]` 恰好装得下 32 项，说明作者数过长期表，漏数了 `keep` 要同时装两个来源。
- 修复建议：`keep` 按"条目数上界"开：`pic_t *keep[H265_MAX_REFS + 32];`（=48），或在 append 前钳 `if (nkeep >= (int)(sizeof keep / sizeof *keep)) return H265_ERR_CORRUPT;`。后者一行，且对畸形流是更符合本文件风格的"响亮拒绝"。

### [high] [CONFIRMED] H.265 SPS `conf_win` 完全未校验——负偏移使解码器内部按任意偏移越界读

- 位置：`c/lib/video/h265_nal.c:351-352`（读入，无任何范围检查）；后果点 `c/lib/video/h265.c:484-493`（`emit()` 用它给输出平面定位）与 `c/lib/video/h265.c:439-470`（`build_display()` 按它的宽高读样本）

```c
/* h265_nal.c:352 -- (int) 强转可产生任意 int，包括负数与近 2^31 */
for (int i = 0; i < 4; i++) s.conf_win[i] = (int)bs_ue(bs);

/* h265.c:484-491 -- cl/ct 可为负，y16 指向平面可见区之外任意远处 */
int cl = sps->conf_win[0] * 2, cr = sps->conf_win[1] * 2;
int ct = sps->conf_win[2] * 2, cb = sps->conf_win[3] * 2;
out->width  = sps->width  - cl - cr;        /* 可大于编码宽度甚至为负 */
out->height = sps->height - ct - cb;
out->y16 = p->y + (long)ct * p->stride_y + cl;   /* 无界偏移 */
...
/* build_display() 随后从 y16 起按 out->width × out->height 读样本写入 d->disp */
```

- 触发与后果：恶意 SPS 声明 `conf_win_left_offset = 0xFFFFFF9C`（(int) 后为 -100）即得 `cl = -200`、`width = 编码宽 + 200`；更极端的值让 `y16` 落到分配区外任意位置，`build_display` 从那里读 `stride_y × height` 个样本——确定性堆越界读（段错误）或读回相邻堆块当像素。`*2` 处 `conf_win[i] ≥ 0x40000000` 时另有有符号溢出 UB。解码器自身当场崩，无需借道调用方。规范上 ConfWin 偏移必须非负且 `width - (left+right) > 0`；`h264_stream_info` 同样会交出负宽高。这是与 S1/H-5 同一族的"SPS 几何字段信任"缺口，只是发生在 ring-3。
- 修复建议：解析处校验并拒绝：`if (s.conf_win[0] + s.conf_win[1] > s.width / 2 || s.conf_win[2] + s.conf_win[3] > s.height / 2) return H265_ERR_CORRUPT;`（值先按 uint32 比较、再钳非负），一行挡掉整族。

### [medium] [CONFIRMED] H.264 I16x16 DC 与色度 DC 的反量化缺 level 钳制——crafted 系数即可 int32 溢出（UB，静默花屏）

- 位置：`c/lib/video/h264_mb.c:834`（I16x16 luma DC）、`c/lib/video/h264_mb.c:279`（色度 DC）；对照组是同文件 `c/lib/video/h264_mb.c:216-219`（`idct_add_dc_ac` 在乘法**前**把 level 钳到 ±16383）

```c
/* h264_mb.c:830-836 -- dc[] 直接来自 read_block，未钳制；
 * h264_dc16_transform 内部钳的是输入（±2^26），Hadamard 输出可达 ±2^28 */
h264_dc16_transform(dc);
int k = qpy / 6;
for (int i = 0; i < 16; i++) {
    int p = dc[i] * ls[0];        /* 2^28 * 160 ≈ 1.1e10 —— 溢出 */
    dc[i] = k >= 6 ? p * (1 << (k - 6)) : (p + (1 << (5 - k))) >> (6 - k);
}

/* 对照：h264_mb.c:216-219 明确先钳再乘 */
if (c > 16383) c = 16383; else if (c < -16384) c = -16384;
int p = c * ls[r];
```

- 触发与后果：CAVLC `decode_level` 允许 level 达 ±3.4e7（`h264_cavlc.c:110` 的 `CAVLC_MAX_LEVEL_PREFIX 28` 路径），CABAC UEG 可达 ±1.7e7（`h264_cabac.c:283-291`）。一个 ±1.35e7 的 I16x16 DC 系数经 Hadamard（×1 即可）后乘 flat 默认表的 `ls[0]=160` 就越过 INT32_MAX；色度 DC（2×2 Hadamard 后 `h264_mb.c:279`）同样。后果是有符号溢出 UB（回绕为错的 DC → 亮/暗异常块 → 经帧内预测沿参考链扩散）。对比之下 4×4/8×8 残差路径（`h264_pred.c:117/182`）经作者计算恰好封顶在 int32 内（1.94e9 / 9.7e8），且 `idct_add_dc_ac` 有显式钳制——两条 DC 路径是同一防御意图下的漏网点。
- 修复建议：两处乘法前加与 `idct_add_dc_ac` 相同的 `clamp_level`（±16383）。注意 I16x16 的钳制点必须在 `h264_dc16_transform` 之前（对原始 coef），否则 Hadamard 已放大。

### [medium] [CONFIRMED] H.264 SPS 裁剪窗口未校验——负 crop 使输出帧声明几何大于分配，调用方越界读

- 位置：`c/lib/video/h264_nal.c:318-320`（读入无检查）；后果点 `c/lib/video/h264.c:498-502`（`new_picture`）

```c
/* h264_nal.c:319 -- (int) 强转，可为负 */
for (int i = 0; i < 4; i++) s.crop[i] = (int)bs_ue(bs);

/* h264.c:498-502 -- cw 可为负，width 超过宏块对齐编码宽度 */
int cw = sps->crop_flag ? (sps->crop[0] + sps->crop[1]) * 2 : 0;
d->width  = d->mbw * 16 - cw;      /* cw = -8000 时 width = 编码宽 + 8000 */
d->height = d->mbh * 16 - ch;
if (d->width <= 0 || d->height <= 0) return H264_ERR_CORRUPT;   /* 只挡非正 */
```

- 触发与后果：`crop[0] = 0xFFFFF060`（int -4000）等值使 `d->width` 比 `stride_y` 的可见区宽出数千像素；解码器本身不按 width 寻址，但 `h264frame.width/height` 原样交给调用方（h264.h:36-37 的契约是"caller displays from (0,0) with w/h"），Preview/vidcheck/播放器按此 blit 即越过帧分配读内存。`crop[0]+crop[1] ≥ 2^30` 时乘 2 另有溢出 UB。与上一条 H.265 conf_win 同族，只是越界点在消费方，故降一档。
- 修复建议：解析处校验 `crop` 非负且 `(crop[0]+crop[1])*2 < mb_width*16`、`(crop[2]+crop[3])*2 < mb_height*16`（规范等价条件），违规返回 CORRUPT。

### [low] [CONFIRMED] H.264 MMCO / ref_pic_list_reordering 参数经 `(int)` 强转后参与有符号运算——多处溢出 UB

- 位置：`c/lib/video/h264_nal.c:517-520`（mmco arg）、`:407-409`（reorder arg）；UB 点 `c/lib/video/h264_dpb.c:177`（`arg + 1`）、`:185`、`:325`（`-(arg + 1)`）
- 触发与后果：`bs_ue` 值域 0..2^32-1，`(int)` 强转后可为任意 int32；`arg = 0x7FFFFFFF` 时 `arg + 1` 溢出，随后 `frame_num - (arg+1)` 再溢出。规范要求 `difference_of_pic_nums_minus1 < 2^log2_max_frame_num` 等，均未校验。实际后果限于回绕出错误 picnum → `find_short` 找不到图 → 命令静默无效（解码继续，参考错图），无内存不安全；但 UB 本身在 UBSan 门下会红。
- 修复建议：解析处按 `log2_max_frame_num` 钳制 arg 上界（与 spec 一致），溢出面即消失。

### [low] [CONFIRMED] H.265 `parse_strps` inter-RPS 派生集可写 `delta_poc[16]`——struct 内 4 字节越界写后才拒绝

- 位置：`c/lib/video/h265_nal.c:181-217`（append 循环）、`:218`（检查在写之后）

```c
/* strps_t: int delta_poc[H265_MAX_REFS]; —— 合法下标 0..15 */
out->delta_poc[k] = dpoc;      /* k 可达 16：父集 16 项 + DeltaRPS 自身 */
...
out->num_positive = k - i;
if (k > H265_MAX_REFS) return H265_ERR_CORRUPT;   /* :218，写完才查 */
```

- 触发与后果：被引用父集的 `num_negative + num_positive ≤ 16`，加 `DeltaRPS` 自身共 17 个候选；全为同号时 `k` 达 17，第 17 次 append 写 `delta_poc[16]`——恰好落在同 struct 的 `used[0]` 上（偏移重合），随后 `:218` 拒绝返回、值被丢弃。实际后果是无害的结构内重叠 + UB；但"先写后查"的顺序与该文件其余部分的风格相悖，若日后调整 struct 布局（如在 delta_poc 后插入别的成员）即变真越界。
- 修复建议：把 `if (k > H265_MAX_REFS)` 提到派生循环之前（`nd + 1 > H265_MAX_REFS + 1` 的兄弟检查在 `:169` 已经这么做了），或把两个数组都开到 `H265_MAX_REFS + 1`。

### [low] [CONFIRMED] H.265 `pred_weight_table` 权重值无上界——与 32 位样本乘法组合可溢出（UB，错色）

- 位置：`c/lib/video/h265_nal.c:610-611`（`luma_weight[l][i] = (1 << denom) + (int)bs_se(bs)`，无范围检查）；UB 点 `c/lib/video/h265_mc.c:280/282`（`v * weight`）、`:301`（`s0*w0 + s1*w1`）
- 触发与后果：`bs_se` 可达 ±1.07e9；样本是 14-bit 中间值（±16383），`16383 × 1e9 ≈ 1.6e13` 越过 int32。规范对权重有派生范围（约 ±2^7·2^(6-bitdepth+8) 量级），未校验。后果是回绕错色（clip 收尾，无内存不安全），仅 crafted 流可达。对照组：`chroma_offset` 在 `h265_nal.c:622` 做了 `h265_clip3(-128,127,...)`，说明作者记得钳 offset、漏了 weight。
- 修复建议：`luma_weight/chroma_weight` 存储前钳到规范范围（如 ±(1<<13)）即可覆盖乘法上界。

### [low] [CONFIRMED] ols VM 的 `c->dst` 不做边界检查——纵深防御与同函数 a/b/c 的处理不对称

- 位置：`c/lib/video/../gfx3d/shader/runtime/ols_vm.c:63-65`（`regs[c->a < OLS_REGISTERS ? c->a : 0]` 三处钳制）对 `:97`（`out->varying[c->dst]`）与 `:164`（`regs[c->dst]`）裸用

```c
float *a = regs[c->a < OLS_REGISTERS ? c->a : 0],   /* 读侧钳了 */
      ...;
case OLS_VARYING:
    memcpy(out->varying[c->dst], a, sizeof v);       /* :97，未钳 */
...
memcpy(regs[c->dst], v, sizeof v);                   /* :164，未钳 */
```

- 现状与后果：`ol_shader_validate`（`shader/ir/ols_validate.c:45`）确实拒绝 `c->dst >= OLS_REGISTERS`、`:70` 拒绝 varying 越界，且当前三个 `ols_run` 调用点（ol3d.c:202/254、ol_material.c:75）都先经 validate——所以今天不可达。但读侧既然钳了写侧却裸奔，等于承认"指令可能未经 validate 到达 VM"这个威胁模型的一半。若未来新增一个绕过 `ol3d_pipeline_validate` 的运行路径（如调试回放、缓存程序），此处立即是栈写越界。
- 修复建议：与 a/b/c 一致地钳 `dst`（`if (c->dst >= OLS_REGISTERS) { c->error=1; return OL_ARGUMENT; }`），一行消除不对称。

### [low] [CONFIRMED] H.264 多 slice 帧的去块滤波只看最后一个 slice 的 `disable_deblocking_filter_idc`

- 位置：`c/lib/video/h264.c:522-526`（`finish_picture` 用 `d->last_slice` 决定是否滤波整个帧）
- 触发与后果：spec 允许同一帧的各 slice 携带不同的 `disable_deblocking_filter_idc`（0/1/2 逐 slice 生效，idc==2 时本文件已正确按 `slice_first_mb[]` 跳过片间边）。当一帧混用 idc=0 与 idc=1 的 slice 时，整帧按最后一个 slice 的取值处理——该滤的不滤、不该滤的滤。无内存安全问题，仅 crafted/罕见流的正确性缺陷；`h264_deblock.c:414` 的"idc==1 整帧跳过"契约也由此继承。
- 修复建议：`mbinfo_t` 已按 MB 记录 slice 归属（`h264_slice_of`），在 `h264_deblock_frame` 的每条边判定时改查该边所属 slice 的 idc；或先文档化为已知限制（与现注释合并）。

### [low] [SUSPECTED] H.265 `entry_point_offset[i] = bs_u(bs, len) + 1` 在 len=32 且全 1 时回绕为 0

- 位置：`c/lib/video/h265_nal.c:806`
- 触发与后果：`len` 允许到 32，`bs_u(bs,32)=0xFFFFFFFF` 时 `+1` 在 uint32 上回绕为 0，使相邻 substream 起点重合。`h265.c:1746` 的 `sub_off[i+1] > rbsp_len` 检查与 `h265_cabac_start` 的负值拒绝（`h265_cabac.c:217`）兜住了内存安全，后果只是 substream 重叠解码出垃圾——属"畸形流被静默接受为错图"而非越界。标 SUSPECTED 是因为未实际构造流验证 CABAC 层的最终表现。
- 修复建议：`if (off == 0xFFFFFFFF) return H265_ERR_CORRUPT;`。

---

## 覆盖内未发现问题的高风险区（如实记录）

- **H.264 DPB/参考帧管理**（h264_dpb.c）：mmco 六种命令、滑动窗驱逐、`unref_all` 与 `pending_free` 的交互（`h264_release_pic` 的 memset 使 reap 二次释放成为 no-op）逐条推演无 UAF/悬挂；`apply_reorder` 的"只删 refIdx 之后的副本"注释与 x264 加权 P 的行为一致性成立。
- **H.265 DPB/输出**（h265.c `queue_output`/`emit`/`to_free`）：zombie 计数的注释（C.4.5 只数 reference|output）与实测记录一致；`to_free` 的生命周期与 H.264 `pending_free` 同构，安全。
- **CABAC/CAVLC 引擎**：H.264 引擎有 ctxIdx 边界检查（`h264_cabac.c:171`），H.265 引擎依赖调用方上下文构造（逐 ctx 上界核算均 ≤154，`h265_cabac_start:217` 拒绝负 bytepos——这也兜住了 entry-point 偏移为负的路径）；CAVLC 全部 VLC 查表有界，`h264_tables.h` 的 `[15][16]`/`[7][15]` 行数与 `tc-1 ≤ 14`、`row ≤ 6` 的索引上界吻合。
- **gfx 光栅化器**（gfx_raster.c）：`g_acc` 的 unsigned short 容量论证成立（≤ subs*256=8192）；`acc_to_row` 的 16.16 倒数在 `a < full` 下乘积 < 2^24 无溢出；`span_add` 双端钳制使 p0/p1 恒在 [0,n)；两遍 sweep 的 dry-run 语义自洽；`ne ≤ GFX_MAX_ACTIVE` 时跳过 dry run 的论证正确。24.8 坐标经 `>>` 处理负值是有意选择（文件头与 UBSan 历史注释）。
- **gfx3d**：`raster()` 对 `w ≤ 1e-12` 整三角形剔除、深度/混合写点均在钳制后的 [0,width)×[0,height) 内、`int` 截断的包围盒被两侧 clamp 收敛；`clip()` 的 Sutherland-Hodgman 上界 3+6=9 ≤ 12；资源版本（`buffer_view` 的 version 比对）在 `ol3d_draw_buffers:65` 正确拦截过期视图；`ol3d_skin_vertex` 对权重和/关节索引全部校验。齐次裁剪（w≤0）、深度、透视校正除零（`den <= 0 || !isfinite` 显式拒绝）三项重点均无洞。
- **MPEG-1/2**（31/31 bit-exact 的那条链）：slice/MB 边界、`decode_motion` 的 5+r_size 符号扩展、`m12_pred_edge` 的钳制读取、宏块地址推进的越界拒绝——未发现新缺陷。**MPEG-4** 注意：仓库里只有 hdr/mc/idct/bits/tables，**宏块层与公开解码入口不存在**（`m4_mc`/`m4_loop_filter` 仅被 tests/unit/mpeg4_mc_test.c 调用），故本报告不对未接线的半成品下结论。

## 已知问题（未重复上报）

- `h264.c:17-40` 头部注释自述的 **KNOWN DEFECT**（低 QP/High 矩阵下零星 ±1 采样）——已知、已定位排除清单、有复现命令。
- **H.265 B slices 不完整**（`test-h265-b` 红，`got 79 want 80`；`h265_mc.c:239-261` 有 2026-08-30 的完整隔离测量）及 **`test-vidbench-guest` 在 1280x720 下 h265 `decode error -3`**——CLAUDE.md 已如实标注。
- **MPEG-1/2 IDCT 锚定 ffmpeg `-idct simple`**（mpeg12.c 头部 27-40 行的测量依据）——设计决策，非缺陷。
- **MJPEG 借道 c/lib/image 的 `img_decode()`**、视频栈无 `<video src>` 播放路径、VP8 inter 关闭、VP9 门空（解码器未提交）——CLAUDE.md 已记录的架构现状。
- `gfx_stroke.c:28-51` 自述的 **`gfx_path_rect` 无闭合标记、按开路径描边** 的限制——文件内已声明 "REPORTED rather than guessed at here"，本报告不重复计数。
- 既有审计（docs/CODE_AUDIT.md）已覆盖并修复的 `c/kernel/gui/raster.c`、`c/lib/text/ttf.c` 等相邻文件问题，不在本分区，未纳入。
