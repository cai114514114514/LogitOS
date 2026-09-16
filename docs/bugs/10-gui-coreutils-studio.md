# GUI / coreutils / studio 缺陷猎捕报告（c/apps/gui + c/apps/coreutils + c/apps/studio + 共享头）

- 日期：2026-09-16
- 分区：`c/apps/gui`（aui 部件工具包 + 12 个窗口应用）、`c/apps/coreutils`（/bin/sh + 工具 + sshd/sftpd/login/httpd）、`c/apps/studio`（Code Studio 引擎）、`c/apps` 共享头（logit.h / clib.h / logit_rich.h / logit_cells.h / accounts.h / httpd_protocol.h）。
- 方法：先读 `docs/CODE_AUDIT.md`、`docs/BUG_BACKLOG.md`、`docs/BUG_REVIEW_2026-09-09.md` 与 `CLAUDE.md` 的 OPEN BUG 记录建已知清单；再按优先级通读源码——aui.c 全文、sh.c 全文、sshd.c/sshd_channels.h/sshd_channel_state.h/sftpd.c/accounts.h/login.c 全文、httpd.c + httpd_protocol.h 全文、terminal.c / preview.c / files.c(+inc) / textedit.c(+document.h+work.inc) / ch.c / monitor.c / gallery.c / settings.c(主体) / greeter.c(主体) / gui/studio.c 全文、studio/ 的 engine/runner/storage/jobs/document/edit/pairs/project/session/language/source_snapshot/completion_job/internal.h + editor.inc，其余小文件（widgets/assistant/clock/cp/cat/ls/head/wc/mv/rm/stat/dir/show/pref/clip 等）逐个快读；读注释后再定性，遵循"缺 beats 错着实现"的 house rule。

## 统计

| 严重度 | 条数 |
|--------|------|
| critical | 0 |
| high | 2 |
| medium | 4 |
| low | 9 |

---

## 发现

### [high] [CONFIRMED] Preview 音频按 2 声道定长缓冲，adec_read 按解码器实际声道数（最多 8）写入 —— 栈上结构体越界写

位置：`c/apps/gui/preview/preview.c:382`（buf 定义）、`preview.c:537`（只钳制本地副本）、`preview.c:603`（溢出写）；`c/lib/audio/audio.c:275-310`（adec_read 按自身 channels 写出）、`c/lib/audio/wav.c:79-91`（WAV 允许 `nch <= AUDIO_MAX_CHANNELS` 即 8）。

```c
/* preview.c aplay */
short          buf[ABUF_FRAMES * 2];          /* 1024 帧 × 2 声道 */
...
/* aplay_start()：只钳自己的副本，解码器内部仍是 6/8 声道 */
if (a->ch > 2) a->ch = 2;
...
/* aplay_pump() */
long got = adec_read(a->dec, a->buf, ABUF_FRAMES);
```

```c
/* audio.c adec_read —— 按 d->channels 交错写出，无每调用方缓冲概念 */
out[(done + i) * d->channels + c] = clip16(v);
```

触发与后果：打开一个 ≥3 声道的 WAV 或 FLAC（wav.c 明确放行到 8 声道），且机器有声卡（`snd_open_s16` 成功）。`adec_read` 一次最多写 1024 帧 × 8 声道 = 16384 字节进 4096 字节的 `buf`，越界约 12 KB。`aplay` 是 `play_container()` 的栈局部，`buf` 之后是 `whybuf`/指针/`fmt` 并继续冲出结构体——ring-3 栈破坏，最直接的表现是解码大文件时进程崩溃，破坏内容部分受声道布局影响。对照组就在树里：`c/apps/gui/terminal/terminal.c:1250` 的 `pend[AUD_CHUNK_FRAMES * AUDIO_MAX_CHANNELS]` 按最大声道数定长，是对的；preview 没有。
修复建议：与 terminal.c 同法，`buf` 改按 `ABUF_FRAMES * AUDIO_MAX_CHANNELS` 定长；或在 `aplay_start` 里对 `a->ch > 2` 直接置 `why = "only mono/stereo here"` 拒绝播放（拒绝优于错误声道输出）。

