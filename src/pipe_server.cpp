/**
 * ============================================================
 * pipe_server.cpp — EXE 端命名管道服务器实现
 * ============================================================
 * 本文件实现 pipe_server.h 中声明的 PipeServer 类
 *
 * 模块组成
 * 
 * ·生命周期管理（Start / Stop / 析构）
 * ·客户端连接等待（WaitForClient）
 * ·帧发送（SendFrame）
 * ·帧接收（RecvFrame — 基于 PeekNamedPipe 轮询）
 * ·等待特定帧类型（WaitForFrame）
 *
 * 线程模型（单线程）
 * 
 * ·主线程负责所有操作：创建管道、等待连接、发送命令、接收响应
 * ·不使用后台读取线程 避免 ReadFile 阻塞管道句柄导致 WriteFile 死锁
 *
 * 超时实现
 * 
 * ·RecvFrame 使用 PeekNamedPipe 检测管道中是否有数据可读
 * ·如果没有数据 Sleep(10) 后重试 累计等待时间达到 timeoutMs 则返回 false
 * ·如果有数据（至少 HEADER_SIZE 字节） 调用 ReadFrame 读取完整帧
 * ·ReadFrame 内部的 ReadFile 会立即返回（因为数据已就绪）
 * ============================================================
 */

#include "pipe_server.h"


// ============================================================
// 连接工作线程上下文
// ============================================================
// 用于 WaitForClient 中将阻塞的 ConnectNamedPipe 放到工作线程执行
struct ConnectContext
{
    HANDLE pipe;       // 管道句柄
    bool   success;    // 连接结果
};


// ============================================================
// 连接工作线程函数
// ============================================================
// 在独立线程中调用 ConnectNamedPipe（阻塞等待客户端连接）
// 这个线程只用于连接阶段 连接完成后即退出 不影响后续 I/O
static DWORD WINAPI ConnectWorkerProc(LPVOID lpParam)
{
    // 获取上下文
    ConnectContext* ctx = static_cast<ConnectContext*>(lpParam);

    // 调用 ConnectNamedPipe 等待客户端连接
    // 此函数会阻塞 直到有客户端连接或出错
    BOOL ok = ConnectNamedPipe(ctx->pipe, nullptr);

    if (!ok)
    {
        // ConnectNamedPipe 返回 FALSE
        // 检查是否是因为客户端在调用前已连接（这也是成功情况）
        DWORD err = GetLastError();
        ctx->success = (err == ERROR_PIPE_CONNECTED);
    }
    else
    {
        // ConnectNamedPipe 返回 TRUE 客户端已连接
        ctx->success = true;
    }

    return 0;
}


// ============================================================
// 析构函数
// ============================================================
PipeServer::~PipeServer()
{
    // 确保资源已释放
    Stop();
}


// ============================================================
// 创建命名管道服务器
// ============================================================
bool PipeServer::Start(const wchar_t* pipeName)
{
    // 如果已有管道打开 先关闭
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    // 创建命名管道
    // PIPE_ACCESS_DUPLEX: 双向管道（可读可写）
    m_pipe = CreateNamedPipeW(
        pipeName,                   // 管道名称
        PIPE_ACCESS_DUPLEX,         // 双向访问模式
        PIPE_TYPE_BYTE |            // 字节模式传输
        PIPE_READMODE_BYTE |        // 字节模式读取
        PIPE_WAIT |                 // 阻塞模式
        PIPE_REJECT_REMOTE_CLIENTS, // 拒绝远程连接（安全）
        1,                          // 最大实例数（单实例）
        65536,                      // 输出缓冲区（64KB EXE→DLL 方向）
        65536,                      // 输入缓冲区（64KB DLL→EXE 方向）
        0,                          // 默认超时
        nullptr);                   // 默认安全属性

    // 检查创建结果
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    // 重置停止标志
    m_stopFlag = false;

    return true;
}


// ============================================================
// 停止服务器
// ============================================================
void PipeServer::Stop()
{
    // 设置停止标志
    m_stopFlag = true;

    // 关闭管道句柄
    // 关闭句柄会使任何阻塞的管道操作立即失败返回
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    // 重置状态
    m_connected = false;
}


