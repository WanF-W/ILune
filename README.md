# ILune

> A dedicated CLI for Il2CppLua — automates DLL injection and handles IPC via Windows Named Pipes — v2.1.0

## 简介

ILune 是 [Il2CppLua](../Il2CppLua) 的配套 CLI 工具：定位目标进程、把
Il2CppLua.dll 注入游戏进程、通过 Windows 命名管道与 DLL 通信，并提供交互式
Lua REPL 控制台。两者必须配套使用，版本通过 HELLO 帧握手校验。

## 特性

- 按进程名（`-n`）或 PID（`-p`）定位目标进程
- 自动在 `ilune.exe` 同目录查找 `Il2CppLua.dll`，或用 `-d` 显式指定
- `CreateRemoteThread + LoadLibraryW` 注入，失败时回退 `NtCreateThreadEx`
- 通过共享内存向 DLL 传递管道名称
- 命名管道 IPC：Lua `print` / Hook 回调日志实时显示、错误着色输出、
  版本握手校验、超时保护
- 交互式 REPL：自动识别语句与表达式，表达式返回值自动回显
- 启动时执行 Lua 脚本（`-l`）
- `exit` / `quit` 命令与 Ctrl+C 安全退出

## 用法

```bat
ilune -n Game.exe                 :: 按进程名附加
ilune -p 1234                     :: 按 PID 附加
ilune -n Game.exe -d C:\mods\Il2CppLua.dll
ilune -n Game.exe -l startup.lua  :: 附加并执行启动脚本
```

命令行参数：

| 参数 | 说明 |
| --- | --- |
| `-n` / `--name` | 目标进程名（不区分大小写） |
| `-p` / `--pid` | 目标进程 ID |
| `-d` / `--dll` | Il2CppLua.dll 路径（默认在 exe 同目录查找） |
| `-l` / `--lua` | 附加成功后自动执行的 Lua 脚本路径 |

`-n` 与 `-p` 必须指定其一。

## 工作流程

1. 解析命令行参数，定位目标进程与 DLL 路径。
2. 创建命名管道服务器 `\\.\pipe\Il2CppLua_<PID>`。
3. 注入 DLL：创建共享内存写入管道名 → 远程线程加载 DLL →
   等待 LoadLibraryW 返回确认。
4. 等待 DLL 连接管道，接收 HELLO 帧校验版本，再等待 READY 帧
   （DLL 完成 IL2CPP 解析与 Lua 初始化）。
5. 若指定了 `-l`，先执行启动脚本。
6. 进入 REPL 循环，直到用户输入 `exit` / `quit`、Ctrl+C 或管道断开。

## 通信协议

帧格式：`[1 字节类型][4 字节小端长度][N 字节负载]`，最大负载 1 MB。

| 方向 | 类型 | 说明 |
| --- | --- | --- |
| DLL → EXE | `MSG_HELLO` | 版本握手，负载为 `Il2CppLua/2.1.0` |
| DLL → EXE | `MSG_READY` | IL2CPP + Lua 初始化完成 |
| DLL → EXE | `MSG_LOG` | Lua 输出（print / 返回值回显 / 日志） |
| DLL → EXE | `MSG_ERROR` | 命令执行错误 |
| DLL → EXE | `MSG_OK` | 命令执行成功 |
| EXE → DLL | `MSG_CMD` | 执行 Lua 代码字符串 |
| EXE → DLL | `MSG_FILE` | 执行 Lua 文件（负载为路径） |
| 双向 | `MSG_EXIT` | 退出通知 |

协议定义在 `src/protocol.h`，DLL 与 EXE 两端共享同一份，保证一致。

## REPL 说明

- 输入是赋值 / 注释等语句时原样发送；是表达式时自动包装为 `return <expr>`
  以便回显返回值。
- `print` 输出与 Hook 回调输出（含 `il2cpp.mainThread.schedule` 任务）实时
  显示，不阻塞输入。
- 每条输入（无论有无输出）与下一个 `ilune >>` 之间空一行作为分割；
  异步输出会先换行独占一行再恢复提示符。

```text
ilune >> print("hello")
hello

ilune >> 1 + 2
3

ilune >> 
```

## 构建

环境要求：

- Windows x64
- Visual Studio 2022（平台工具集 v145）
- Windows SDK 10.0

步骤：

1. 打开 `ilune.slnx`（或直接打开 `ilune.vcxproj`）。
2. 配置选择 **Release | x64**。
3. 生成解决方案，产物为 `ilune.exe`。

把 `ilune.exe` 与编译好的 `Il2CppLua.dll` 放在同一目录即可使用。

## 目录结构

```
src/
  ilune.cpp          主程序：参数解析、REPL、输出渲染
  injector.*         进程定位与 DLL 注入
  pipe_server.*      EXE 侧命名管道服务器
  protocol.h         两端共享的通信协议定义
  common.h           公共类型
```

## 已知限制

- 仅 Windows x64。
- 目标游戏为 Unity IL2CPP（需等待 GameAssembly.dll 加载，默认最长等待
  30 秒）。
- 注入可能需要管理员权限（视目标进程保护程度而定）。

## License

MIT License. 详见 [LICENSE](LICENSE)。
