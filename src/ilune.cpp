/**
 * ============================================================
 * ilune.cpp — Il2CppLua v2.1.0 注入器主程序
 * ============================================================
 * 本文件是 ilune.exe 的主入口 负责
 * 
 * ·命令行参数解析
 * ·目标进程定位
 * ·DLL 定位与注入
 * ·管道服务器管理
 * ·握手流程（HELLO → READY）
 * ·交互式 REPL（读取-求值-打印循环）
 * ·自动回显（表达式自动包装 il2cpp.inspect）
 * ·彩色控制台输出
 *
 * 用法
 * 
 * ·ilune.exe -n <进程名> [-d <dll路径>] [-l <lua脚本>]
 * ·ilune.exe -p <PID>   [-d <dll路径>] [-l <lua脚本>]
 *
 * REPL 特性
 * 
 * ·输入表达式自动回显结果（通过 il2cpp.inspect 包装）
 * ·输入语句原样执行
 * ·输入 "exit" 或 "quit" 退出并发送 MSG_EXIT 给 DLL
 * ·Lua print 输出实时显示
 *
 * 仅针对 Windows x64
 * ============================================================
 */

#include "injector.h"
#include "pipe_server.h"
#include "protocol.h"

// Windows API
#include <windows.h>

// C/C++ 标准库
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>


// ============================================================
// 控制台颜色
// ============================================================
// 使用 SetConsoleTextAttribute 设置文本颜色
namespace color
{
    // 设置控制台文本属性
    static void Set(WORD attr)
    {
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), attr);
    }

    // 重置为默认白色
    static void Reset() { Set(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); }

    // 红色（错误）
    static void Red()    { Set(FOREGROUND_RED | FOREGROUND_INTENSITY); }

    // 绿色（成功）
    static void Green()  { Set(FOREGROUND_GREEN | FOREGROUND_INTENSITY); }

    // 黄色（提示/警告）
    static void Yellow() { Set(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY); }

    // 青色（标题/横幅）
    static void Cyan()   { Set(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY); }

    // 灰色（次要信息）
    static void Gray()   { Set(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); }
}


// ============================================================
// 命令行参数结构
// ============================================================
struct Args
{
    std::wstring processName;   // -n: 目标进程名
    DWORD        pid = 0;       // -p: 目标进程 ID
    std::wstring dllPath;       // -d: DLL 路径
    std::wstring luaScript;     // -l: 启动时执行的 Lua 脚本文件
    bool         valid = false; // 参数是否有效
};


// ============================================================
// 全局管道服务器指针（用于 Ctrl+C 信号处理）
// ============================================================
static PipeServer* g_pServer = nullptr;


// ============================================================
// Ctrl+C 信号处理函数
// ============================================================
// 当用户按下 Ctrl+C 时 发送 MSG_EXIT 给 DLL 并清理资源
static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT)
    {
        // 发送退出帧给 DLL
        if (g_pServer != nullptr && g_pServer->IsConnected())
        {
            g_pServer->SendFrame(protocol::MSG_EXIT, nullptr, 0);
        }

        // 强制退出进程
        ExitProcess(0);
    }
    return FALSE;
}


// ============================================================
// 命令行参数解析
// ============================================================
static Args ParseArgs(int argc, wchar_t* argv[])
{
    Args args;

    // 遍历所有参数
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];

        // -n / --name: 目标进程名
        if ((arg == L"-n" || arg == L"--name") && i + 1 < argc)
        {
            args.processName = argv[++i];
        }
        // -p / --pid: 目标进程 ID
        else if ((arg == L"-p" || arg == L"--pid") && i + 1 < argc)
        {
            args.pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        }
        // -d / --dll: DLL 路径
        else if ((arg == L"-d" || arg == L"--dll") && i + 1 < argc)
        {
            args.dllPath = argv[++i];
        }
        // -l / --lua: 启动 Lua 脚本
        else if ((arg == L"-l" || arg == L"--lua") && i + 1 < argc)
        {
            args.luaScript = argv[++i];
        }
    }

    // 验证参数：必须指定 -n 或 -p
    args.valid = (args.pid != 0) || !args.processName.empty();

    return args;
}

