export const meta = {
  name: 'logit-system-bug-hunt',
  description: 'System-wide bug hunt across Logit OS: per-subsystem finders + adversarial verification (read-only)',
  phases: [
    { title: 'Find', detail: '16 parallel code-reviewer finders, one per subsystem lane' },
    { title: 'Verify', detail: 'each candidate independently verified by a skeptic, default-reject' },
  ],
}

const R = '/Users/wangzhe/ststem/src'

// What is intentional/known -- finders MUST NOT report these as bugs.
const NONBUGS = `KNOWN/INTENTIONAL -- do NOT report these as bugs:
- e1000 NIC is POLLED (no NIC IRQ); net_poll pumped from the WM loop. By design.
- TCP (net/transport/tcp.c): single outstanding segment, no reordering / window scaling /
  congestion control; large multi-cert RSA handshake flights can fail. Known limitation.
- LogitFS cross-boot write durability degrades across repeated non-snapshot boots (use -snapshot). Known.
- TLS 1.3 only, two cipher suites (ChaCha20-Poly1305, AES-128-GCM), no resumption/0-RTT/client-certs. Intentional.
- TTF rasterizer: no hinting, grayscale only (no subpixel), no bidi/shaping; integer-only. Intentional.
- One instance per GUI app; no lazy-FP (CR0.TS). Intentional.
- DNS uses static config (no DHCP yet). Known.
- The kernel is freestanding (no libc), x86_64, -mno-red-zone -mno-mmx -msse -msse2 (SSE enabled at boot, M15).
- Scheduling is effectively single-core (SMP only does parallel framebuffer present; no cross-core preemptive
  scheduler). So general cross-core data races are NOT applicable -- but IRQ reentrancy (timer/keyboard/mouse/
  IRQ handlers vs main-thread code touching the same state) IS a real, in-scope bug class.`

const CTX = `Logit OS -- a from-scratch x86_64 kernel + userland (C + nasm), QEMU/TCG. You are hunting for
REAL, CONCRETE bugs: memory safety (buffer/array overflow, out-of-bounds, use-after-free, double-free,
uninitialized reads, type confusion), integer overflow/signedness leading to bad sizes/indices, missing
error/NULL checks (esp. on attacker- or disk- or network-controlled input), resource leaks (memory, fds,
slots, locks not released on error paths), IRQ-reentrancy races, off-by-one, wrong state-machine
transitions, incorrect bounds/length math, and clear logic errors. Parsers of untrusted input
(net stack, crypto/x509/tls, fs on-disk structures, image codecs png/gif/inflate, the TTF font parser)
are the highest-value targets -- an OOB read/write there is a real vulnerability.

${NONBUGS}

Report ONLY concrete defects with a specific trigger/scenario and a file:line. NOT style, naming,
formatting, missing comments, or "could be refactored". When unsure whether something is intentional,
prefer NOT reporting it. Read the ACTUAL source files (glob/grep/read them) -- do not guess.`

const FINDING_SCHEMA = {
  type: 'object', additionalProperties: false,
  properties: { findings: { type: 'array', items: {
    type: 'object', additionalProperties: false,
    properties: {
      title: { type: 'string' },
      file: { type: 'string', description: 'path relative to repo root' },
      location: { type: 'string', description: 'function name + line number(s)' },
      category: { type: 'string', enum: ['memory-safety','integer-overflow','error-handling','resource-leak','irq-race','off-by-one','logic','parsing-untrusted','hardening'] },
      severity: { type: 'string', enum: ['critical','high','medium','low'] },
      trigger: { type: 'string', description: 'the concrete input/sequence that triggers it' },
      explanation: { type: 'string' },
      proposed_fix: { type: 'string' },
    }, required: ['title','file','location','category','severity','trigger','explanation','proposed_fix'],
  } } }, required: ['findings'],
}
const VERDICT_SCHEMA = {
  type: 'object', additionalProperties: false,
  properties: {
    real: { type: 'boolean', description: 'true ONLY if a concrete, reachable defect' },
    confidence: { type: 'string', enum: ['high','medium','low'] },
    reasoning: { type: 'string', description: 'why real or not; cite the code you read' },
    refined_fix: { type: 'string' },
    fix_risk: { type: 'string', enum: ['low','medium','high'], description: 'risk that applying the fix breaks something / needs care' },
    regression_gate: { type: 'string', description: 'which make target(s) would catch a regression: test / test-as / test-shell / test-as-os / manual' },
  }, required: ['real','confidence','reasoning','refined_fix','fix_risk','regression_gate'],
}

