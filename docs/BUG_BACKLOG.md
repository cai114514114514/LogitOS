# Confirmed bug backlog (from BUGS_EXTENDED.md triage, 2026-06-07)

## 修复状态（2026-08-04）

13 项已全部修复。逐条修复位置与方法如下（原 triage 表格保留在下方，仅作历史记录）：

- **C2**：`c/net/tls/tls.c` — `aead_open` 前加 `blen-16 > sizeof s->app` 拒绝。
- **M2**：`c/net/tls/tls.c` — CertVerify 分支加 `ml<4` 与 `4+siglen>ml` 检查。
- **M3**：`c/net/tls/tls.c` — `tls_der_sig_to_rs` 补全 `p<end` / `nb` / `p+sl` / `p+il` 边界。
- **M11**：`c/apps/browser/css_engine.c` — `h_node_classes` 改 static 数组（32 上限），消除泄漏。
- **H2**：`c/net/http/http.c` — Content-Length 解析加饱和上限。
- **H3**：`c/net/http/url.c` — 端口累积 >65535 即截断。
- **H5**：`c/net/ip/ip.c` — `ip_input` 加 `ip_checksum(h, ihl)!=0` 丢弃。
- **H9**：`c/drivers/net/e1000.c` — `rx_init`/`tx_init` 改返回 int，`pmm_alloc` 逐次检查+回滚。
- **M20**：`c/drivers/virtio/virtio.c` — reset 等待改 2 亿次有界循环。
- **H11**：`c/kernel/cpu/smp.c` — AP 启动失败分支补 `kfree(stk)`。
- **H17**：`c/apps/as/vm.c` — `op_DIV`/`op_MOD` 加 INT64_MIN/-1 `runtime_error`。
- **H18**：`c/apps/as/vm.c` — `op_NEG` 加 INT64_MIN 检查。
- **M23**：`c/apps/libc/src/stdlib.c` — `strtod` 指数累积钳制（`strtoll` 原有 cutoff 防护核实无误，无需改）。

---

62 claims triaged vs HEAD: **13 REAL**, 4 already-fixed, 33 false, 9 design.

Deferred (user prioritised the SMP scheduler). Fix in a gated batch later; C2 is the one urgent (remote OOB write).