// ============================================================
// 打印横幅
// ============================================================
static void PrintBanner()
{
    color::Cyan();
    std::wcout << L"===============================================================\n";
    color::Yellow();
    std::wcout << L"\nILune For Il2CppLua v2.1.0\n";
    std::wcout << L"  - A Native C++ Bridge For Lua Interaction With Il2Cpp\n\n";
    color::Cyan();
    std::wcout << L"===============================================================\n\n";
    color::Reset();
}

// ============================================================
// 打印帮助
// ============================================================
static void PrintHelp()
{
    color::Red();
    std::wcout << L"\nError cmd\n\n";
    color::Reset();
    std::wcout << L"Using like \"ilune -n/p xxx.exe/1234\" to attach target process\n";
    std::wcout << L"  extra args\n";
    std::wcout << L"   -d --dll      Specify DLL path\n";
    std::wcout << L"   -l --lua      Start with a .lua file path\n";
}

// ============================================================
// 定位 DLL 路径
// ============================================================
static std::wstring LocateDll(const std::wstring& explicitPath)
{
    // 如果用户显式指定了路径 验证文件是否存在
    if (!explicitPath.empty())
    {
        if (GetFileAttributesW(explicitPath.c_str()) != INVALID_FILE_ATTRIBUTES) return explicitPath;

        color::Red();
        std::wcerr << L"[!] DLL not found: " << explicitPath << std::endl;
        color::Reset();

        // 文件不存在
        return L"";
    }

    // 自动搜索：在 EXE 所在目录查找 Il2CppLua.dll
    wchar_t exeDir[MAX_PATH];

    // 获取 EXE 完整路径
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);

    // 查找最后的路径分隔符
    wchar_t* lastSlash = wcsrchr(exeDir, L'\\');

    // 截取目录部分
    if (lastSlash) *lastSlash = L'\0';

    // 构造候选路径：{exeDir}\Il2CppLua.dll
    wchar_t candidate[MAX_PATH];
    swprintf_s(candidate, MAX_PATH, L"%s\\Il2CppLua.dll", exeDir);

    // 找到 DLL
    if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES)return candidate;

    // 未找到 DLL
    color::Red();
    std::wcerr << L"[!] Cannot find Il2CppLua.dll. Place it next to ilune.exe\n";
    std::wcerr << L"    or use -d to specify the path explicitly.\n";
    color::Reset();

    return L"";
}


// ============================================================
// 定位目标进程
// ============================================================
static DWORD LocateTarget(const Args& args)
{
    // 如果指定了 PID 直接验证
    if (args.pid != 0)
    {
        HANDLE h = Injector::OpenTargetProcess(args.pid);

        // PID 有效
        if (h)
        {
            CloseHandle(h);
            return args.pid;
        }

        // PID 无效
        color::Red();
        std::wcerr << L"[!] Cannot open process with PID " << args.pid << std::endl;
        color::Reset();
        return 0;
    }

    // 按进程名查找
    DWORD pid = Injector::FindProcessByName(args.processName);
    if (pid == 0)
    {
        color::Red();
        std::wcerr << L"[!] Process not found: " << args.processName << std::endl;
        color::Reset();
    }

    return pid;
}

// ============================================================
// 最健壮的字符编码转换交互
// ============================================================
// 固定获取控制台 UTF-16 输入转 UTF-8 命名管道输出
// 固定获取命名管道 UTF-8 输入转 UTF-16 控制台输出
// EXE 端正常使用 std::wcxx
// 隔绝一切转码问题及 Windows 自身 Bug

// ============================================================
// UTF-8 转 UTF-16 输出到控制台
// ============================================================
static void SafePrintUtf8(const std::string& utf8_str, bool useStderr = false) {

    // 空字符串直接返回
    if (utf8_str.empty()) return;

    // 获取控制台输出句柄
    HANDLE hConsole = GetStdHandle(useStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    // 安全检查：句柄是否有效
    if (hConsole == INVALID_HANDLE_VALUE) return;

    // 计算需要的 UTF-16 字符数
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), static_cast<int>(utf8_str.size()), NULL, 0);
    // 安全检查
    if (wlen <= 0) return;

    // 分配缓冲区
    std::wstring wstr(wlen, L'\0');
    // 转换
    int converted_len = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), static_cast<int>(utf8_str.size()), &wstr[0], wlen);
    // 安全检查
    if (converted_len == 0) return;

    // 使用 WriteConsoleW 直接输出 无视系统代码页
    DWORD written;
    WriteConsoleW(hConsole, wstr.c_str(), static_cast<DWORD>(converted_len), &written, NULL);
}