### [high] [CONFIRMED] KEY_F1..F12 被 aui.c / textedit.c / terminal.c 当 Unicode 码点插入 —— 全系统文本入口输入假字符

位置：`include/abi/logit_abi.h:226,267-278`（ABI 规则与新增键）、`c/drivers/char/keyboard.c:200-202`（内核实际发送）、`c/apps/gui/aui.c:2348`、`c/apps/gui/textedit/textedit.c:232`、`c/apps/gui/terminal/terminal.c:2545`。正确处理对照：`c/apps/studio/studio_editor.inc:44`。

```c
/* logit_abi.h —— 规则原文："0x101 and up is a KEY_* code and NOT text" */
#define KEY_F1    0x109
...
#define KEY_F12   0x114
```

```c
/* aui.c tf_apply_key —— 只枚举了八个导航键，F 键落进"码点"分支 */
else if (k > 0x7F && !tf_is_nav_key(k)) {
    char enc[4]; int el = tf_utf8_encode((unsigned)k, enc);
```

```c
/* terminal.c on_key —— F 键被 UTF-8 编码后写进 shell 的 stdin */
else if (a > 0x7F) {
    char enc[4]; int el = key_utf8_encode((unsigned)a, enc);
    for (int i = 0; i < el; i++) type_char(enc[i]);
```

触发与后果：按下物理 F1..F12（keyboard.c 对 scancode 0x3B..0x44/0x57/0x58 直发 `KEY_F1+..`），三个入口都把它当 U+0109..U+0114（拉丁扩展区 "ĥ" 等）插入：aui 文本框（Finder 重命名、Settings 网络字段、Chat 提示框、files 项目目标框）插入假字符；TextEdit 文档被写入假字符并随保存落盘；Terminal 把它编码后送进 /bin/sh 的行编辑器——用户没敲过的字母出现在命令行里。aui.c 自己的注释（2228-2230）写明了枚举而非范围判断正是为了"未来的非连续 KEY_* 新增不会开始被打进文本框"，F1..F12 恰是该预言的兑现，三处枚举都没有跟上。studio_editor.inc 第 44 行 `if(key>=KEY_F1&&key<=KEY_F12)return;` 证明该约定在本仓库已知。
修复建议：把 `tf_is_nav_key` / textedit 的导航判断 / terminal 的 `key_is_nav` 扩为 `key >= KEY_F1 && key <= KEY_F12` 一并吞掉（或在 logit.h 提供 `key_is_text(a)` 单一判据，收拢"一个罐子"）。

### [medium] [SUSPECTED] aui_menubar：未悬停任何菜单项时 Enter 提交 `*ii = -1`，调用方契约上是 `items[mi][-1]` 越界读

位置：`c/apps/gui/aui.c:2762`（`*pop.sel = pop.hi`）、`aui.c:2811`（`pop.hi = -1` 初值）。

```c
pop.items = items[i]; pop.sel = ii; pop.hi = -1;
...
else if (in.a == '\n') { *pop.sel = pop.hi; pop_changed_id = pop.owner; pop.kind = 0; }
```

触发与后果：打开菜单后不移动鼠标、直接按 Enter（或先按 Enter 前焦点恰在菜单栏），`*ii` 被写成 -1 并上报"已选择"。当前唯一调用方 `c/apps/gui/gallery/gallery.c:245` 只做 `menu_mi * 100 + menu_ii` 显示，无实害；但按 aui.h 的组件契约，一个用 `menus[*mi][*ii]` 取菜单文本的调用方会以 -1 索引指针数组——`menu_ii` 是 `int`，-1 不会被 clamp（对比 `aui_select` 路径 `hi` 都被 `iclamp(..., 0, n - 1)` 约束）。
修复建议：Enter 时 `if (pop.hi < 0)` 视为取消（kind=0，不上报），或把 `pop.hi` 初值改 0 并让键盘 Down 先行。

