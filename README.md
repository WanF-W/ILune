<div align="center">

# 💻 ILune

Il2CppLua 的配套 CLI：注入 DLL + 交互式 Lua 控制台

**v2.1.0** · Windows x64 · MIT

</div>

## 📑 目录

- [简介](#intro)
- [特性](#features)
- [快速开始](#quickstart)
- [命令行参数](#args)
- [工作流程](#flow)
- [通信协议](#protocol)
- [REPL 说明](#repl)
- [构建](#build)
- [目录结构](#layout)
- [已知限制](#limits)
- [License](#license)

<a id="intro"></a>
## 简介

ILune 是 [Il2CppLua](../Il2CppLua) 的配套 CLI 工具：定位目标进程、注入 DLL、
通过 Windows 命名管道与游戏内的 Il2CppLua.dll 通信，并提供交互式 Lua REPL。
两者必须配套使用，版本通过 HELLO 帧握手校验。

<a id="features"></a>
## ✨ 特性

- 按进程名或 PID 定位目标
- 自动查找 Il2CppLua.dll（exe 同目录），也可 `-d` 指定
- CreateRemoteThread 注入，失败回退 NtCreateThreadEx
- 共享内存传递管道名，命名管道 IPC
- Lua `print` / Hook 日志实时显示，错误着色输出
- 语句与表达式自动识别，返回值自动回显
- `-l` 启动脚本，`exit` / Ctrl+C 安全退出

<a id="quickstart"></a>
## 🚀 快速开始

```bat
ilune -n Game.exe
```

启动游戏后在提示符直接输入 Lua：

```lua
local hero = il2cpp.get_class("MyGame", "HeroData")
print(hero:get_name())
```

<a id="args"></a>
## ⌨️ 命令行参数

| 参数 | 说明 |
| --- | --- |
| `-n` / `--name` | 目标进程名（不区分大小写） |
| `-p` / `--pid` | 目标进程 ID |
| `-d` / `--dll` | DLL 路径，默认 exe 同目录 |
| `-l` / `--lua` | 附加成功后自动执行脚本 |

`-n` 与 `-p` 必须指定其一：

```bat
ilune -n Game.exe
ilune -p 1234
ilune -n Game.exe -d C:\mods\Il2CppLua.dll
ilune -n Game.exe -l startup.lua
```

<a id="flow"></a>
## 🔄 工作流程

1. 解析参数，定位进程与 DLL
2. 创建命名管道服务器 `\\.\pipe\Il2CppLua_<PID>`
3. 注入 DLL（共享内存传管道名 → 远程线程 LoadLibraryW）
4. 等待连接 + HELLO 版本握手 + READY
5. 执行 `-l` 启动脚本（可选）
6. 进入 REPL，直到 `exit` / Ctrl+C / 断线

<a id="protocol"></a>
## 📡 通信协议

帧格式：`[1 字节类型][4 字节小端长度][负载]`，最大负载 1 MB。

| 方向 | 类型 | 说明 |
| --- | --- | --- |
| DLL → EXE | `MSG_HELLO` | 版本握手（`Il2CppLua/2.1.0`） |
| DLL → EXE | `MSG_READY` | 初始化完成 |
| DLL → EXE | `MSG_LOG` | Lua 输出 |
| DLL → EXE | `MSG_ERROR` / `MSG_OK` | 错误 / 成功 |
| EXE → DLL | `MSG_CMD` / `MSG_FILE` | 执行代码 / 文件 |
| 双向 | `MSG_EXIT` | 退出 |

两端共用 `src/protocol.h`，保证协议一致。

<a id="repl"></a>
## 💬 REPL 说明

- 赋值、注释等语句原样发送；表达式自动包装为 `return <expr>` 回显
- 输出实时显示，不阻塞输入
- 每条输入与下一个提示符之间空一行；异步输出独占一行后恢复提示符

```text
ilune >> print("hello")
hello

ilune >> 1 + 2
3

ilune >> 
```

<a id="build"></a>
## 🔨 构建

环境要求：Windows x64、Visual Studio 2022（工具集 v145）、Windows SDK 10.0。

```bat
:: 打开 ilune.slnx，选择 Release | x64 生成
:: 产物：ilune.exe，与 Il2CppLua.dll 放同一目录
```

<a id="layout"></a>
## 📂 目录结构

```text
src/
  ilune.cpp       主程序：参数解析、REPL、输出渲染
  injector.*      进程定位与 DLL 注入
  pipe_server.*   EXE 侧命名管道服务器
  protocol.h      两端共享协议
  common.h        公共类型
```

<a id="limits"></a>
## ⚠️ 已知限制

- 仅 Windows x64
- 目标必须是 Unity IL2CPP 游戏（等待 GameAssembly.dll 加载，最长 30 秒）
- 注入可能需要管理员权限

<a id="license"></a>
## 📄 License

MIT License，详见 [LICENSE](LICENSE)。
