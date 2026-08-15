/**
 * ============================================================
 * pipe_server.h — EXE 端命名管道服务器声明
 * ============================================================
 * 本模块运行在 ilune.exe（注入器）中 负责
 *
 * ·创建命名管道服务器
 * ·等待 DLL 客户端连接
 * ·提供线程安全的帧发送接口
 * ·提供带超时的帧接收接口（基于响应帧队列 + 条件变量）
 *
 * 架构（后台读取线程 + 重叠 I/O）
 *
 *   ┌──────────────┐   SendFrame() → WriteFile(重叠)   ┐
 *   │  主线程 REPL  │                                  │
 *   │ RecvFrame()  │← 响应帧队列（OK/ERROR/EXIT）      │ 管道
 *   └──────────────┘                                  │
 *   ┌──────────────┐   ReaderLoop() → ReadFile(重叠)  ┘
 *   │ 后台读取线程  │→ MSG_LOG 实时打印（日志回调）
 *   └──────────────┘→ 其余帧入队供 RecvFrame 消费
 *
 * 为什么可以用后台读取线程
 *
 * ·Windows 命名管道在阻塞模式下 同一句柄上的 I/O 会被序列化
 *   读线程阻塞在 ReadFile 时 主线程的 WriteFile 也会被阻塞
 * ·因此句柄必须以 FILE_FLAG_OVERLAPPED 打开
 *   重叠模式下挂起的读不会阻塞同一句柄的写
 * ·后台线程持续读管道: Lua print / hook 回调输出无需等待用户输入命令即可实时显示
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
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>


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

    // 构造 / 析构
    PipeServer() = default;
    ~PipeServer();

    // 生命周期

    /**
     * 创建命名管道服务器
     *
     * @param pipeName 管道名称（如 L"\\\\.\\pipe\\Il2CppLua_5454"）
     * @return true 创建成功 false 失败
     *
     * 创建单个双工字节模式管道 并以 FILE_FLAG_OVERLAPPED 打开
     * 以便后台读取线程的挂起读不阻塞主线程的写
     */
    bool Start(const wchar_t* pipeName);

    /**
     * 停止服务器
     * 关闭管道、退出后台读取线程、清空响应队列
     */
    void Stop();

    /**
     * 等待 DLL 客户端连接
     *
     * @param timeoutMs 超时时间（毫秒） -1 表示无限等待
     * @return true 客户端已连接 false 超时或失败
     *
     * 连接成功后启动后台读取线程 持续读取 DLL 发来的帧
     */
    bool WaitForClient(int timeoutMs);

    // 状态查询
    bool IsConnected() const { return m_connected.load(); }

    /**
     * 设置日志回调
     *
     * DLL 发来的 MSG_LOG 帧（Lua print / hook 回调输出）由后台读取线程
     * 通过此回调实时输出 无需等待用户输入命令
     * 必须在 WaitForClient 之前设置
     */
    void SetLogCallback(std::function<void(const char*)> cb) { m_logCallback = std::move(cb); }

    /**
     * 发送一个帧到 DLL
     * @param type 消息类型
     * @param data 负载数据（可为 nullptr）
     * @param len  负载长度
     * @return true 发送成功 false 失败
     */
    bool SendFrame(uint8_t type, const void* data, uint32_t len);

    /**
     * 接收一个帧（阻塞或带超时）
     *
     * @param out       [out] 接收到的帧
     * @param timeoutMs 超时时间（毫秒）
     *                  -1 = 无限等待
     *                   0 = 非阻塞（立即返回）
     *                  >0 = 等待指定毫秒
     * @return true 收到帧 false 超时或管道断开
     *
     * 从后台读取线程填充的响应队列中取帧（OK / ERROR / EXIT）
     * 日志帧不进入队列 由日志回调直接输出
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
     * 循环接收帧 丢弃非匹配类型 直到收到目标类型或超时
     * 仅用于握手阶段（等待 HELLO / READY）
     * REPL 阶段应使用 RecvFrame 逐个处理
     */
    bool WaitForFrame(uint8_t expectedType, Frame& out, int timeoutMs);

private:
    // 后台读取线程主循环
    void ReaderLoop();

    // 成员变量
    HANDLE m_pipe = INVALID_HANDLE_VALUE;   // 管道句柄（FILE_FLAG_OVERLAPPED）

    std::atomic<bool> m_connected{ false }; // 连接状态（原子操作）
    std::atomic<bool> m_stopFlag{ false };  // 停止标志
    std::mutex m_writeMutex;                // 写操作互斥锁

    std::thread m_readerThread;                       // 后台读取线程
    std::function<void(const char*)> m_logCallback;   // 日志实时输出回调

    std::deque<Frame> m_frameQueue;         // 响应帧队列（OK/ERROR/EXIT）
    std::mutex m_queueMutex;                // 队列互斥锁
    std::condition_variable m_queueCv;      // 队列条件变量
};
