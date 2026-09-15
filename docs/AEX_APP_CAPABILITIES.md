# AEX v3 应用能力表

登记表：`c/apps/agent/catalog.json`。本次实际构建并启动验证 75 个自有程序：13 个桌面应用、62 个命令行/服务程序。浏览器排除在这次迁移外。

所有列出的程序都接入内核认证身份、双模式激活、语义上下文、受限代理和独立记忆接口。常规启动继续进入原程序入口。

Finder/TextEdit 实现资料到报告与持续修改；Tasks/agentctl 提供用户任务控制。其余应用的代理接受显式文本上下文，提供领域分析和文本草稿。表中的 `analyze_context` / `draft_text` 不表示该代理能自动执行原命令、操作设备或改写系统设置。GUI 上下文由应用主动发布，不使用截图。

| 程序 | AppID | 安装路径 | 类型 | 清单动作 |
| --- | --- | --- | --- | --- |
| files | `os.logit.finder` | `/files.aex` | gui | `read_sources` |
| textedit | `os.logit.textedit` | `/textedit.aex` | gui | `read_document`, `write_candidate`, `revise_document` |
| assistant | `os.logit.assistant` | `/assistant.aex` | gui | `analyze_context`, `draft_text` |
| agentd | `os.logit.agentd` | `/bin/agentd` | cli | `analyze_context`, `draft_text` |
| agentctl | `os.logit.agentctl` | `/bin/agentctl` | cli | `status`, `pause`, `cancel`, `resume`, `revise` |
| as | `os.logit.as` | `/bin/as` | cli | `analyze_context`, `draft_text` |
| audiocheck | `os.logit.audiocheck` | `/bin/audiocheck` | cli | `analyze_context`, `draft_text` |
| cat | `os.logit.cat` | `/bin/cat` | cli | `analyze_context`, `draft_text` |
| ch | `os.logit.ch` | `/ch.aex` | gui | `analyze_context`, `draft_text` |
| chart | `os.logit.chart` | `/bin/chart` | cli | `analyze_context`, `draft_text` |
| clear | `os.logit.clear` | `/bin/clear` | cli | `analyze_context`, `draft_text` |
| clip | `os.logit.clip` | `/bin/clip` | cli | `analyze_context`, `draft_text` |
| clock | `os.logit.clock` | `/clock.aex` | gui | `analyze_context`, `draft_text` |
| cp | `os.logit.cp` | `/bin/cp` | cli | `analyze_context`, `draft_text` |
| crash | `os.logit.crash` | `/bin/crash` | cli | `analyze_context`, `draft_text` |
| demuxcheck | `os.logit.demuxcheck` | `/bin/demuxcheck` | cli | `analyze_context`, `draft_text` |
| dir | `os.logit.dir` | `/bin/dir` | cli | `analyze_context`, `draft_text` |
| echo | `os.logit.echo` | `/bin/echo` | cli | `analyze_context`, `draft_text` |
| entropy | `os.logit.entropy` | `/bin/entropy` | cli | `analyze_context`, `draft_text` |
| execinfo | `os.logit.execinfo` | `/bin/execinfo` | cli | `analyze_context`, `draft_text` |
| false | `os.logit.false` | `/bin/false` | cli | `analyze_context`, `draft_text` |
| free | `os.logit.free` | `/bin/free` | cli | `analyze_context`, `draft_text` |
| gallery | `os.logit.gallery` | `/gallery.aex` | gui | `analyze_context`, `draft_text` |
| greeter | `os.logit.greeter` | `/sbin/greeter.aex` | gui | `analyze_context`, `draft_text` |
| h2check | `os.logit.h2check` | `/bin/h2check` | cli | `analyze_context`, `draft_text` |
| head | `os.logit.head` | `/bin/head` | cli | `analyze_context`, `draft_text` |
| httpd | `os.logit.httpd` | `/bin/httpd` | cli | `analyze_context`, `draft_text` |
| lm | `os.logit.lm` | `/bin/lm` | cli | `analyze_context`, `draft_text` |
| login | `os.logit.login` | `/bin/login` | cli | `analyze_context`, `draft_text` |
| ls | `os.logit.ls` | `/bin/ls` | cli | `analyze_context`, `draft_text` |
| mkdir | `os.logit.mkdir` | `/bin/mkdir` | cli | `analyze_context`, `draft_text` |
| monitor | `os.logit.monitor` | `/monitor.aex` | gui | `analyze_context`, `draft_text` |
| msecheck | `os.logit.msecheck` | `/bin/msecheck` | cli | `analyze_context`, `draft_text` |
| mv | `os.logit.mv` | `/bin/mv` | cli | `analyze_context`, `draft_text` |
| net | `os.logit.net` | `/bin/net` | cli | `analyze_context`, `draft_text` |
| nice | `os.logit.nice` | `/bin/nice` | cli | `analyze_context`, `draft_text` |
| notify | `os.logit.notify` | `/bin/notify` | cli | `analyze_context`, `draft_text` |
| ping | `os.logit.ping` | `/bin/ping` | cli | `analyze_context`, `draft_text` |
| pkgverify | `os.logit.pkgverify` | `/bin/pkgverify` | cli | `analyze_context`, `draft_text` |
| polltest | `os.logit.polltest` | `/bin/polltest` | cli | `analyze_context`, `draft_text` |
| poweroff | `os.logit.poweroff` | `/bin/poweroff` | cli | `analyze_context`, `draft_text` |
| pref | `os.logit.pref` | `/bin/pref` | cli | `analyze_context`, `draft_text` |
| preview | `os.logit.preview` | `/preview.aex` | gui | `analyze_context`, `draft_text` |
| prog | `os.logit.prog` | `/bin/prog` | cli | `analyze_context`, `draft_text` |
| ps | `os.logit.ps` | `/bin/ps` | cli | `analyze_context`, `draft_text` |
| pwd | `os.logit.pwd` | `/bin/pwd` | cli | `analyze_context`, `draft_text` |
| readcore | `os.logit.readcore` | `/bin/readcore` | cli | `analyze_context`, `draft_text` |
| reboot | `os.logit.reboot` | `/bin/reboot` | cli | `analyze_context`, `draft_text` |
| rec | `os.logit.rec` | `/bin/rec` | cli | `analyze_context`, `draft_text` |
| renice | `os.logit.renice` | `/bin/renice` | cli | `analyze_context`, `draft_text` |
| rm | `os.logit.rm` | `/bin/rm` | cli | `analyze_context`, `draft_text` |
| schedtest | `os.logit.schedtest` | `/bin/schedtest` | cli | `analyze_context`, `draft_text` |
| settings | `os.logit.settings` | `/settings.aex` | gui | `analyze_context`, `draft_text` |
| sftpd | `os.logit.sftpd` | `/bin/sftpd` | cli | `analyze_context`, `draft_text` |
| sh | `os.logit.sh` | `/bin/sh` | cli | `analyze_context`, `draft_text` |
| show | `os.logit.show` | `/bin/show` | cli | `analyze_context`, `draft_text` |
| sleep | `os.logit.sleep` | `/bin/sleep` | cli | `analyze_context`, `draft_text` |
| smptest | `os.logit.smptest` | `/bin/smptest` | cli | `analyze_context`, `draft_text` |
| sndtest | `os.logit.sndtest` | `/bin/sndtest` | cli | `analyze_context`, `draft_text` |
| socktest | `os.logit.socktest` | `/bin/socktest` | cli | `analyze_context`, `draft_text` |
| sshd | `os.logit.sshd` | `/bin/sshd` | cli | `analyze_context`, `draft_text` |
| stat | `os.logit.stat` | `/bin/stat` | cli | `analyze_context`, `draft_text` |
| studio | `os.logit.studio` | `/studio.aex` | gui | `analyze_context`, `draft_text` |
| syslogd | `os.logit.syslogd` | `/bin/syslogd` | cli | `analyze_context`, `draft_text` |
| terminal | `os.logit.terminal` | `/terminal.aex` | gui | `analyze_context`, `draft_text` |
| thrtest | `os.logit.thrtest` | `/bin/thrtest` | cli | `analyze_context`, `draft_text` |
| touch | `os.logit.touch` | `/bin/touch` | cli | `analyze_context`, `draft_text` |
| true | `os.logit.true` | `/bin/true` | cli | `analyze_context`, `draft_text` |
| uname | `os.logit.uname` | `/bin/uname` | cli | `analyze_context`, `draft_text` |
| uptime | `os.logit.uptime` | `/bin/uptime` | cli | `analyze_context`, `draft_text` |
| vidbench | `os.logit.vidbench` | `/bin/vidbench` | cli | `analyze_context`, `draft_text` |
| vidcheck | `os.logit.vidcheck` | `/bin/vidcheck` | cli | `analyze_context`, `draft_text` |
| vidcheck265 | `os.logit.vidcheck265` | `/bin/vidcheck265` | cli | `analyze_context`, `draft_text` |
| wc | `os.logit.wc` | `/bin/wc` | cli | `analyze_context`, `draft_text` |
| widgets | `os.logit.widgets` | `/widgets.aex` | gui | `analyze_context`, `draft_text` |

登记表 SHA-256：`0ac5c90111592624995b7de8bee2b4de497c8a405f799f4b5261e95e111c6f83`。

`test-agent-catalog-build` 验证每份实际 ELF/AEX 的清单、校验、激活入口和身份；QEMU 故障入口使用 `--catalog` 逐一启动这些程序并核对实际内核身份。此项证明全部接线，不等于逐一验证媒体解码、网络服务、关机等每个应用的全部原生功能。