### [medium] [SUSPECTED] sftpd 建新文件后的 chmod 按路径而非 fd —— open 与 chmod 之间的路径替换竞态

位置：`c/apps/coreutils/sftpd.c:179-186`。

```c
int exists = st_lstat(p, &s) == 0;
h->fd = sys_open(p, mode); if (h->fd < 0) { rc = SF_PERM; break; }
if (st_fstat(h->fd, &s) < 0 || (s.mode & LST_IFMT) != LST_IFREG) { sys_close(h->fd); break; }
if (!exists && (a.flags & 4) &&
    (_sys(SYS_FSYNC,h->fd,0,0)<0 || st_chmod(p, a.mode & 0777) < 0)) { sys_close(h->fd); break; }
```

触发与后果：客户端带权限位创建新文件时，`sys_open(p)` 与 `st_chmod(p)` 是两个独立按路径的系统调用；同为该账号 uid 的本地进程（或另一并发 SFTP 请求）可在此窗口把 `p` rename/replace 成别的文件，chmod 落到替换者身上——权限位应用到非请求目标的文件。仅同 uid 可达（目录写权限即准入），且文件头注释已自认 ABI 无 O_EXCL、注释也解释了为何"pathname chmod is not a correct fchmod after rename"——但那句话针对的是 SETSTAT 分支，OPEN 分支这里同样成立。
修复建议：内核侧补 `SYS_FCHMOD`（或 setattr-by-fd），走 `st_fchmod(h->fd, ...)`；不可行时至少把 fsync+chmod 与 open 的窗口在文档中降级为"权限位为尽力而为"。

### [medium] [CONFIRMED] sh 前台作业的 stdin 写满即丢字节 —— 大粘贴到不读 stdin 的作业静默截断

位置：`c/apps/coreutils/sh.c:1029-1036`（写循环）、`sh.c:647-649`（CTL_JOB 收集侧同样有 1024 字节上限静默丢弃）。

```c
if (pend_text_n && j->stdin_fd >= 0) {
    int n = pend_text_n; pend_text_n = 0;
    for (int i = 0; i < n; ) {
        int w = sys_write(j->stdin_fd, pend_text + i, n - i);
        if (w <= 0) break;                     /* EAGAIN：剩余字节就此丢失 */
        i += w;
    }
}
```

触发与后果：前台作业运行中粘贴大段文本（> 内核管道余量减去已积压量），子进程暂不读 stdin（例如 `sleep 100` 后再回头读，或一个先算后读的程序）。`sys_write` 返回 EAGAIN 时循环 break，而 `pend_text_n` 已被清零——剩余字节无回执、无重试、无提示地消失。同一文件对 stdout 侧（`drain`、`copy`）处处"宁可等也不丢"，这里是唯一一个静默丢字节的方向。
修复建议：写不完时保留 `pend_text`（把 `pend_text_n` 改记剩余偏移），下轮 `wait_foreground` 循环继续写；缓冲满且长期 EAGAIN 再选择丢弃并回报丢弃字节数。

### [medium] [CONFIRMED] Terminal 视频/音频槽位回收用 `objgen_next % MAXVID`，不是 LRU —— 新槽可能踢掉最新片段而保留最老

位置：`c/apps/gui/terminal/terminal.c:1121`（视频）、`terminal.c:1403`（音频）、对照 `aui.c:1294`（anim 槽是真 LRU）。

```c
int slot = nvid < MAXVID ? nvid++ : (objgen_next % MAXVID);
struct vidobj *v = &vids[slot];
vid_free(v);                                    /* 该槽上的旧片段立即被停/释放 */
```

