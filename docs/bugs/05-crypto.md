# c/crypto 密码学实现审计报告

- 日期：2026-09-16
- 范围：`c/crypto/` 全部 .c/.h（12,207 行：aead/hash/kdf/pubkey/pq/trust 及顶层 crypto.h/cpu_report.c/aes_dispatch.c），另延伸两处密钥来源消费点：`c/kernel/core/rng.c`（DRBG/种子来源）、`c/crypto/pq/mlkem_rand.c`、`mldsa_rand.c`
- 方法：先读 `docs/CODE_AUDIT.md` 全文与 `CLAUDE.md` 的 "OPEN BUG / constant time / AES-NI" 段落建立已知清单（见文末），再按优先级深读——GCM/CTR 计数器语义、CBC 填充常量时间、ChaCha20-Poly1305 AAD/长度编码、RSA PKCS#1 v1.5/PSS 填充验证、ECDSA nonce 与标量乘法、X25519 clamping、ML-KEM 封装/解封装与隐式拒绝、ML-DSA 签名/验证与边界比较、HKDF/HMAC 边界、SRNG 种子来源；keccak/crc32c/blake2s/sha1 略读并对表驱动实现抽查（Kyber zetas[0]=-1044 首表项、Dilithium zetas[1]=4808194、Keccak RC/RHO/PI、CRC-32C 0x82f63b78 均与公开参考一致）。
- 与既往报告的关系：`CODE_AUDIT.md`（2026-08-04/05）中密码学条目（S8、H-14、H-15、X25519 全零检查、hkdf_expand_label 契约、DRBG 重构、RSA-PSS salt 长度、hash_sel 回退、int 长度参数等）经逐条对代码确认均处于"已修"或"已声明取舍"状态，本报告不重复；2026-08-29 落地的"thirteen 2026 primitives"（ML-KEM/ML-DSA/keccak/AES-GCM-SIV/XChaCha20/BLAKE3/KMAC/cSHAKE/scrypt/argon2/secp256k1/x448/field448）是本次审计的重点增量，此前无逐行审计记录。

## 总体结论

未发现 critical/high/medium 级缺陷。AEAD 三族（GCM、ChaCha20-Poly1305 及 XChaCha、GCM-SIV）的计数器语义（GCM inc32 / SP 800-38A 全块递增 / RFC 8452 LE32 递增）、标签比较（全部常量时间累加）、GCM-SIV 解密后验签并擦除明文等关键点均正确；RSA PKCS#1 v1.5 验证严格（PS≥8、精确长度、无 Bleichenbacher e=3 形），PSS 结构检查完整；ECDSA 签名为 RFC 6979 确定性 nonce 且逐步与规范核对一致；ML-KEM 隐式拒绝恒算 Kbar、无错误返回、FIPS 203 7.2 模数检查在封装侧落地；ML-DSA 的 Decompose q-1 特例、`>=` 边界、HintBitUnpack 全部结构不变量检查在位。多数风险点已有文件头注释论证并配有负向控制。剩余发现集中在零化缺口与两处罕见输入下的边界，全部为 low。

## 发现（按严重度）

### [low] [CONFIRMED] ML-DSA 签名/密钥生成路径多组秘密中间量不清零

- 位置：`c/crypto/pq/mldsa.c:549,594-614`（keygen 的 `t0`/`t1`）、`c/crypto/pq/mldsa.c:629,644-645,677,724-728`（sign 的 `t0_hat`、`y`、`y_hat`、`z`、`cs1`/`cs2`/`low`/`ct0` 中间多项式）
- 该文件对 `s1`/`s2`/`s1_hat`/`rho_prime` 做了 `wipe()`（613-614、724、727 行），但同为秘密材料的量未擦：