// ============================================================
// 控制台读取一行 UTF-16 转 UTF-8
// ============================================================
static bool SafeReadUtf8(std::string& outUtf8)
{
    // 获取控制台输入句柄
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    // 安全检查：句柄是否有效
    if (hIn == INVALID_HANDLE_VALUE) return false;

    // 读取 UTF-16 输入
    std::wstring wline;
    wchar_t buf[256]{};
    DWORD readCount = 0;

    // 使用循环读取 直到遇到换行符
    while (true)
    {
        // ReadConsoleW 一次可能读多行（粘贴） 也可能读半行
        // 读取失败
        if (!ReadConsoleW(hIn, buf, 255, &readCount, nullptr)) return false;

        // EOF
        if (readCount == 0) return false;

        // 查找换行符
        for (DWORD i = 0; i < readCount; ++i)
        {
            // 跳过 \r
            if (buf[i] == L'\r') continue;

            // 换行 读取结束
            if (buf[i] == L'\n') goto done_read;

            // 拼接读取结果
            wline += buf[i];
        }
    }

done_read:
    // 转换为 UTF-8
    // 空输入内容
    if (wline.empty())
    {
        outUtf8.clear();
        return true;
    }

    // 计算需要的 UTF-8 字符数
    int len = WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), static_cast<int>(wline.size()), nullptr, 0, nullptr, nullptr);

    // 安全检查
    if (len <= 0)
    {
        outUtf8.clear();
        return false;
    }

    // 转换
    outUtf8.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), static_cast<int>(wline.size()), &outUtf8[0], len, nullptr, nullptr);

    return true;
}

// ============================================================
// 读取文件内容为字符串
// ============================================================
// 注意要求 .lua 文件编码为 UTF-8
static std::string ReadFileContent(const std::wstring& path)
{
    // 以二进制模式打开文件
    std::ifstream file(path, std::ios::binary);

    // 文件打开失败
    if (!file) return "";

    // 读取全部内容到字符串
    std::ostringstream ss;
    ss << file.rdbuf();

    return ss.str();
}

// ============================================================
// 判断输入是否为语句（而非表达式）
// ============================================================
// 用于 auto-echo：如果是语句 原样发送 如果是表达式 
// 包装为 `return il2cpp.inspect(<expr>)` 以自动显示结果
static bool IsStatement(const std::string& input)
{
    // 空输入视为语句
    if (input.empty()) return true;

    // 跳过前导空白
    size_t start = input.find_first_not_of(" \t\r\n");

    // 全是空白
    if (start == std::string::npos) return true;

    // 多行输入视为语句
    if (input.find('\n') != std::string::npos) return true;

    // 检查是否以 Lua 关键字开头
    static const char* keywords[] = {
        "local", "return", "if", "for", "while", "do",
        "function", "repeat", "until", "break", "end"
    };

    for (const char* kw : keywords)
    {
        size_t kwLen = strlen(kw);

        // 比较输入开头是否匹配关键字
        if (input.compare(start, kwLen, kw) == 0)
        {
            // 确保关键字后是单词边界（非字母/数字/下划线）
            char next = (start + kwLen < input.length()) ? input[start + kwLen] : '\0';
            // 是关键字开头的语句
            if (!isalnum(static_cast<unsigned char>(next)) && next != '_') return true;
        }
    }

    // 检查是否包含赋值符号 = （但不是 ==, ~=, >=, <=）
    for (size_t i = start; i < input.length(); ++i)
    {
        if (input[i] == '=')
        {
            char next = (i + 1 < input.length()) ? input[i + 1] : '\0';
            char prev = (i > 0) ? input[i - 1] : '\0';

            // ==：等于比较
            if (next == '=')
            {
                // 跳过下一个 =
                i++;
                continue;
            }

            // ~=, >=, <=
            if (prev == '~' || prev == '>' || prev == '<') continue;

            // 找到赋值"=",是赋值语句
            return true;
        }
    }

    // 检查是否以 -- 开头（注释）
    if (input.compare(start, 2, "--") == 0) return true;

    // 不是语句 是表达式
    return false;
}