触发与后果：前两个视频各占 0/1 号槽；第三个视频取 `objgen_next % 2`——objgen_next 是全体滚动条对象共用的递增计数，奇偶交替，结果是最初两个视频之后，每来一个新片段踢掉的不是"最老的那个"而是奇偶命中的那个（两次中的一次是刚刚才加的那个）。注释声称"Recycling a slot stops the older clip"，实现并不保证；音频同理（MAXAUD=2）。用户可见症状：三个视频依次 `show` 时，第三个会让第二个（还在屏上的那个）突然定格，最老的那个反而还在播。
修复建议：给 vidobj/audobj 记录 `opened_ns` 或引用它的滚动行号，回收时挑最老者；或维护一个朴素的 FIFO next-victim 游标，而不是借用对象代数。

### [low] [CONFIRMED] preview 裸 H.264/HEVC 流播完一遍立即无声无息从头重播，永不结束

位置：`c/apps/gui/preview/preview.c:970-1028`（`play_annexb` 外层 `for (;;)`）。

```c
for (;;) {
    vdec v;
    if (!vdec_open(&v, is265)) return fail_with("out of memory");
    ...
    vdec_close(&v);
    if (frames == 0) return fail_with("no frames in this stream");
    plog("preview: stream %s frames=%d\n", name, frames);
}   /* 无 break：回到开头再放一遍 */
```

触发与后果：打开裸 Annex B 基本流（容器/图片/音频都不认、`looks_like_annexb` 命中）。容器路径播完会显示 "finished: N pictures"；这条路径播完最后一帧直接从第一帧重来，无限循环，只能 Esc/关窗退出，没有任何"已结束"状态。若是有意循环，缺一句注释与一个 loop 语义说明；按容器路径的行为看更像漏了收尾。
修复建议：外层循环去掉，播完 `vdec_close` 后落进与容器路径一致的 `show_message("finished ...")`。

### [low] [CONFIRMED] aui.c ck_report 的 `char b[220]` 无界拼接，算术上界 314 字节 —— 仅 -DAUI_COST 基准构建可达

位置：`c/apps/gui/aui.c:1727-1749`。

```c
char b[220]; int q = 0;
...
PUT("[gfx] w="); NUM((unsigned)win_w);
... /* 11 个 PUT+NUM 对：固定串 94 字节 + 每个数字最长 20 位 = 314 > 220 */
b[q++] = '\n';
```

触发与后果：全部 11 个计数值同时逼近 `unsigned long long` 上限（20 位）时写越 `b[220]` 最多约 94 字节。现实数值（每 2 秒清零的均值、四位窗口宽）远到不了；但该函数的存在意义是 bench 溢出告警，它自己是无界的。`NUM` 内部的 `t[24]` 是对的，只有 `b` 没有界。
修复建议：给 `PUT/NUM` 加 `q < (int)sizeof b - 1` 上限，与文件里其余"says the number"风格一致。

### [low] [CONFIRMED] aui_progress 非确定模式在 w==0 时 `% 0` —— 潜伏除零

位置：`c/apps/gui/aui.c:2181`。

```c
int seg = w / 3;
int t = (int)((aui_anim_loop() / 6) % (unsigned)(w + seg));   /* w==0 → % 0 */
```

触发与后果：`aui_progress(x, y, 0, -1)`（非确定）触发整数除零，ring-3 SIGFPE。函数开头对确定分支有 `w<=0` 的隐式防护（`aui_round` 自拒），非确定分支没有。当前所有调用方（settings.c:331 的 `w-32`、monitor、gallery）都传正值，属潜伏地雷——正是 CLAUDE.md 说的 silent failure class。
修复建议：函数入口 `if (w <= 0 || h <= 0) return;`，与 `aui_fill` 同款。

### [low] [CONFIRMED] login 空用户名 `tries--` 无限循环 —— 串口脚本可持续独占登录提示符

位置：`c/apps/coreutils/login.c:346-351`。

```c
for (int tries = 0; tries < 3; tries++) {
    outs("\nLOGIN: ");
    char name[ACCT_NAME];
    int n = readline(0, name, sizeof name);
    if (n < 0) return 1;
    if (n == 0) { tries--; continue; }       /* 空行不计入三试 */
```

