# 引擎组件综合样例

从仓库根目录启动：

```sh
python3 -m http.server 8679 --bind 127.0.0.1 --directory tests/fixtures
```

QEMU user networking 下，在 LogitOS 浏览器打开 `http://10.0.2.2:8679/engine-components/index.html`。Host 浏览器可打开 `http://127.0.0.1:8679/engine-components/index.html`，但 host 的成功不能作为 LogitOS 支持证据。

本样例只使用普通 HTML/CSS/JS，不注入引擎私有测试函数、不使用站点特判。资源均来自当前本地目录。

逐组检查：模块状态必须由实际执行替换；三种原子容器与文本同行；表单编辑/选择/重置/提交结果一致；按钮新增/删除实际节点；任务顺序写入记录；存储切换 tab 后按 local/session 分别保留或隔离；fetch 和动态 import 必须取到独立文件；Canvas 必须显示两色并读取像素；最后检查普通链接/表单的真实导航。

dialog/popover、元素滚动、RTL 等是后续缺口样例，展示在这里不表示已通过。综合样例不输出整个引擎的兼容率。正式本批机制门禁是 `test-importmap`、`test-page-runtime`、`test-storage-backend` 与布局组件对应门禁；guest 观察另行记录。