// ============================================================
// 执行一条 Lua 命令
// ============================================================
// 发送 MSG_CMD 等待并处理响应帧（LOG / OK / ERROR）
//
// @param server 管道服务器
// @param code Lua 代码（UTF-8）
// @param timeoutMs 超时时间
// @return true 执行成功 false 失败或超时
static bool ExecuteCommand(PipeServer& server, const std::string& code, int timeoutMs = protocol::COMMAND_TIMEOUT)
{
    // 发送 Lua 代码到 DLL
    if (!server.SendFrame(protocol::MSG_CMD, code.c_str(), static_cast<uint32_t>(code.size())))
    {
        color::Red();
        std::wcerr << L"[!] Failed to send command\n";
        color::Reset();

        return false;
    }

    // 循环接收响应帧
    auto startTime = std::chrono::steady_clock::now();

    while (true)
    {
        // 计算剩余超时
        int remaining = timeoutMs;

        if (timeoutMs > 0)
        {
            // 计算剩余等待时间
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
            remaining = timeoutMs - static_cast<int>(elapsed);

            // 超时
            if (remaining <= 0)
            {
                color::Red();
                std::wcerr << L"[!] Command timeout\n";
                color::Reset();
                return false;
            }
        }

        // 用来接收一个帧
        PipeServer::Frame frame;

        // 管道断开
        if (!server.RecvFrame(frame, remaining))
        {
            color::Red();
            std::wcerr << L"[!] Connection lost\n";
            color::Reset();

            return false;
        }

        // 根据帧类型处理
        switch (frame.type)
        {
            // Lua 输出：直接写入控制台
            case protocol::MSG_LOG:
            {
                // 直接使用安全输出 UTF-8 到控制台
                std::string text(frame.payload.begin(), frame.payload.end());
                SafePrintUtf8(text);
                break;
            }

            // 命令执行成功
            case protocol::MSG_OK: 
            {
                return true;
            }

            // 命令执行失败
            case protocol::MSG_ERROR:
            {
                // DLL 返回的错误消息是 UTF-8 编码 用 SafePrintUtf8 输出
                std::string text(frame.payload.begin(), frame.payload.end());

                color::Red();
                SafePrintUtf8("[Error] " + text + "\n", true);
                color::Reset();

                return false;
            }

            // DLL 主动断开
            case protocol::MSG_EXIT:
            {
                color::Yellow();
                std::wcerr << L"[*] DLL requested disconnect\n";
                color::Reset();

                return false;
            }

            // 忽略未知帧类型
            default: break;
        }
    }
}

// ============================================================
// REPL 辅助：打印提示符
// ============================================================
// 标记当前是否正停留在提示符行（光标在 "ilune >> " 之后）
// 供后台读取线程的日志回调判断是否需要先换行/恢复提示符
static std::atomic<bool> g_promptActive{ false };

// 下一个提示符前是否需要补一个换行（与上一条输出/回车隔开一行）
// 首次提示符与空输入回车不置位 其余命令执行与异步输出后都会置位
static std::atomic<bool> g_promptSeparate{ false };

static void PrintPrompt()
{
    // 除首次提示符与空输入回车外 每个提示符前补一个换行
    // 保证与上一条内容（或玩家的回车）之间恰好空一行
    if (g_promptSeparate.exchange(false))
    {
        std::wcout << L"\n";
    }

    g_promptActive = true;
    color::Yellow();
    std::wcout << L"ilune >> ";
    color::Reset();

    // 刷新缓冲区
    std::wcout.flush();
}