触发与后果：向串口持续输入空行（脚本、粘贴、失控的自动机），`tries--` 使三试上限永不生效，/bin/login 永不退出、控制台提示符被独占。是便利性选择（空行多为误回车），但没有对连续空行的计数上限。
修复建议：单独计连续空行数，超过如 10 次按一次失败计。

### [low] [CONFIRMED] ls/dir/show 等 coreutil 对 argv 路径 `c_strcpy` 静默截断 —— 作用于错误路径

位置：`c/apps/coreutils/ls.c:74`、`c/apps/coreutils/dir.c:23`、`c/apps/coreutils/show.c:47,66`（同型）。

```c
} else { c_strcpy(path, argv[i], sizeof path); have_path = 1; }   /* ls.c，path[128] */
```

触发与后果：`ls /very/long/path...`（>127 字节）被截到 127 字节后当作目录名——报"no such directory"指向一个用户没输入过的路径，或更坏：截断后的前缀恰好存在时列出错误目录。同族的 `sh run_external` 超长命令名已改为响亮拒绝（sh.c:878-881），这批工具没有跟上。
修复建议：仿 sh.c：拷贝后 `strlen(argv[i]) >= sizeof path` 即 `errs("path too long")` 返回 1。

### [low] [CONFIRMED] `stat -u` 非八进制输入不报错，`oct()` 提前停止返回部分值 —— `stat -u 9` 把 umask 置 000

位置：`c/apps/coreutils/stat.c:84-85`、`stat.c:108`。

```c
static int oct(const char *s)
{ int v = 0; while (*s >= '0' && *s <= '7') v = v * 8 + (*s++ - '0'); return v; }
...
int prev = st_umask(argc >= 3 ? oct(argv[2]) : -1);
```

触发与后果：`stat -u 9`（或 `-u 8x`、`-u ""`）时 `oct()` 停在首个非法字符返回 0——把进程 umask 设为 000（创建文件最宽松），既不报错也不像查询。`stat -c/-o` 的 chmod/chown 参数同函数，`-c 9 file` 会 chmod 成 0000。与本文件其余地方"拒绝并说原因"的风格相悖。
修复建议：`oct()` 返回 -1 表失败（或带 endptr 校验消耗了全串），main 对 -1 打印 usage 返回 1。

### [low] [CONFIRMED] Terminal ^V 粘贴：剪贴板 4096 字节，typed[] 只有 256，超出静默丢弃

位置：`c/apps/gui/terminal/terminal.c:2532-2535`（^V）、`terminal.c:2165-2167`（typed 上限）。

```c
static char typed[256];
static int  typed_n;
static void type_char(char c) { if (typed_n < (int)sizeof typed) typed[typed_n++] = c; }
...
case 22:                                    /* ^V: paste */
    for (int i = 0; i < clip_n; i++) type_char(clip[i]);
```

触发与后果：复制一段 >256 字节的文本再 ^V，只有前 256 字节进 shell，其余无声丢失；剪贴板本身装了 4096 字节，说明 256 不是有意的一致上限。
修复建议：type_flush 改为按 typed 满即多次 `ctl_send(RT_C_TEXT)` 分帧（协议本就支持任意多条 TEXT 帧），type_char 满时先 flush 再装。

### [low] [CONFIRMED] sftpd READDIR 单项路径超长时该项从列表中永久丢失

位置：`c/apps/coreutils/sftpd.c:228-237`。

```c
int a = c_strlen(h->path), b = c_strlen(d[i].name);
if (a + b + 2 > SF_PATH) { status(id, SF_FAIL); return; }   /* cursor 已越过该项 */
```

触发与后果：目录里有一个"目录路径 + 名字"合计超 255 字节的项时，READDIR 在该项处整体报 SF_FAIL，但 `h->cursor` 已被 `st_getdents` 推过它——客户端重试也从下一项开始，该项从 SFTP 视图中永久消失（本机 shell `ls` 仍可见）。错误也无差别（SF_FAIL 而非 skip）。
修复建议：跳过不可表示的项继续装订其余条目（或对单目录一次性返回句柄级名字而非拼全路径），并保持失败可重入。

