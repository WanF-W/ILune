/**
 * ============================================================
 * pipe_server.h — EXE 端命名管道服务器声明
 * ============================================================
 * 本模块运行在 ilune.exe（注入器）中 负责
 * 
 * ·创建命名管道服务器
 * ·等待 DLL 客户端连接
 * ·提供线程安全的帧发送接口
 * ·提供带超时的帧接收接口（基于 PeekNamedPipe 轮询）
 *
 * 架构（无后台读取线程）
 * 
 *   ┌──────────────────────────────────────────┐
 *   │              主线程 (REPL)                │
 *   │  SendFrame() → WriteFile(管道)           │
 *   │  RecvFrame()  ← PeekNamedPipe + ReadFile │
 *   └──────────────────────────────────────────┘
 *                            ↕ 管道
 *   ┌──────────────────────────────────────────┐
 *   │          DLL (PipeChannel)               │
 *   └──────────────────────────────────────────┘
 *
 * 为什么不用后台读取线程
 * 
 * ·Windows 命名管道在阻塞模式下 同一句柄上的 I/O 操作会被序列化
 * ·如果读取线程在 ReadFile 中阻塞等待数据 主线程的 WriteFile 也会被
 * ·阻塞 直到 ReadFile 完成 — 形成死锁
 * ·改用 PeekNamedPipe 轮询：只在有数据可读时才调用 ReadFile 
 * ·ReadFile 会立即返回（因为数据已就绪） 不会长时间占用管道句柄
 *
 * 仅针对 Windows x64
 * ============================================================
 */
#pragma once
#include "common.h"
#include "protocol.h"

// Windows API
#include <windows.h>

// 标准库
#include <atomic>


// ============================================================
// PipeServer — 命名管道服务器
// ============================================================
class PipeServer
{
public:
    // ========================================================
    // 帧结构
    // ========================================================
    // 从管道读取的一个完整帧
    struct Frame
    {
        uint8_t              type = 0;     // 消息类型（protocol::MessageType）
        std::vector<uint8_t> payload;      // 负载数据（可能为空）
    };

    // ---- 构造/析构 ----
    PipeServer() = default;
    ~PipeServer();

    // ---- 生命周期 ----

    /**
     * 创建命名管道服务器
     *
     * @param pipeName 管道名称（如 L"\\\\.\\pipe\\Il2CppLua_5454"）
     * @return true 创建成功 false 失败
     *
     * 创建一个双向字节模式的命名管道 等待客户端连接
     */
    bool Start(const wchar_t* pipeName);

    /**
     * 停止服务器
     * 关闭管道、重置状态
     */
    void Stop();

    /**
     * 等待 DLL 客户端连接
     *
     * @param timeoutMs 超时时间（毫秒） -1 表示无限等待
     * @return true 客户端已连接 false 超时或失败
     *
     * 使用工作线程调用 ConnectNamedPipe（阻塞） 
     * 主线程用 WaitForSingleObject 等待 实现超时控制
     * 连接成功后即可直接进行帧收发 无需启动额外线程
     */
    bool WaitForClient(int timeoutMs);

    // ---- 状态查询 ----

    // 是否有客户端已连接
    bool IsConnected() const { return m_connected.load(); }

    // ---- 帧发送（主线程 → DLL）----
    // 线程安全（内部加锁） 但实际只有主线程调用

    /**
     * 发送一个帧到 DLL
     * @param type 消息类型
     * @param data 负载数据（可为 nullptr）
     * @param len  负载长度
     * @return true 发送成功 false 失败
     */
    bool SendFrame(uint8_t type, const void* data, uint32_t len);

    // ---- 帧接收（DLL → 主线程）----
    // 直接从管道读取 使用 PeekNamedPipe 实现超时

    /**
     * 接收一个帧（阻塞或带超时）
     *
     * @param out       [out] 接收到的帧
     * @param timeoutMs 超时时间（毫秒）
     *                  -1 = 无限等待
     *                   0 = 非阻塞（立即返回）
     *                  >0 = 等待指定毫秒
     * @return true 接收到帧 false 超时或管道断开
     *
     * 实现方式：使用 PeekNamedPipe 轮询检测数据可用性
     * 只有当管道中有足够数据（至少 HEADER_SIZE 字节）时才调用 ReadFile
     * 这样 ReadFile 会立即返回 不会长时间占用管道句柄 
     * 避免与 SendFrame 中的 WriteFile 产生死锁
     */
    bool RecvFrame(Frame& out, int timeoutMs = -1);

    /**
     * 非阻塞接收一个帧
     * 等价于 RecvFrame(out, 0)
     */
    bool PollFrame(Frame& out) { return RecvFrame(out, 0); }

    /**
     * 等待特定类型的帧
     *
     * 循环接收帧 丢弃非匹配类型的帧 直到收到目标类型或超时
     * 仅用于握手阶段（等待 HELLO / READY） 
     * REPL 阶段应使用 RecvFrame 逐个处理
     *
     * @param expectedType 期望的帧类型
     * @param out          [out] 接收到的帧
     * @param timeoutMs    超时时间
     * @return true 收到目标帧 false 超时
     */
    bool WaitForFrame(uint8_t expectedType, Frame& out, int timeoutMs);

private:
    // 成员变量
    HANDLE m_pipe = INVALID_HANDLE_VALUE;   // 管道句柄

    std::atomic<bool> m_connected{ false }; // 连接状态（原子操作）
    std::atomic<bool> m_stopFlag{ false };  // 停止标志（用于 Stop）

    std::mutex m_writeMutex;                // 写操作互斥锁
};