```c
    poly s1_hat[MLDSA_L_MAX], s2_hat[MLDSA_K_MAX], t0_hat[MLDSA_K_MAX];
    ...
    poly y[MLDSA_L_MAX], y_hat[MLDSA_L_MAX], w[MLDSA_K_MAX], w1[MLDSA_K_MAX], row[MLDSA_L_MAX];
    ...
    poly z[MLDSA_L_MAX];
```

  - `t0`/`t0_hat`：t0 是私钥的组成部分（sk 布局 `rho||K||tr||s1||s2||t0`），keygen 打包进 sk 后 `t0[]` 不清零（614 行的 wipe 列表里没有它），sign 解包出的 `t0_hat` 同样不在 724/727 行的 wipe 列表里。
  - `y`/`y_hat`（掩码多项式）：`z = y + c·s1` 且 c 公开，若 `y` 与公开的 `z` 同时残留在被复用的栈帧中，`s1 = (z - y)/c` 可直接恢复签名私钥份额。
  - `cs1`/`cs2`/`low`/`ct0`（循环体内的 `c·s1`、`c·s2`、`c·t0`）同样未清零。
- 触发与后果：无内存安全后果；是密钥残留面。栈帧被后续调用复用后被侧信道/信息泄漏类缺陷读到即放大。同目录 `mlkem.c` 的纪律（每个秘密结构都 `wipe()`）在此文件只执行了一半。附带同类问题：`c/crypto/pq/mlkem.c:615-621`（decaps 的 `struct shake sh`，吸收了秘密 z 的 Keccak 挤压态）与 `mlkem.c:360-368`（`prf()` 的 `sh`，吸收了 sigma）也未擦。
- 修复建议：在 sign/keygen 的失败与成功两条收尾路径统一把 `t0_hat`、`y`、`y_hat`、循环内 `cs1/cs2/low/ct0`、keygen 的 `t0` 追加进 `wipe()` 列表；`mlkem.c` 的两处 `struct shake` 用后 `wipe(&sh, sizeof sh)`。

### [low] [CONFIRMED] x25519 不清零 clamped 标量与梯子状态

- 位置：`c/crypto/pubkey/x25519.c:118-148`

```c
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t e[32];
    for (int i=0;i<32;i++) e[i]=scalar[i];
    e[0]&=248; e[31]&=127; e[31]|=64;          /* clamp */
    ...
    fe_invert(z2,z2); fe_mul(x2,x2,z2);
    fe_tobytes(out, x2);
}
```

- 全文件 `grep -c wipe` 为 0：clamped 私有标量 `e`、梯子状态 `x2/z2`（`x2` 即共享秘密的 x 坐标）、`z2` 的求逆中间值全部留在栈上。对比同目录 `ed25519.c:474-477`（`h/a/prefix/rbuf/r/kbuf/k/c/R` 十项全擦）与 `ecdsa.c:550,593`（d/kb/R 全擦），本文件是 c/crypto 中唯一不给秘密路径做任何擦除的标量乘法实现。
- 触发与后果：同上，密钥残留面（低 severity）。共享秘密本身已写回调用方，问题仅在栈上副本。
- 修复建议：函数收尾 `crypto_wipe(e, sizeof e)` 与 `x1..z3/tmp0/tmp1`（`fe` 均为 40 字节，代价可忽略），风格对齐 ed25519.c。

### [low] [CONFIRMED] ecdsa.c `blind_scalar` 注释在 P-521 加宽后失实（注释漂移）

- 位置：`c/crypto/pubkey/ecdsa.c:511-515`

```c
/* Build the blinded scalar kb = d + rho*n and report how many bits to scan.
 * rho is forced into [2^30, 2^31) so the product's width is fixed (a rho that
 * happened to be tiny would blind almost nothing). With rho < 2^31 and n < 2^384
 * we get rho*n < 2^415 and d < n, so kb < 2^416 = exactly NL*32 bits -- it fits
 * the bn with no carry out, which is why the ladder scans nbytes*8 + 32 bits. */
```

- 注释写于 NL=13（P-256/384）时代：NL 现为 18（文件头 19 行，P-521 所需），`NL*32` = 576 而非 416，且 P-521 的 `n < 2^521` 使 "n < 2^384" 前提不再成立。代码本身仍然正确（重新推演：rho*n < 2^552，kb < 2^553 < 2^576，梯子扫 `nbytes*8+32` = 560 位 ≥ 553，无进位溢出、无越界），但注释的两处量化论断与现状矛盾——本树标准（AGENTS.md 第 1 节）要求注释里的数字可复核，这条正是"后来者按错误数字改动宽度"的引信。
- 触发与后果：无即时功能后果；后人若据注释"416 位足够"缩回 NL 即引入真实溢出。
- 修复建议：注释改写为对三条曲线分别成立的界（P-521：kb < 2^553 ≤ 2^576 = NL*32），或直接写 `rho*n < 2^(521+31)`。