### [low] [CONFIRMED] files.c do_paste 忽略 copy_tree 返回值 —— 粘贴失败零提示

位置：`c/apps/gui/files/files.c:438-441`；`copy_tree`（files.c:341-356）内部对子项失败同样吞掉。

```c
if (path_under(clip[i], dst)) continue;
if (clip_cut) sys_rename(clip[i], dst);
else          copy_tree(clip[i], dst, 0);      /* 返回值未检查，递归内亦不上抛 */
```

触发与后果：目标盘满、目标名已存在为只读、深层子树中途失败——粘贴看起来成功（无 notice），实际只复制了一部分或什么都没复制；剪切模式 `clip_count = 0` 后原件已随 rename 移走尚可，复制模式下的部分结果是静默数据不完整。该应用有现成的 `agent_notice` 通道（rename/newfolder 都用了），粘贴没有。
修复建议：copy_tree 逐项回传失败并让 do_paste 把第一条失败写进 `agent_notice`；至少对顶层 `copy_tree` 返回值非 0 给提示。

---

## 已知问题（未重复上报）

以下各项在既有文档中已记录，本次核实其现状，不重复计数：

- **旧审计已知且已修**（复核确认修复在位）：terminal Tab 死循环（`terminal.c:627` 已带 `l->len < MAXCOLS` 防护）；files 多选删除索引漂移（选择已改为名字表示，files.c:183-237）；textedit 特殊键截断为控制字符（te_apply_key/te_utf8_encode 已修——但注意本报告的 KEY_F1..F12 是新 ABI 键带来的**新**同类缺口，非同一处）；`cp -r` 自递归（path_under 在位）；sh fork 失败分支（sh.c:929 已正确处理）；sh `run_external` 超长命令名（已改拒绝，sh.c:878）；cat/wc/head 短写（均已 retry）；studio 旧 gui/studio.c 的 `scan_error` 溢出与 EV_CLOSE 僵尸——旧文件已被 c/apps/studio 引擎重构取代，runner 有 WNOHANG reap（runner.c:183）+ B09 的 exit-code 归一（runner.c:186-192 注释）；TextEdit/Studio"读取失败当空文件可覆盖"（BUG_REVIEW B04）已在 textedit 以 `load_refused` 修复、Studio 以打开失败拒绝修复；B08"保存失败仍运行"已在 jobs.c:21-31 检查 `st_save_document` 返回。
- **已知遗留，仍在**：`sh` 无 quoting（含空格路径拆多参，CODE_AUDIT 遗留清单原文）；terminal `spawn_shell` 失败路径管道 fd 泄漏、shell 退出后 `in_w` 永不关（旧审计 low，现状未变）；copy_tree/delete_tree 递归深度虽有 TREE_DEPTH_MAX=32 上限但 `cp.c` 与 files.c 各持一份常数（两扇门）；`sshd` 无真实进程组/job control（文件头自述）；pty 无 `stty` 语义（自述）；`/etc/ai.conf` 可被任意 CAP_FS 进程读取（ch.c 文件头自述）；LogitFS 短目录读导致 Preview 多次扫描合并的 workaround（preview.c:1252-1277 注释自述，根因在 FS 线）。
- **跨区核实后剔除**：httpd.c:381 `_sys(SYS_WAITPID, pid, 0, 1)` 传 NULL 状态指针——已核对 `c/kernel/exec/proc.c:853,858` 的 `if (status)` 判空，安全，不报。
- **覆盖声明**：`ch/ch_sse.c`（有宿主门 test-ch-host/negctl）、`logit_sniff.h`、`httpd` 之外的 net 测试工具（ping/rec/sndtest/thrtest/polltest/socktest/schedtest/smptest/readcore 等，属测试装置）、clock/ 与 widgets/assistant 的绘制细节、settings.c 前 316 行、studio 的 diagnostics/navigation/highlight 逐行、`c/net/ssh/*`（明确归 c/net 线）未逐行深读；本报告不做全系统无遗漏声明。