// ============================================================
// 等待 DLL 客户端连接
// ============================================================
bool PipeServer::WaitForClient(int timeoutMs)
{
    // 前置检查
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    // 创建连接上下文
    ConnectContext ctx{};
    ctx.pipe = m_pipe;
    ctx.success = false;

    // 在工作线程中调用 ConnectNamedPipe
    // ConnectNamedPipe 是阻塞的 需要放在单独线程中
    // 主线程通过 WaitForSingleObject 实现超时控制
    // 注意：这个工作线程在连接完成后立即退出 不会影响后续管道 I/O
    HANDLE hConnectThread = CreateThread(
        nullptr,                // 默认安全属性
        0,                      // 默认栈大小
        ConnectWorkerProc,      // 线程函数
        &ctx,                   // 上下文参数
        0,                      // 立即运行
        nullptr);               // 不需要线程 ID

    // 创建线程失败
    if (hConnectThread == nullptr) return false;

    // 等待连接完成或超时
    DWORD waitResult = WaitForSingleObject(hConnectThread, timeoutMs);

    if (waitResult == WAIT_TIMEOUT)
    {
        // 超时：工作线程仍在阻塞于 ConnectNamedPipe
        // 关闭管道句柄强制 ConnectNamedPipe 返回
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;

        // 等待工作线程退出（此时 ConnectNamedPipe 会失败返回）
        WaitForSingleObject(hConnectThread, protocol::THREADEXIT_TIMEOUT);
        // 关闭工作进程句柄
        CloseHandle(hConnectThread);

        return false;
    }

    // 线程已完成（成功或失败）
    CloseHandle(hConnectThread);

    // 检查连接结果
    if (!ctx.success) return false;

    // 连接成功
    // 不需要启动读取线程 直接标记为已连接
    // 后续 SendFrame 和 RecvFrame 都在主线程中同步执行
    m_connected = true;
    m_stopFlag  = false;

    return true;
}