const LANES = [
  { key: 'boot',         paths: `${R}/boot/`, focus: 'multiboot2 parse, 32-bit checks, identity page-table build, long-mode switch, SSE/CR0/CR4 setup, FXSAVE area, stack setup (stack_bottom vs page tables proximity). Asm correctness: register clobbers, wrong sizes, missing checks.' },
  { key: 'cpu',          paths: `${R}/kernel/cpu/`, focus: 'GDT/TSS (rsp0), IDT, ISR save/restore (FXSAVE/FXRSTOR), int 0x80 syscall dispatch & argument validation (user-controlled pointers/lengths passed to kernel!), interrupt gate IF handling, exception handlers, fault containment.' },
  { key: 'mm',           paths: `${R}/kernel/mm/`, focus: 'PMM (frame alloc/free, double-free, bitmap bounds), VMM 4-level walk (vmm_map_page, intermediate USER flags, leaf protection), per-process spaces (vmm_new_space/clone/free), heap/kmalloc (overflow, alignment, free-list corruption, OOM NULL handling).' },
  { key: 'sched-exec',   paths: `${R}/kernel/sched/ ${R}/kernel/exec/`, focus: 'scheduler (run queue, thread reap, kstack), fork (vmm_clone_user, fork_ret), execve (argv/envp stack build -- bounds!), waitpid/reap (zombie, space free), fd table (file.c: F_VFS buffer, F_PIPE ring refcounts/EOF, F_TTY), proc table bounds, ELF/AEX loader (header validation on disk-controlled input -- segment sizes/offsets).' },
  { key: 'core-pci',     paths: `${R}/kernel/core/ ${R}/kernel/pci/`, focus: 'kprintf (format string handling, buffer bounds), panic, early init ordering, PCI config (0xCF8/0xCFC, BAR parsing, bounds).' },
  { key: 'gui',          paths: `${R}/kernel/gui/`, focus: 'window manager (wm.c: dynamic windows, per-window surfaces, gui syscall handling -- user rect/coords/length validation), fb.c (surface blit bounds, back buffer, fb_present), compositor, raster/text/font metrics. Look for OOB blits, integer overflow in w*h, missing clip.' },
  { key: 'char-timer',   paths: `${R}/drivers/char/ ${R}/drivers/timer/`, focus: 'PS/2 keyboard & mouse (IRQ handlers, 1-byte buffer, scancode state, ring buffers vs main-thread consumer = IRQ race), serial, RTC/CMOS read (BCD, race on update), PIT. Reentrancy and ring-buffer index bugs.' },
  { key: 'block-virtio', paths: `${R}/drivers/block/ ${R}/drivers/virtio/`, focus: 'ATA PIO (read/write, bounded poll, retry), blkdev, virtio transport (PCI cap parse in BAR4, split virtqueue setup, descriptor ring indices, used/avail wrap, the g_virtio_busy non-preempt poll), virtio-blk request, virtio-gpu (resource, TRANSFER_TO_HOST_2D/RESOURCE_FLUSH, scanout). DMA buffer sizing, descriptor index overflow.' },
  { key: 'net-lower',    paths: `${R}/drivers/net/ ${R}/net/link/ ${R}/net/ip/ ${R}/net/core/`, focus: 'e1000 (RX/TX descriptor rings, polled, the set_rx_control flush timing, buffer sizes), eth/arp/ip/icmp/udp parsing of RECEIVED packets (untrusted!): length checks, header bounds, checksum, ARP cache, fragment handling, copy sizes.' },
  { key: 'net-upper',    paths: `${R}/net/transport/ ${R}/net/dns/ ${R}/net/http/`, focus: 'TCP state machine & slot lifecycle (FIN_WAIT/TIME_WAIT leaks already fixed -- look for OTHERS), seq/ack math, recv buffer bounds; DNS response parse (untrusted: name compression pointers -> infinite loop / OOB, record counts), HTTP/URL/HTML parse (header parsing, Content-Length, redirect Location, de-tag buffer bounds, entity decode).' },
  { key: 'tls-crypto',   paths: `${R}/net/tls/ ${R}/crypto/`, focus: 'TLS 1.3 record/handshake parse (untrusted server data: length fields, transcript, key schedule), X.509/DER parse (crypto/trust: length/tag bounds, recursion depth, integer parse), RSA (bignum modexp bounds, PKCS#1/PSS padding checks -- forgery!), ECDSA/EC point validation, AEAD tag verification (constant-time?), HKDF. OOB in DER/cert parsing is critical.' },
  { key: 'fs',           paths: `${R}/fs/`, focus: 'LogitFS on-disk parse (untrusted disk image: superblock validation, block/inode bounds, direct[12]+single+double indirect index math, dir entry iteration bounds, free-block bitmap), VFS path resolution (path length, traversal), inode_trunc (double-indirect free), read/write offset+length bounds.' },
  { key: 'lib-image',    paths: `${R}/lib/image/ ${R}/lib/text/`, focus: 'DEFLATE/inflate (huffman table bounds, distance/length codes, output buffer overflow -- classic), PNG (chunk length, IDAT, filter, scanline bounds), GIF (LZW dictionary overflow, image descriptor bounds), img wrapper; UTF-8 decode (overlong/truncated sequences), TTF parser (cmap fmt4/12 bounds, glyf simple+composite recursion, hmtx, loca, table offset validation -- untrusted font file). All parse untrusted input -> high value.' },
  { key: 'libc',         paths: `${R}/apps/libc/`, focus: 'mini-libc: malloc arena (free-list, coalescing, alignment, OOB), str/mem funcs (off-by-one, missing NUL), vsnprintf (%e/%f/%g, width/precision, buffer bounds), strtod/strtoll, setjmp/longjmp, fd-backed stdio (short read/write, fseek, EOF), io.c errno/syscall wrappers.' },
  { key: 'apps-gui-cli', paths: `${R}/apps/gui/ ${R}/apps/coreutils/`, focus: 'aui toolkit, GUI apps (clock/textedit/monitor/terminal/netapp/widgets), sh (pipe/redirect/arg parsing, PATH), coreutils. Look for buffer overflows in input handling, unchecked syscall returns, arg parsing bounds, terminal emulator escape/pipe handling.' },
  { key: 'apps-browser', paths: `${R}/apps/browser/`, focus: 'OUR browser code only (dom.c, layout.c, css_engine.c, browser_paint.c, js_dom.c) -- NOT third_party libcss/quickjs. DOM build from untrusted HTML (entity decode, tag nesting, auto-close), layout math (integer overflow, infinite loop), the css_select_handler callbacks (NULL handling, the #document-as-NULL gotcha), js_dom bindings (lifetime, the mutation dirty-flag). Hit-testing bounds.' },
]