// ============================================================
// 主入口
// ============================================================
int wmain(int argc, wchar_t* argv[])
{
    // 解析命令行参数
    Args args = ParseArgs(argc, argv);

    // 命令行参数是否有效
    if (!args.valid)
    {
        PrintHelp();
        return 1;
    }

    // 打印横幅
    PrintBanner();

    // ========================================================
    // 定位目标进程
    // ========================================================
    color::Gray();
    std::wcout << L"[*] Locating target process...\n";
    color::Reset();

    //查找目标进程pid
    DWORD pid = LocateTarget(args);
    //没找到目标进程
    if (pid == 0) return 1;

    std::wstring procName = Injector::GetProcessName(pid);

    color::Green();
    std::wcout << L"[+] Target: " << procName << L" (PID: " << pid << L")\n";
    color::Reset();

    // ========================================================
    // 定位 DLL
    // ========================================================
    color::Gray();
    std::wcout << L"[*] Locating Il2CppLua.dll...\n";
    color::Reset();

    // 查找要注入的 DLL 路径
    std::wstring dllPath = LocateDll(args.dllPath);
    // 没找到 DLL
    if (dllPath.empty()) return 1;

    color::Green();
    std::wcout << L"[+] DLL: " << dllPath << L"\n";
    color::Reset();

    // ========================================================
    // 创建管道服务器
    // ========================================================
    // 管道名称格式：\\.\pipe\Il2CppLua_<PID>
    wchar_t pipeName[128];
    swprintf_s(pipeName, 128, L"%s%lu", protocol::PIPE_PREFIX, pid);

    color::Gray();
    std::wcout << L"[*] Creating pipe server...\n";
    color::Reset();

    // 保存全局指针（Ctrl+C 用）
    PipeServer server;
    g_pServer = &server;

    // 创建失败
    if (!server.Start(pipeName))
    {
        color::Red();
        std::wcerr << L"[!] Failed to create pipe server\n";
        color::Reset();

        return 1;
    }

    // 注册日志回调: DLL 发来的 MSG_LOG（Lua print / hook 回调输出）
    // 由 PipeServer 的后台读取线程实时打印 无需等待用户输入命令
    server.SetLogCallback([](const char* text) {
        if (text == nullptr) return;

        // 光标还停留在提示符行时 先换行 让异步输出独占一行
        // 避免输出直接粘连在 "ilune >> " 后面
        if (g_promptActive)
        {
            std::wcout << L"\n";
        }

        // 统一输出：直接打印 末尾没有换行时补一个 保证内容独占完整行
        SafePrintUtf8(text);

        // 文本末尾无换行时补一个 只收尾不负责与提示符分隔
        size_t len = strlen(text);
        if (len == 0 || text[len - 1] != '\n')
        {
            std::wcout << L"\n";
        }

        // 异步输出后恢复提示符 保证光标回到可输入状态
        // 与下一个提示符的分隔由 PrintPrompt 统一补换行
        if (g_promptActive)
        {
            g_promptSeparate = true;
            PrintPrompt();
        }

        std::wcout.flush();
    });

    // ========================================================
    // 注入 DLL
    // ========================================================
    color::Gray();
    std::wcout << L"[*] Injecting DLL...\n";
    color::Reset();

    // 注入失败
    if (!Injector::Inject(pid, dllPath, pipeName))
    {
        color::Red();
        std::wcerr << L"[!] Injection failed\n";
        color::Reset();

        // 确保清理残错数据
        server.Stop();

        return 1;
    }

    color::Green();
    std::wcout << L"[+] DLL injected\n";
    color::Reset();

    // ========================================================
    // 等待 DLL 连接
    // ========================================================
    color::Gray();
    std::wcout << L"[*] Waiting for DLL to connect...\n";
    color::Reset();

    // 连接超时 管道创立后 HANDSHAKE_TIMEOUT 毫秒未收到 DLL 连接
    if (!server.WaitForClient(protocol::HANDSHAKE_TIMEOUT))
    {
        color::Red();
        std::wcerr << L"[!] DLL did not connect (timeout " << protocol::HANDSHAKE_TIMEOUT / 1000 << L"s)\n";
        color::Reset();
        server.Stop();

        return 1;
    }

    color::Green();
    std::wcout << L"[+] DLL connected\n";
    color::Reset();

    // ========================================================
    // 握手：等待 HELLO + READY
    // ========================================================

    color::Gray();
    std::wcout << L"[*] Handshake...\n";
    color::Reset();

    // 等待 HELLO 帧
    PipeServer::Frame frame;

    // 握手失败 未收到Hello
    if (!server.WaitForFrame(protocol::MSG_HELLO, frame, protocol::DLLSAYHELLO_TIMEOUT))
    {
        color::Red();
        std::wcerr << L"[!] Handshake failed: no HELLO\n";
        color::Reset();
        server.Stop();

        return 1;
    }

    // 检查 HELLO 内容 校验版本
    std::string hello(frame.payload.begin(), frame.payload.end());

    color::Green();
    SafePrintUtf8("[+] Handshake: " + hello + "\n");
    color::Reset();

    // 收到 Hello 帧 共享内存可释放
    Injector::CloseSharedMemory();

    // 等待 READY 帧
    if (!server.WaitForFrame(protocol::MSG_READY, frame, protocol::DLLSAYREADY_TIMEOUT))
    {
        color::Red();
        std::wcerr << L"[!] Handshake failed: no READY (timeout)\n";
        color::Reset();

        // 检查是否收到了 ERROR 帧
        if (frame.type == protocol::MSG_ERROR)
        {
            std::string err(frame.payload.begin(), frame.payload.end());
            SafePrintUtf8("    DLL error: " + err + "\n", true);
        }

        server.Stop();

        return 1;
    }

    // 显示 READY 状态
    std::string ready(frame.payload.begin(), frame.payload.end());

    color::Green();
    SafePrintUtf8("[+] Ready: " + ready + "\n");
    color::Reset();

    // ========================================================
    // 执行启动脚本（如果指定了 -l 参数）
    // ========================================================
    if (!args.luaScript.empty())
    {
        color::Gray();
        std::wcout << L"[*] Executing startup script: " << args.luaScript << L"\n";
        color::Reset();

        // 读取脚本文件内容
        std::string scriptContent = ReadFileContent(args.luaScript);

        if (scriptContent.empty())
        {
            color::Red();
            std::wcerr << L"[!] Cannot read script file\n";
            color::Reset();
        }
        else
        {
            // 发送脚本内容作为 Lua 命令执行
            ExecuteCommand(server, scriptContent, protocol::LUAFILE_TIMEOUT);
        }

        std::wcout << L"\n";
    }

    // ========================================================
    // 注册 Ctrl+C 处理
    // ========================================================
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // ========================================================
    // 交互式 REPL 循环
    // ========================================================
    color::Cyan();
    std::wcout << L"Enter Lua code ('exit' to quit)\n\n";
    color::Reset();

    std::wcout << L"Welcome!\n\n";
    while (true)
    {
        // 打印提示符
        PrintPrompt();

        // 读取用户输入
        // 使用 ReadConsoleW 直接读取 绕过 std::wcin 的编码问题
        std::string input;

        if (!SafeReadUtf8(input))
        {
            // EOF (Ctrl+Z)
            std::wcout << L"\n";
            break;
        }

        // 用户已按回车 光标离开提示符行 后续异步输出不再换行/恢复提示符
        g_promptActive = false;

        // 跳过空输入
        if (input.empty() || input.find_first_not_of(" \t\r\n") == std::string::npos) continue;

        // 检查退出命令
        if (input == "exit" || input == "quit")
        {
            color::Gray();
            std::wcout << L"\n[*] Sending exit signal...\n";
            color::Reset();

            server.SendFrame(protocol::MSG_EXIT, nullptr, 0);

            break;
        }

        // 自动回显：判断是语句还是表达式
        std::string codeToSend;

        // 语句：原样发送 有返回值自然输出 无返回值（void）不输出
        // 表达式：包装为 return <expr>
        // void 函数返回 0 个值 PrintReturnValues 不会输出任何内容
        // 有返回值时 通过 __tostring 元方法自动格式化输出
        if (IsStatement(input)) codeToSend = input;
        else codeToSend = "return " + input;

        // 执行命令
        if (!ExecuteCommand(server, codeToSend))
        {
            // 执行失败或连接断开 检查是否是连接断开 如果只是执行错误 继续 REPL
            if (!server.IsConnected())
            {
                color::Red();
                std::wcerr << L"\n[!] Connection lost. DLL may have unloaded.\n";
                color::Reset();

                break;
            }
        }

        // 与下一个提示符隔开一行（由 PrintPrompt 统一补换行）
        g_promptSeparate = true;
    }

    // ========================================================
    // 清理
    // ========================================================
    color::Gray();
    std::wcout << L"[*] Shutting down...\n";
    color::Reset();

    server.Stop();

    color::Green();
    std::wcout << L"[+] Done\n\n";
    color::Reset();

    std::wcout << L"Thanks for using ilune!\n\n";

    // 保底清除共享内存
    Injector::CloseSharedMemory();

    return 0;
}