// ============================================================
// 帧发送
// ============================================================
bool PipeServer::SendFrame(uint8_t type, const void* data, uint32_t len)
{
    // 前置检查
    if (!m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;

    // 加锁保护写操作
    // 虽然当前只有主线程调用 但 Ctrl+C 处理函数可能从另一个线程调用 SendFrame
    std::lock_guard<std::mutex> lock(m_writeMutex);

    // 调用协议层写入帧
    return protocol::WriteFrame(m_pipe, type, data, len);
}


// ============================================================
// 帧接收（基于 PeekNamedPipe 轮询 带超时）
// ============================================================
// 这是修复管道死锁的关键函数
//
// 旧实现使用后台读取线程持续 ReadFile 导致管道句柄被阻塞的 ReadFile 占用 
// 主线程的 WriteFile 无法执行（Windows 命名管道在阻塞模式下序列化 I/O）
//
// 新实现使用 PeekNamedPipe 非阻塞检测数据可用性
// 
// ·PeekNamedPipe 查询管道中有多少字节可读（不会阻塞）
// ·如果可读字节数 >= HEADER_SIZE 调用 ReadFrame 读取完整帧
// ·ReadFrame 内部的 ReadFile 会立即返回（因为数据已就绪）
// ·如果没有数据 Sleep(10) 后重试 累计等待达到 timeoutMs 则超时
//
// 这样管道句柄不会被长时间占用 SendFrame 中的 WriteFile 可以随时执行
bool PipeServer::RecvFrame(Frame& out, int timeoutMs)
{
    // 前置检查
    if (!m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;

    // 记录开始时间（用于计算已等待时间）
    auto startTime = std::chrono::steady_clock::now();

    // 轮询循环
    while (!m_stopFlag)
    {
        // 使用 PeekNamedPipe 检测管道中的可读数据量
        // PeekNamedPipe 是非阻塞的 立即返回管道中可读的字节数
        // 它不会将数据从管道中移除 只是"偷看"
        DWORD bytesAvailable = 0;
        BOOL peekOk = PeekNamedPipe(
            m_pipe,             // 管道句柄
            nullptr,            // 不复制数据到缓冲区
            0,                  // 不读取任何字节
            nullptr,            // 不接收已读字节数
            &bytesAvailable,    // [out] 管道中可读的总字节数
            nullptr);           // 不接收剩余字节数

        // PeekNamedPipe 失败 — 管道已断开或出错
        if (!peekOk)
        {        
            m_connected = false;
            return false;
        }

        // 检查是否有足够数据读取帧头
        // 帧头为 5 字节（1 字节类型 + 4 字节长度）
        // 只有当可读字节数 >= HEADER_SIZE 时才尝试读取
        if (bytesAvailable >= protocol::HEADER_SIZE)
        {
            // 数据已就绪 调用 ReadFrame 读取完整帧
            // 由于 PeekNamedPipe 确认了至少有 HEADER_SIZE 字节可读 
            // ReadFrame 中读取帧头的 ReadFile 会立即返回
            // 读取帧头后 可能还需要读取负载,负载数据通常紧随帧头到达 
            // ReadFrame 内部会循环 ReadFile 直到读完全部负载
            uint8_t  type = 0;
            uint8_t* data = nullptr;
            uint32_t len  = 0;

            // ReadFrame 失败 — 管道已断开或数据损坏
            if (!protocol::ReadFrame(m_pipe, type, data, len))
            {
                m_connected = false;
                return false;
            }

            // 构造帧对象
            out.type = type;

            // 深拷贝负载数据
            // ReadFrame 使用内部静态缓冲区 下次调用会覆盖 必须立即复制
            if (len > 0 && data != nullptr)
            {
                out.payload.assign(data, data + len);
            }
            else
            {
                out.payload.clear();
            }

            // 成功读取一帧
            return true;
        }

        // ---- 没有足够数据 根据超时策略处理 ----
        if (timeoutMs == 0)
        {
            // 非阻塞模式：立即返回
            return false;
        }

        if (timeoutMs > 0)
        {
            // 带超时模式：检查是否已超时
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();

            // 超时
            if (elapsed >= timeoutMs) return false;

            // 还有剩余时间 短暂休眠后重试
            // 休眠时间取剩余时间和 10ms 的较小值
            int remaining = timeoutMs - static_cast<int>(elapsed);
            DWORD sleepMs = (remaining < 10) ? remaining : 10;
            Sleep(sleepMs);
        }
        else
        {
            // 无限等待模式（timeoutMs < 0）
            // 短暂休眠后重试 避免 CPU 空转
            Sleep(10);
        }
    }

    // 停止标志被设置
    m_connected = false;

    return false;
}


// ============================================================
// 等待特定类型的帧
// ============================================================
bool PipeServer::WaitForFrame(uint8_t expectedType, Frame& out, int timeoutMs)
{
    // 记录开始时间（用于计算剩余超时）
    auto startTime = std::chrono::steady_clock::now();

    // 循环接收帧 直到收到目标类型或超时
    while (!m_stopFlag)
    {
        // 计算剩余超时时间
        int remaining = timeoutMs;
        if (timeoutMs > 0)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
            remaining = timeoutMs - static_cast<int>(elapsed);

            if (remaining <= 0) return false;
        }
        else if (timeoutMs == 0)
        {
            // 非阻塞模式 直接返回
            return false;
        }
        else
        {
            // 无限等待
            remaining = -1;
        }

        // 接收一个帧
        if (!RecvFrame(out, remaining))
        {
            // RecvFrame 返回 false 可能是超时或管道断开
            // 检查连接状态以区分
            if (!m_connected) return false;
            // 超时 继续循环（如果有剩余时间）
            continue;
        }

        // 检查帧类型
        if (out.type == expectedType) return true;

        // 如果是错误帧 也返回（让调用方处理握手期间的错误）
        if (out.type == protocol::MSG_ERROR) return true;
    }

    // 停止标志被设置
    return false;
}
