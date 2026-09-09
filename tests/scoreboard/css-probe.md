# Site scoreboard css-probe

commit d6d6a0ae5, ISO build/logit.iso (sha256:9da63eee1b542124), 1 sites (+0 controls), 1 run(s) each, 85 s wall

HARNESS 1

WHAT THIS TABLE IS NOT. `changed px` counts pixels that differ from an empty tab
photographed in the same boot. It cannot tell a rendered page from a flat dark
block: bing scores 625,312 changed pixels and is exactly the "一坨黑黑的" that was
reported by hand. Nothing here checks whether the RIGHT pixels changed -- that is
what reftests are for, and none of WPT's 17,155 of them run on this machine. The
top verdict is `PAINTED`, which means pixels changed, no script threw, and the
guest asked for everything the document requires. It does not mean correct.

The two `control-` rows are NOT results. They exist to prove the harness, the
network and the build were working during the pass; if either fails, no other row
in the snapshot means anything. They say nothing about the browser.

| site               | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| bing-search        | HARNESS    |      - |     - |        - |   - |          - |           - | unreachable |

## Detail

### bing-search -- HARNESS
url:      https://www.bing.com/search?q=python
reported: "我使用bing搜索python成功返回了结果!!!可是......CSS渲染根本无法正常使用"
verdict:  the Dock never launched the Browser
host:     UNREACHABLE (URLError: <urlopen error [SSL: UNEXPECTED_EOF_WHILE_READING] EOF occurred in violation of protocol (_ssl.c:1000)>)