### [low] [SUSPECTED] ecdsa_verify 未把摘要整数 e 先对 n 约减，可突破 Barrett 前提（fail-closed 误拒）

- 位置：`c/crypto/pubkey/ecdsa.c:434-439`（对照同树 `secp256k1.c:395-397` 做了约减）

```c
    /* e = leftmost min(hlen, nbytes) bytes of hash, as an integer mod n */
    int use = hlen < fl ? hlen : fl;
    bn_from_be(e, hash, use);

    bn w; mod_inv(w, s, c->n);
    mod_mul(u1, e, w, c->n);
```

- `e` 直接取自哈希，未做 "e ≥ n 则减 n"（签名路径 `ecdsa_sign` 在 762 行做了，secp256k1 的 verify 也做了，唯独这条路径没有）。`barrett_reduce`（201-218 行）的注释自述前提 `prod < m^2`，最终校正循环只有 3 次（215 行）。当 `e ∈ [n, 2^256)` 且 `w > n^2/e` 时 `e*w ≥ n^2`，前提被突破，可能返回未完全约减的 `u1` → R 点错 → **正当签名被误拒**。P-256/SHA-256 下 `P(e ≥ n) ≈ 2^-32`，恶意 TLS 服务端可离线研磨 ServerHello 随机数构造转录哈希主动触发。
- 触发与后果：仅 DoS/fail-closed（错验证无法被利用为错误接受：u1 错值仍需满足椭圆曲线方程，等价于解 ECDLP）。诚实流量随机触发概率 ~2^-32/次。
- 修复建议：对齐 `ecdsa_sign`/`secp256k1.c`：`if (bn_cmp(e, c->n) >= 0) { bn_sub(...); }` 一行（bits2int 结果 < 2^qlen < 2n，一次条件减精确）。标注 SUSPECTED 是因为错误需要 `e*w ≥ n^2` 且 3 次校正不够的窄窗口，静态推演无法 100% 排除 3 次校正恰好兜住的情形。

### [low] [SUSPECTED] kernel rng：RDSEED/RDRAND 在位但持续失败时静默退化为 rdtsc 重播种

- 位置：`c/kernel/core/rng.c:102-119`（对照 134-140 行的告警只覆盖"CPU 根本没有"的情形）

```c
        for (int t = 0; t < 16 && !got; t++) {
            if (has_rdseed)      got = rdseed64(&v);
            else if (has_rdrand) got = rdrand64(&v);
            else break;
        }
        if (got) buf[n++] = v;
    }
    buf[n++] = rdtsc();
```

- `rng_reseed()` 对 `rng_gather()` 返回的 n 个字一概照收：硬件标志在、但 RDSEED 因持续 CF=0 全部失败（16 次重试均无退避间隔）时，本次 epoch 的种子实际只有 rdtsc+tick，且无任何日志——`rng_strong()` 仍返回 1，TLS 的弱熵闸门被绕过。CODE_AUDIT 记录的修复只解决了"无硬件时照常握手"与"静默退化无告警"的**CPUID 缺失**分支。
- 触发与后果：需 RDSEED 反复失败的硬件/虚拟机（早期 KVM 型号、部分云嵌套虚拟化）才会命中；命中后该 epoch 的会话密钥可预测性回到修复前的水平。
- 修复建议：`rng_reseed` 统计本次硬件字成功数，为 0（或 < 2）时 `kprintf` 告警并考虑把 `rng_strong` 语义降级，或在重试循环加短延时/交叉 rdtsc 搅拌。

### [low] [SUSPECTED] AES-GCM-SIV `build_blocks` 有符号长度加法可回绕绕过容量检查

- 位置：`c/crypto/aead/aes_gcm_siv.c:266-269`

```c
    int aad_pad = (aadlen + 15) & ~15;
    int msg_pad = (msglen + 15) & ~15;
    if (aad_pad + msg_pad + 16 > SIV_MAXBLOCKS * 16) return -1;   /* caller's buffer too small */
    memset(blocks, 0, (size_t)(aad_pad + msg_pad + 16));
```