phase('Find')
const results = await pipeline(
  LANES,
  lane => agent(
    `${CTX}\n\nYour lane: ${lane.key}. Audit ONLY these paths: ${lane.paths}\nFocus areas: ${lane.focus}\n\nRead the files thoroughly. Return up to 8 of the MOST concrete, highest-severity real bugs you can substantiate (prioritize memory-safety / parsing-untrusted / security over minor issues). For each, give an exact file:line and a concrete trigger. If the lane is genuinely clean, return fewer or an empty array -- do not pad.`,
    { label: `find:${lane.key}`, phase: 'Find', schema: FINDING_SCHEMA, agentType: 'feature-dev:code-reviewer' }
  ),
  (review, lane) => parallel((review?.findings || []).map(f => () =>
    agent(
      `${CTX}\n\nA reviewer flagged this potential bug. INDEPENDENTLY verify it by reading the ACTUAL source. Be a skeptic: default real=false unless you can (a) point to the exact code, (b) construct a concrete reachable trigger, and (c) rule out that it is intentional/guarded elsewhere. Also judge how risky the fix is and which test target would guard a regression.\n\nFINDING (lane ${lane.key}):\nTitle: ${f.title}\nFile: ${f.file}\nLocation: ${f.location}\nCategory: ${f.category}\nSeverity: ${f.severity}\nTrigger: ${f.trigger}\nExplanation: ${f.explanation}\nProposed fix: ${f.proposed_fix}`,
      { label: `verify:${lane.key}:${(f.title||'').slice(0,24)}`, phase: 'Verify', schema: VERDICT_SCHEMA, agentType: 'general-purpose' }
    ).then(v => ({ lane: lane.key, title: f.title, file: f.file, location: f.location, category: f.category, severity: f.severity, trigger: f.trigger, explanation: f.explanation, proposed_fix: f.proposed_fix, verdict: v }))
  ))
)

const all = results.flat().filter(Boolean)
const confirmed = all.filter(f => f.verdict?.real && f.verdict?.confidence !== 'low')
const order = { critical: 0, high: 1, medium: 2, low: 3 }
confirmed.sort((a, b) => (order[a.severity] ?? 9) - (order[b.severity] ?? 9))
return {
  candidates_total: all.length,
  confirmed_total: confirmed.length,
  by_severity: confirmed.reduce((m, f) => (m[f.severity] = (m[f.severity] || 0) + 1, m), {}),
  by_lane: confirmed.reduce((m, f) => (m[f.lane] = (m[f.lane] || 0) + 1, m), {}),
  confirmed: confirmed.map(f => ({
    severity: f.severity, lane: f.lane, category: f.category, title: f.title,
    file: f.file, location: f.location, trigger: f.trigger,
    fix: f.verdict.refined_fix, fix_risk: f.verdict.fix_risk, gate: f.verdict.regression_gate,
    why: f.verdict.reasoning,
  })),
}
