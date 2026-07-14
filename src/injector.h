/**
 * ============================================================
 * injector.h — DLL 注入器声明
 * ============================================================
 * 本模块运行在 ilune.exe（注入器）中负责
 * 
 * ·按进程名或 PID 查找目标进程
 * ·创建共享内存传递管道名称给 DLL
 * ·通过 CreateRemoteThread + LoadLibraryW 注入 DLL
 * ·如果 CreateRemoteThread 失败 回退到 NtCreateThreadEx
 *
 * 注入流程：
 * 
 * ·OpenProcess 打开目标进程
 * ·CreateFileMappingW 创建共享内存 写入管道名称
 * ·VirtualAllocEx 在目标进程分配内存 写入 DLL 路径
 * ·CreateRemoteThread 创建远程线程执行 LoadLibraryW
 * ·WaitForSingleObject 等待远程线程完成
 * ·检查 LoadLibraryW 返回值确认注入成功
 * ·清理资源（释放内存、关闭句柄）
 * ·等待数秒后关闭共享内存（确保 DLL 已读取）
 *
 * 仅针对 Windows x64
 * ============================================================
 */
#pragma once
#include "common.h"
#include <windows.h>

// ============================================================
// Injector — DLL 注入器（静态工具类）
// ============================================================
class Injector
{
public:
    /**
     * 按进程名查找进程 ID
     *
     * @param processName 进程名（如 L"Game.exe"） 不区分大小写
     * @return 进程 ID 0 表示未找到
     *
     * 使用 CreateToolhelp32Snapshot 枚举所有进程 
     * 逐个比较进程名（大小写不敏感）
     */
    static DWORD FindProcessByName(const std::wstring& processName);

    /**
     * 获取指定 PID 的进程名
     *
     * @param pid 进程 ID
     * @return 进程名（如 "Game.exe"） 失败返回空字符串
     */
    static std::wstring GetProcessName(DWORD pid);

    /**
     * 打开目标进程
     *
     * @param pid 进程 ID
     * @return 进程句柄 失败返回 nullptr
     *
     * 所需权限：
     * 
     * ·PROCESS_CREATE_THREAD  : 创建远程线程
     * ·PROCESS_VM_OPERATION   : 分配/释放内存
     * ·PROCESS_VM_WRITE       : 写入进程内存
     * ·PROCESS_QUERY_INFORMATION : 查询进程信息
     */
    static HANDLE OpenTargetProcess(DWORD pid);

    // ---- DLL 注入 ----

    /**
     * 将 DLL 注入目标进程
     *
     * @param pid      目标进程 ID
     * @param dllPath  DLL 文件完整路径（宽字符串）
     * @param pipeName 命名管道名称（写入共享内存供 DLL 读取）
     * @return true 注入成功 false 失败
     *
     * 流程：
     * 
     * ·创建共享内存（Il2CppLua_Config_<PID>） 写入管道名称
     * ·在目标进程分配内存并写入 DLL 路径
     * ·创建远程线程调用 LoadLibraryW 加载 DLL
     * ·等待远程线程完成 检查返回值
     * ·清理资源
     * ·等待 3 秒后关闭共享内存（确保 DLL 已读取管道名）
     */
    static bool Inject(DWORD pid, const std::wstring& dllPath, const wchar_t* pipeName);

private:
    // 禁止实例化（纯静态工具类）
    Injector() = delete;
    ~Injector() = delete;

    /**
     * 创建共享内存并写入管道名称
     *
     * @param pid 目标进程 ID（用于构造共享内存名）
     * @param pipeName 管道名称
     * @return 共享内存句柄 失败返回 nullptr
     */
    static HANDLE CreateSharedMemory(DWORD pid, const wchar_t* pipeName);

    /**
     * 使用 NtCreateThreadEx 注入 DLL（CreateRemoteThread 的后备方案）
     *
     * 某些情况下 CreateRemoteThread 会被安全软件拦截或因权限不足失败 
     * NtCreateThreadEx 是更底层的 API 可以绕过部分限制
     *
     * @param hProcess    目标进程句柄
     * @param loadLibrary LoadLibraryW 函数地址
     * @param param       传递给 LoadLibraryW 的参数（DLL 路径地址）
     * @return 远程线程句柄 失败返回 nullptr
     */
    static HANDLE InjectViaNtCreateThreadEx(HANDLE hProcess, void* loadLibrary, void* param);
};
