/**
 * ============================================================
 * injector.cpp — DLL 注入器实现
 * ============================================================
 * 本文件实现 injector.h 中声明的 Injector 类
 *
 * 模块组成
 * 
 * ·进程查找（FindProcessByName / GetProcessName）
 * ·进程打开（OpenTargetProcess）
 * ·共享内存创建（CreateSharedMemory）
 * ·DLL 注入（Inject）
 * ·NtCreateThreadEx 后备注入（InjectViaNtCreateThreadEx）
 *
 * 技术要点
 * 
 * ·进程枚举使用 Toolhelp32 API
 * ·DLL 注入使用 CreateRemoteThread + LoadLibraryW
 * ·NtCreateThreadEx 作为后备方案（更底层 绕过部分限制）
 * ·共享内存用于在注入器和 DLL 之间传递管道名称
 * ·所有资源（句柄、内存）都有严格的清理逻辑
 */

#include "injector.h"
#include "protocol.h"

// Windows API
#include <tlhelp32.h> // CreateToolhelp32Snapshot（进程枚举）
#include <cwchar>     // _wcsicmp（宽字符串不区分大小写比较）


// ============================================================
// NtCreateThreadEx 函数指针类型定义
// ============================================================
// NtCreateThreadEx 是 ntdll.dll 中的未文档化 API 
// 比 CreateRemoteThread 更底层 可以绕过部分安全限制,
// 其原型通过逆向工程获得 在不同 Windows 版本上保持稳定

// NtCreateThreadEx 函数指针类型
typedef NTSTATUS(NTAPI* pfnNtCreateThreadEx)(
    PHANDLE                hThread,           // [out] 线程句柄
    ACCESS_MASK            DesiredAccess,     // 请求的访问权限
    LPVOID                 ObjectAttributes,  // 对象属性（通常为 NULL）
    HANDLE                 ProcessHandle,     // 目标进程句柄
    LPTHREAD_START_ROUTINE lpStartAddress,    // 线程函数地址
    LPVOID                 lpParameter,       // 线程参数
    BOOL                   CreateSuspended,   // 是否创建后挂起
    ULONG                  StackZeroBits,     // 栈零填充位数
    ULONG                  SizeOfStackCommit, // 栈提交大小（0 = 默认）
    ULONG                  SizeOfStackReserve,// 栈保留大小（0 = 默认）
    LPVOID                 lpBytesBuffer);    // 属性缓冲区（通常为 NULL）