- 入口只检查 `aadlen >= 0`；`aadlen` 达 `INT_MAX-14` 以上时 `aadlen + 15` 有符号溢出为负，`aad_pad + msg_pad + 16` 为负、比较恒假，随后 `(size_t)` 负数回绕为巨值 `memset`。同类 int 长度问题在 CODE_AUDIT 中已作为已知低危记录过（hmac/chacha/aesgcm 的负长度），此处是新文件里的新变体：非负但接近 INT_MAX 的值。当前 GCM-SIV "not wired into anything"（文件头 8 行），无可达调用方。
- 修复建议：入口加 `if (aadlen > INT_MAX - 15 || msglen > INT_MAX - 15) return;`，或入口统一按 `> SIV_MAXBLOCKS*16 - 16` 早拒。

### [low] [SUSPECTED] scrypt scratch 长度算式可 uint64 溢出，容量检查失真

- 位置：`c/crypto/kdf/scrypt.c:238-241` 与 `251-254`

```c
uint64_t scrypt_scratch_len(uint64_t N, uint32_t r, uint32_t p)
{
    return (uint64_t)p * 128ULL * (uint64_t)r + scrypt_romix_scratch_len(r, N);
}
...
    uint64_t blk = 128ULL * r;
    uint64_t bblen = (uint64_t)p * blk;
    uint64_t romix_len = scrypt_romix_scratch_len(r, N);
    if (scratch_len < bblen + romix_len) return -1;
```

- `p`、`r` 均为 uint32 且入口只查非零：`p*r` 可达 ~2^64、`p*128*r` 可达 2^71，乘积回绕后 `scratch_len` 检查对完全不足的缓冲区放行，随后 `pbkdf2_sha256_c1(..., Bbuf, bblen)` 按回绕前的量写越界。当前 scrypt 无树内消费者、且调用方需用同一（同样溢出的）helper 算长度才会自洽地犯错，故为 low。
- 修复建议：入口钳制 `r > 64 || p > 4096 || N > (1ULL<<40)`（RFC 7914 建议范围的量级）即拒绝，或用 `bblen / blk != p` 之类除法回验检测回绕。

### [low] [SUSPECTED] CBC 填充校验的 `(i < pad) ? 0xff : 0x00` 依赖编译器生成无分支代码（定时风险，非功能 bug）

- 位置：`c/crypto/aead/aes_modes.c:132-139`

```c
    uint8_t pad = pt[len-1];
    unsigned bad = ((unsigned)pad - 1) >> 4;     /* nonzero if pad == 0 or > 16 */
    for (int i = 0; i < 16; i++) {
        /* mask = 0xff for the i < pad bytes that must equal pad, else 0; the
         * comparison compiles branchless (setcc), never a jump on `pad` */
        unsigned mask = (i < pad) ? 0xffu : 0x00u;
        bad |= (unsigned)(pt[len-1-i] ^ pad) & mask;
    }
```

- 算法正确（`bad` 的初值与 16 次累加、单一出口、坏填充即擦明文），但循环内 mask 的选择是**对秘密 `pad` 的三目**：文件自身声明"compiles branchless (setcc)"，这是对单个编译器的代码gen断言而非结构保证（clang/gcc 在不同 `-O`/目标上可能生成分支）。若成真分支，分支预测器可泄漏 pad 长度——恰是该注释声称已消除的量。
- 修复建议：把三目换成无分支形式并让其成立与编译器无关：`unsigned ge = (unsigned)((int)i - (int)pad) >> 31; bad |= (pt[len-1-i] ^ pad) & ge;`（或 `mask = (unsigned)(pad - 1 - i) >> 31` 类位技巧），并加注释说明为何不再依赖 codegen。

## 定时风险面（结构性，多数已有文档，此处仅列名并指向文档位置）

以下各项均为"已声明的取舍"，不算新缺陷，审计中逐项确认文档与代码一致：

- 便携 AES 后端 S-box 查表 + GHASH 位串行分支：`aesgcm.c:14-19,158-161` 文件头声明；AES-NI 后端在 `crypto.h:23` 暴露 `crypto_simd_constant_time()`。
- RSA/ECDSA/Ed25519 验证不常量时间（公开数据）：`rsa.c:9`、`ecdsa.c:7`、`ed25519.c:44-53`。
- NIST 曲线 ECDH 标量乘不常量时间（32 位 rho 盲化 + 临时标量）：`ecdsa.c:475-493` 整段论证。
- ML-DSA `modq()` 的 `%` 与拒绝循环数据相关分支：`mldsa.c:25-53` 文件头。
- ML-KEM SampleNTT 拒绝采样变时（输入 rho 公开）：`mlkem.c:37-45`。
- AES-128 便携后端即 fallback 的 cache-timing 面：CLAUDE.md "AES-NI is about constant time, not speed" 段。

