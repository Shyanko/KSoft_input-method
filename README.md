# Ksipl

C++17 / Qt 桌面文本编辑器，提供基于 `dicts/` 的内置中文拼音输入，以及 Qwen next-token 智能补全。按照 `agent.md` 实现；

## 演示视频

https://github.com/user-attachments/assets/a5660bc2-bbd6-4dbf-bcd3-c5cab05ee01e

## 构建与运行

需要 CMake 3.21+、支持 C++17 的编译器，以及与编译器匹配的 Qt 6（或 Qt 5.15）开发包。Qt 模块：Core、Gui、Widgets、Network；运行测试还需要 Test。Windows 的 MSVC Qt 包不能与 MinGW 混用。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/ksipl.exe
```

运行时将匹配的 Qt `bin` 和编译器运行库目录加入 `PATH`，或使用 `windeployqt --release build/ksipl.exe` 部署依赖。也可使用项目内的 `scripts/build.ps1` 自动发现本机 Qt、编译、测试并部署 Windows 程序。

```powershell
./scripts/build.ps1
./build/ksipl.exe
./build/ksipl.exe --dicts ./dicts --endpoint http://0.0.0.0:1237/v1/next-token
```

程序依次从可执行文件同级、上级、当前目录寻找 `dicts`。发布时把 `dicts` 放在可执行文件旁，或通过 `--dicts` 指定。Linux/macOS 使用同样的 CMake 配置，运行 `./build/ksipl`。

## 操作

| 功能 | 操作 |
| --- | --- |
| 新建窗口 / 标签页 | Ctrl+N / Ctrl+T |
| 打开 / 保存 / 另存为 | Ctrl+O / Ctrl+S / Ctrl+Shift+S |
| 关闭标签页 | Ctrl+W |
| 移动标签页 | 拖动调整顺序；文件菜单移到新窗口 |
| 切换内置中文拼音与英文/系统输入法 | F2，或工具栏按钮 |
| 拼音选词 | 空格接受选中项，数字 1～7 选择当前页对应项 |
| 拼音翻页 / 选中 | PgUp、PgDn 或 `-`、`=`；上下方向键 |
| 提交拼音原文 / 取消拼音 | Enter / Esc |
| 接受补全第一项 | Tab |
| 接受补全第 n 项 | Ctrl+1～7 |
| 隐藏当前补全 | Esc |

工具栏可以开关智能补全并设置 `k=1～7`；设置保存在 Qt 用户配置中。拼音预编辑结束后，光标停顿 350ms 显示竖向灰色补全列表；灰色建议不会写入正文、撤销栈或保存文件，接受后才作为一次编辑插入。列表也支持鼠标点击。

中文模式下，标点采用中文形式（例如 `, . ! ? ; : ()` → `，。！？；：（）`、`[]` → `【】`、`<>` → `《》`、`\` → `、`、`^` → `……`、`_` → `——`），引号按光标前的配对情况输入 `“”`、`‘’`。数字、字母、空格和其他符号保持原样。小写字母仍用于拼音，预编辑中的单引号仍用于拼音分隔；空格/数字选词、`-`/`=` 翻页、Enter 提交拼音原文和补全快捷键保持原有行为。预编辑期间输入中文标点会先提交当前选中候选，再插入标点；尚无候选时保留拼音原文。英文模式、粘贴内容和模型补全不做标点转换。

文件采用 UTF-8，保留已打开文件的 UTF-8 BOM 和 CRLF/LF 换行约定；非 UTF-8 文件拒绝打开，以避免误转换。保存使用 `QSaveFile` 原子提交，关闭有修改的文档时提示保存。

## 词库

读取 Rime YAML 的头部及 `...` 后的制表符数据，不需要额外 YAML 库。按 `8105`、`41448`、`base`、`ext`、`others`、`tencent` 的顺序加载，也支持目录内其他 `*.dict.yaml` 文件。相同拼音和词条优先保留先出现的记录。

- 支持连续全拼和带 `'` 的拼音，`ü` 对应 `v`，如 `nihao`、`ni'hao`、`lv`。
- 候选优先精确匹配，再按词频排序；支持前缀候选，最多返回 70 项，每页 7 项。
- 对 `tencent.dict.yaml` 的 `text, weight` 格式，使用字表中该字最高权重读音自动注音。已有显式注音词条保留多音字读音；自动注音不枚举所有多音字组合。无法逐字注音的词跳过。
- 这是进程内简化输入法，支持词库词条，不实现系统级输入法注册、整句分词、简拼或用户词频学习。内置中文模式关闭系统 IME；切换英文模式后可使用系统 IME。

## 两步搜索

请求模型接口：

```json
{"prefill":"The capital of France is","k":5}
```

响应支持 JSON token 数组，以及 `tokens`、`candidates`、`data`、`next_tokens` 包装的数组：

```json
[{"token_id":25701,"token":" Paris","logprob":-1.7674602270126343,"probability":0.17076614577660948}]
```

PowerShell 的 `@{...}` 是对象的显示形式；HTTP 响应需要使用有效 JSON。`probability` 缺省时从 `exp(logprob)` 计算，保留 token 文本中的空格。

**单 token 与双 token 路径同时排序**，不做长度归一化。比如 `P(A)=0.9`、`P(B|A)=0.8`，则 `A` 的概率为 `0.9`，`AB` 为 `0.72`，`A` 排在 `AB` 前。因此 `k=1` 时第一项始终是最可能的可插入单 token。

算法先获取根节点 top-k，纳入单 token 路径，再按父节点概率从高到低扩展到深度 2。每步保留 top-k；当父节点概率低于当前第 k 项时停止，因为所有后代概率都不可能更大；概率相等时也检查短路径优先的规则。含回车或换行（`\r`、`\n`）的 token 在每步搜索前被过滤，包含文字与换行的混合 token 也会整项屏蔽，不参与候选或后续扩展；保留原始概率，不重新归一化。空字符串和结束标记也不作为插入项；只在接口返回的 top-k 范围内筛选，过滤后结果可能少于 k 项。排序相同时优先短路径，再按 token ID 排序。

智能补全开启时，光标前最多 4096 个 UTF-16 单元会作为上下文发送到配置接口；默认是 `agent.md` 中的 HTTP 地址。模型请求总超时 20 秒，断网或响应格式错误会在状态栏显示，不影响本地编辑和拼音。

## IPC 与并发

每个编辑器实例通过 `QLocalSocket` 连接同一个后台 `ksipl --service` 进程。服务名由当前用户、词库绝对路径和模型地址散列得到；`QLockFile` 防止多进程同时启动重复服务，IPC 限当前用户访问。消息使用换行分隔 JSON，包含请求 ID，并限制消息大小。

词库在后台工作线程加载和查询；网络使用 Qt 异步请求。拼音查询防抖 35ms，模型补全防抖 350ms。正文、光标、输入状态和设置变化会取消旧补全；响应 ID、上下文和光标位置共同防止过期结果覆盖新结果。每个标签页使用独立连接，互不取消其他标签的任务。连接断开会自动重连并按需重启后台；最后一个客户端退出 30 秒后后台自动退出。

## 验证

Qt Test 覆盖 IPC 分帧与非法帧、词库列定义和自动注音、token 响应解析、随机概率树的搜索与穷举对照、取消请求、错误响应，以及编辑器拼音提交、Ctrl+数字、Tab、撤销、防抖与过期响应。另以完整词库启动独立后台进程，检查重复服务锁与多客户端查词，并验证 UTF-8/BOM/CRLF 保存、标签页和多窗口操作。测试使用本地 HTTP 模型模拟服务，不依赖远端模型可用性；详细结果位于 `build/test-results.txt`。