| id | sev | reach | title | fix |
|----|-----|-------|-------|-----|
| C2 | high | remote | tls_recv: aead_open writes up to 19979 bytes into s->app[16640] — peer-triggered static-bu | In tls_recv, before line 421 (after the rtype==REC_APPDATA check at line 420), add: `if (blen - 16 > (int)sizeof s->app) return -1;`. blen is already known >=0 (line 418) and aead_open re-checks blen<17, so blen-16>16640 rejects exactly the |
| M2 | high | remote | TLS CertVerify handler reads mb[0..3] without checking ml>=4, and uses an unbounded attack | At the top of the `mt == HS_CERT_VERIFY` branch (before line 319) add `if (ml < 4) { s->used=0; return TLS_E_PROTO; }`, and after reading siglen (after line 320) add `if (4 + siglen > ml) { s->used=0; return TLS_E_PROTO; }`. This bounds bot |
| M3 | medium | remote | tls_der_sig_to_rs: missing p<end checks before reading length/integer bytes -- OOB read | Add the missing bounds, mirroring x509.c der_tlv: (a) after line 441's tag consume, `if (p >= end) return -1;` before reading `sl`; (b) before the multi-byte length loop, `if (nb > 4 || p + nb > end) return -1;` (also rejects an absurd nb); |
| M11 | medium | untrusted-file | h_node_classes leaks the malloc'd classes array on every css_select_style call (LibCSS unr | The array storage is the handler's responsibility (LibCSS only consumes the lwc_string refs, and only touches the array during the css_select_style call). Two clean options in src/apps/browser/css_engine.c h_node_classes: (1) Intern into a  |
| H2 | low | remote | Content-Length parse integer overflow (signed overflow / UB) defeats early-fetch-exit opti | Cap the parse so it can never overflow and still preserves the 'already have all the body' short-circuit. In http.c fetch_once, replace line 152 with a guarded loop, e.g.: clen = 0; for (const char *p = cl; *p >= '0' && *p <= '9'; p++) { cl |
| H3 | low | remote | URL port parsing: unbounded signed int accumulation (UB) lets an overflow-to-negative valu | Bound the accumulation so it can never overflow before the range check. In url.c line 37, replace: while (*s >= '0' && *s <= '9') { port = port * 10 + (*s++ - '0'); any = 1; } with: while (*s >= '0' && *s <= '9') { port = port * 10 + (*s++  |
| H5 | low | remote | Incoming IP packets not checksum-verified in ip_input | In ip_input, immediately after the bounds checks (after line 86, where ihl>=20, ihl<=tot, and 14+tot<=len guarantee reading ihl bytes stays inside frame+len), add: if (ip_checksum(h, ihl) != 0) return; Use ihl (not sizeof *h) so IP options  |
| H9 | low | trusted-only | e1000 rx_init/tx_init: pmm_alloc return value unchecked (NULL deref / fault on OOM) | Make rx_init/tx_init return int; after each pmm_alloc() check for 0 and return -1 (e.g. `ring = pmm_alloc(); if (!ring) return -1;` and `rx_buf[i] = (uint8_t*)pmm_alloc(); if (!rx_buf[i]) return -1;`). In e1000_init() propagate: `if (rx_ini |
| M20 | low | trusted-only | virtio_init reset wait — unbounded busy-spin, no timeout | Bound the reset wait like virtio_request. Replace line 81 with a counted loop, e.g.: for (long i = 0; i < 200000000; i++) { if (r8(vd->common, C_STATUS) == 0) break; } if (r8(vd->common, C_STATUS) != 0) { kprintf("[virtio] %x: reset timeout |
| H11 | low | trusted-only | smp.c: AP kernel stack leaked when an ACPI-listed AP fails to start | Add the missing kfree in the failure branch at src/kernel/cpu/smp.c:144: `if (!ap_ack) { kfree(stk); cpu_apicid[g_online] = 0; kprintf("[smp] CPU apic_id=%d did not start\n", (int)aid); }`. (kfree is the matching deallocator for the kmalloc |
| H17 | low | local-script | op_DIV / op_MOD: INT64_MIN / -1 triggers #DE hardware fault instead of catchable runtime_e | In vm.c op_DIV, after the zero check at line 518 add: `if (AS_INT(a)==INT64_MIN && AS_INT(b)==-1) { runtime_error("integer overflow: INT64_MIN / -1"); goto err; }` and likewise in op_MOD after line 526: `if (AS_INT(a)==INT64_MIN && AS_INT(b |
| H18 | low | local-script | op_NEG: negating INT64_MIN is signed integer overflow (UB / wrong result) | In op_NEG (vm.c:530-536), before the integer negate add: `if (IS_INT(a) && AS_INT(a) == INT64_MIN) { runtime_error("integer overflow: cannot negate INT64_MIN"); goto err; }`. This matches the adjacent op_DIV/op_MOD divide-by-zero idiom (vm. |
| M23 | low | untrusted-file | strtoll signed-overflow UB + strtod exponent int-overflow UB on oversized numeric input | strtoll (stdlib.c:36): guard the accumulation, e.g. `if (v > (LLONG_MAX - d) / base) { errno = ERANGE; v = LLONG_MAX; /* consume remaining digits */ while (digit(*s,base)>=0) s++; break; }` before `v = v*base + d` (and return neg?LLONG_MIN: |

false/design: M1(FALSE), M5(FALSE), C1(FALSE), H1(FALSE), H4(FALSE), M6(FALSE), M7(FALSE), M8(FALSE), M9-ARP-VALIDATE(FALSE), L2(FALSE), C5(FALSE), H7(FALSE), H8(FALSE), L7(FALSE), C7(FALSE), H23(FALSE), M21(FALSE), C6(FALSE), H10(FALSE), H19(FALSE), H20(FALSE), H21(FALSE), M17(FALSE), M18(FALSE), L5(FALSE), L6(FALSE), H14(FALSE), H15(FALSE), H16(FALSE), M12(FALSE), M13(FALSE), L3(FALSE), M24(FALSE), M10(DESIGN), L1(DESIGN), M22(DESIGN), H12(DESIGN), M16(DESIGN), H13(DESIGN), M14(DESIGN), M15(DESIGN), L4(DESIGN)