// ============================================================
// 按进程名查找进程 ID
// ============================================================
DWORD Injector::FindProcessByName(const std::wstring& processName)
{
    // 创建进程快照（包含所有进程的信息）
    // TH32CS_SNAPPROCESS 表示拍摄进程列表快照
    // 0表示所有进程
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    // 快照创建失败
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    // 初始化进程条目结构
    PROCESSENTRY32W pe32{};
    // 必须设置大小
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    // 查找结果
    DWORD resultPid = 0;

    // 遍历进程列表
    if (Process32FirstW(hSnapshot, &pe32))
    {
        // 遍历所有进程
        do
        {
            // 不区分大小写比较进程名
            // pe32.szExeFile 是可执行文件的文件名（不含路径）
            if (_wcsicmp(pe32.szExeFile, processName.c_str()) == 0)
            {
                // 找到匹配的进程
                resultPid = pe32.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }

    // 关闭快照句柄
    CloseHandle(hSnapshot);

    // 返回结果
    return resultPid;
}


// ============================================================
// 获取指定 PID 的进程名
// ============================================================
std::wstring Injector::GetProcessName(DWORD pid)
{
    // 创建进程快照
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    // 快照创建失败
    if (hSnapshot == INVALID_HANDLE_VALUE) return L"";

    // 初始化进程条目结构
    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    // 查找结果
    std::wstring result;

    // 遍历进程列表
    if (Process32FirstW(hSnapshot, &pe32))
    {
        do
        {
            if (pe32.th32ProcessID == pid)
            {
                // 找到目标进程
                result = pe32.szExeFile;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);

    return result;
}


// ============================================================
// 打开目标进程
// ============================================================
HANDLE Injector::OpenTargetProcess(DWORD pid)
{
    // 定义所需的访问权限
    // 这些权限是 DLL 注入所必需的
    DWORD accessRights =
        PROCESS_CREATE_THREAD |    // 创建远程线程
        PROCESS_VM_OPERATION |     // VirtualAllocEx / VirtualFreeEx
        PROCESS_VM_WRITE |         // WriteProcessMemory
        PROCESS_QUERY_INFORMATION; // 查询进程信息

    // 打开目标进程
    HANDLE hProcess = OpenProcess(
        accessRights,    // 请求的访问权限
        FALSE,           // 不继承句柄
        pid);            // 目标进程 ID

    return hProcess;     // 失败时返回 nullptr（INVALID_HANDLE_VALUE 的特殊情况）
}


// ============================================================
// 创建共享内存并写入管道名称
// ============================================================
HANDLE Injector::CreateSharedMemory(DWORD pid, const wchar_t* pipeName)
{
    // 构造共享内存名称
    // 格式：Il2CppLua_Config_<PID>
    // DLL 用 GetCurrentProcessId() 获取相同的 PID 来打开此共享内存
    wchar_t shmName[128];
    // swprintf_s(目标宽字符数组, 宽字符数量,格式化字符串必须加 L 前缀 如 L"Hello %s",...可变参数)
    swprintf_s(shmName, 128, L"%s%lu", protocol::SHARED_MEM_PREFIX, pid);

    // 创建共享内存
    // CreateFileMappingW 创建一个可被其他进程通过名称打开的共享内存
    // PAGE_READWRITE 表示可读写
    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,                           // 不关联文件（纯内存映射）
        nullptr,                                        // 默认安全属性
        PAGE_READWRITE,                                 // 读写权限
        0,                                              // 高 32 位大小（0 表示不超过 4GB）
        static_cast<DWORD>(protocol::SHARED_MEM_SIZE),  // 低 32 位大小（512 字节）
        shmName);                                       // 共享内存名称

    if (hMap == nullptr) return nullptr;                // 创建失败

    // 映射共享内存到本进程地址空间
    void* mapped = MapViewOfFile(
        hMap,           // 共享内存句柄
        FILE_MAP_WRITE, // 写入权限
        0, 0, 0);       // 从开头开始 映射全部

    // 映射失败 关闭句柄
    if (mapped == nullptr)
    {
        CloseHandle(hMap);
        return nullptr;
    }

    // 写入管道名称
    // 清零共享内存（确保没有残留数据）
    ZeroMemory(mapped, protocol::SHARED_MEM_SIZE);

    // 将管道名称拷贝到共享内存
    // 使用 wcsncpy_s 防止缓冲区溢出
    // 安全拷贝宽字符串方法 wcsncpy_s(目标缓冲区, 目标最多容纳的宽字符数, 源字符串, 截断策略)
    wcsncpy_s(static_cast<wchar_t*>(mapped), protocol::SHARED_MEM_SIZE / sizeof(wchar_t), pipeName, _TRUNCATE);

    // 解除映射（但保持共享内存句柄打开）
    // 共享内存只要至少有一个打开的句柄就保持有效
    // DLL 会通过 OpenFileMappingW 打开它
    UnmapViewOfFile(mapped);

    // 返回句柄 调用方负责后续关闭
    return hMap;
}


// ============================================================
// DLL 注入主函数
// ============================================================
bool Injector::Inject(DWORD pid, const std::wstring& dllPath, const wchar_t* pipeName)
{
    // 创建共享内存（传递管道名称给 DLL）
    HANDLE hSharedMem = CreateSharedMemory(pid, pipeName);

    // 共享内存创建失败
    if (hSharedMem == nullptr) return false;

    // 打开目标进程
    HANDLE hProcess = OpenTargetProcess(pid);
    if (hProcess == nullptr)
    {
        // 清理共享内存
        CloseHandle(hSharedMem);
        // 无法打开进程
        return false;
    }

    // 获取 DLL 绝对路径
    // LoadLibraryW 需要绝对路径（或系统搜索路径中的文件名）
    // 如果用户提供了相对路径 转换为绝对路径
    wchar_t fullPath[MAX_PATH];
    DWORD pathLen = GetFullPathNameW(
        dllPath.c_str(), // 输入路径
        MAX_PATH,        // 输出缓冲区大小
        fullPath,        // 输出缓冲区
        nullptr);        // 不需要文件名部分指针

    if (pathLen == 0 || pathLen >= MAX_PATH)
    {
        // 路径转换失败或路径过长
        CloseHandle(hProcess);
        CloseHandle(hSharedMem);
        return false;
    }

    // 验证 DLL 文件是否存在
    if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
    {
        // DLL 文件不存在
        CloseHandle(hProcess);
        CloseHandle(hSharedMem);
        return false;
    }

    // 计算所需内存大小（包括零终止符）
    size_t dllPathSize = (wcslen(fullPath) + 1) * sizeof(wchar_t);

    // 在目标进程中分配内存
    // VirtualAllocEx 在指定进程的地址空间中分配内存
    void* remoteMemory = VirtualAllocEx(
        hProcess,                 // 目标进程句柄
        nullptr,                  // 让系统选择分配地址
        dllPathSize,              // 分配大小（字节数）
        MEM_COMMIT | MEM_RESERVE, // 提交并保留内存
        PAGE_READWRITE);          // 读写权限

    if (remoteMemory == nullptr)
    {
        // 内存分配失败
        CloseHandle(hProcess);
        CloseHandle(hSharedMem);
        return false;
    }

    // 将 DLL 路径写入目标进程内存
    SIZE_T bytesWritten = 0;
    BOOL writeOk = WriteProcessMemory(
        hProcess,       // 目标进程句柄
        remoteMemory,   // 目标地址（目标进程中）
        fullPath,       // 源数据（本进程中的 DLL 路径）
        dllPathSize,    // 数据大小
        &bytesWritten); // 实际写入字节数

    if (!writeOk || bytesWritten != dllPathSize)
    {
        // 写入失败
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        CloseHandle(hSharedMem);
        return false;
    }

    // 获取 LoadLibraryW 函数地址
    // kernel32.dll 在所有进程中的加载地址相同（Windows 保证）
    // 因此可以在本进程中获取地址 直接用于目标进程
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32 == nullptr)
    {
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        CloseHandle(hSharedMem);
        return false;
    }

    // 获取 LoadLibraryW 的地址
    void* loadLibraryAddr = reinterpret_cast<void*>(GetProcAddress(hKernel32, "LoadLibraryW"));

    if (loadLibraryAddr == nullptr)
    {
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        CloseHandle(hSharedMem);
        return false;
    }

    // 创建远程线程执行 LoadLibraryW
    // CreateRemoteThread 在目标进程中创建一个新线程
    // 线程函数为 LoadLibraryW 参数为 DLL 路径地址

    HANDLE hThread = CreateRemoteThread(
        hProcess,                                                  // 目标进程句柄
        nullptr,                                                   // 默认安全属性
        0,                                                         // 默认栈大小
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr), // 线程函数
        remoteMemory,                                              // 传递给线程函数的参数（DLL 路径地址）
        0,                                                         // 立即执行
        nullptr);                                                  // 不需要线程 ID

    // ---- 如果 CreateRemoteThread 失败 尝试 NtCreateThreadEx ----
    if (hThread == nullptr)
    {
        hThread = InjectViaNtCreateThreadEx(hProcess, loadLibraryAddr, remoteMemory);
    }

    if (hThread == nullptr)
    {
        // 两种注入方式都失败
        VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        CloseHandle(hSharedMem);
        return false;
    }

    // 等待远程线程完成,即 DLL 执行 DllMain 方法
    // LoadLibraryW 完成后 远程线程结束
    // 设置 30 秒超时防止无限等待
    DWORD waitResult = WaitForSingleObject(hThread, protocol::DLLINIT_TIMEOUT);

    // 检查线程退出码（即 LoadLibraryW 的返回值）
    // 如果返回值非零 表示 DLL 加载成功
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    // 关闭线程句柄
    CloseHandle(hThread);

    // 清理资源
    // 释放目标进程中的内存（DLL 已加载 路径不再需要）
    VirtualFreeEx(hProcess, remoteMemory, 0, MEM_RELEASE);

    // 关闭进程句柄
    CloseHandle(hProcess);

    // 等待 DLL 读取共享内存
    // DLL 在其工作线程中会读取共享内存获取管道名称
    // 等待 3 秒确保 DLL 已完成读取
    Sleep(protocol::FOR_DLLREADMEM_TIME);

    // 关闭共享内存句柄
    // 关闭后共享内存将被销毁（如果没有其他句柄打开）
    CloseHandle(hSharedMem);

    // 检查注入是否成功（LoadLibraryW 返回值非零）
    return (waitResult == WAIT_OBJECT_0 && exitCode != 0);
}


// ============================================================
// 使用 NtCreateThreadEx 注入 DLL（后备方案）
// ============================================================
HANDLE Injector::InjectViaNtCreateThreadEx(HANDLE hProcess, void* loadLibrary, void* param)
{
    // 动态加载 ntdll.dll
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    // 无法获取 ntdll 句柄
    if (hNtdll == nullptr) return nullptr;

    // 获取 NtCreateThreadEx 函数地址
    auto pNtCreateThreadEx = reinterpret_cast<pfnNtCreateThreadEx>(GetProcAddress(hNtdll, "NtCreateThreadEx"));

    // 函数不存在
    if (pNtCreateThreadEx == nullptr) return nullptr;

    // 调用 NtCreateThreadEx 创建远程线程
    HANDLE hThread = nullptr;
    NTSTATUS status = pNtCreateThreadEx(
        &hThread,                                              // [out] 线程句柄
        THREAD_ALL_ACCESS,                                     // 完全访问权限
        nullptr,                                               // 默认对象属性
        hProcess,                                              // 目标进程句柄
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), // 线程函数
        param,                                                 // 参数（DLL 路径地址）
        FALSE,                                                 // 不挂起
        0,                                                     // 默认栈零填充
        0,                                                     // 默认栈提交大小
        0,                                                     // 默认栈保留大小
        nullptr);                                              // 无属性缓冲区

    // 检查返回状态
    // NTSTATUS >= 0 表示成功
    if (status < 0 || hThread == nullptr) return nullptr;

    // 成功 返回线程句柄
    return hThread;
}