## 已知问题（未重复上报）

以下在本审计中复核确认仍然存在/仍然成立，且已被 CODE_AUDIT.md、CLAUDE.md 或文件自身注释记录，按要求不作为新发现：

1. RSA-PSS 接受任意 salt 长度（TLS 1.3 要求 sLen=hLen）——`rsa.c:275-281`，CODE_AUDIT 低危在案。
2. `hash_sel` 对未知 hlen 静默回退 SHA-512；`hmac` 对未知 hlen 静默不写——`rsa.c:235-240`、`hmac_hkdf.c:25`，CODE_AUDIT 低危在案。
3. 有符号 int 长度参数负值无纵深检查（hmac/hkdf/chacha/aesgcm 各入口已查 `aadlen<0||len<0`，hkdf 内部未再查）——CODE_AUDIT 低危在案，本次确认 AEAD 四族入口均已补查。
4. `curves_init`/`params_init` SMP 首调用竞态——CODE_AUDIT 低危在案（ecdsa.c:86-88、secp256k1.c:86-89 同构）。
5. X25519 全零共享密钥检查不在 `x25519()` 内而在 tls.c 消费点（负向向量钉住低阶点必产全零）——CODE_AUDIT/TLS 批已记录的分层安排。
6. HKDF 仅 SHA-256/384（`hkdf_expand_label` 拒 64）；`hkdf_expand` 非法参数静默 no-op 已文档化为调用方契约——CODE_AUDIT "已知限制"。
7. AES-GCM-SIV、XChaCha20-Poly1305、BLAKE3、KMAC/cSHAKE、scrypt、argon2、secp256k1、x448/field448、ML-DSA 均为"primitive + gate、无消费者"状态（本次 grep 确认树内无调用），其中 secp256k1_verify/sign 的无长度参数公钥接口（H-15 同类）在无消费者期间不构成可达缺陷；ML-DSA sign/verify 全帧约 50-90 KiB（`mldsa.c:68-75` 文件头自述"未按内核 32 KiB 栈预算、不得在无重测前从 ring 0 调用"）。
8. DRBG 运行期行为（Hash_DRBG 风格、1024 次/1 MiB 周期重播种、state/output 分离）为 CODE_AUDIT 2026-08-05 批次的既定修复形态，本次复核 `c/kernel/core/rng.c` 与该记录一致（新发现仅上面第 5 条）。
9. `sha1`/`ocsp_sha1` 为 OCSP 专属遗留原语（已破产算法，仅作解析用），文件有 "do not" 级注释；其实现本身正确。
10. `cpu_report.c` 为测量报告 TU（AVX 关闭原因论证），非密码原语，未做逐行审计。

## 覆盖清单（按文件）

- 深读（逐行）：crypto.h、aesgcm.c、aes_modes.c、aes_ni.c、aes_dispatch.c、aes_gcm_siv.c、chacha20poly1305.c、chacha_core.h、xchacha20poly1305.c、rsa.c、ecdsa.c、x25519.c、ed25519.c、secp256k1.c、x448.c、field448.c、mlkem.c/.h、mlkem_rand.c、mldsa.c/.h、mldsa_rand.c、keccak.c、argon2.c、scrypt.c、pbkdf2.c、hmac_hkdf.c、pkgsig.c/.h、aexsig.c、rng.c。
- 略读+抽查（常量表/finish/padding 路径）：sha256.c（SHA-224 共核）、sha384.c（SHA-512 家族共核与 512/t IV 注记）、sha1.c、blake2b.c、blake2s.c（三轮负向控制注释）、blake3.c（四陷阱逐项核对：ROOT 位置、CHUNK 标志、二进制计数器归并、XOF 计数器）、kmac.c、cshake.c、crc32c.c、roots.c、aes_backend.h、cpu_report.c（仅定性）。
