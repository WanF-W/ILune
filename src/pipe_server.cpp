/**
 * ============================================================
 * pipe_server.cpp — EXE 端命名管道服务器实现
 * ============================================================
 * 本文件实现 pipe_server.h 中声明的 PipeServer 类
 *
 * 模块组成
 *
 * ·重叠 I/O 辅助（ReadPipeOverlapped / WritePipeOverlapped）
 * ·生命周期管理（Start / Stop / 析构）
 * ·客户端连接等待（WaitForClient）
 * ·后台读取线程（ReaderLoop — 持续读管道并分发帧）
 * ·帧发送（SendFrame — 重叠 I/O 写）
 * ·帧接收（RecvFrame — 基于响应帧队列）
 * ·等待特定帧类型（WaitForFrame）
 *
 * 线程模型
 *
 * ·主线程: REPL / 命令收发（SendFrame + RecvFrame 队列）
 * ·后台读取线程: 持续 ReadFile 管道 日志帧实时打印 响应帧入队
 * ·句柄以 FILE_FLAG_OVERLAPPED 打开 挂起的读不阻塞主线程的写
 * ============================================================
 */

#include "pipe_server.h"


// ============================================================
// 重叠 I/O 辅助
// ============================================================

// 重叠 I/O 读取（阻塞等待完成）
// 返回 false 表示管道断开或读取失败
static bool ReadPipeOverlapped(HANDLE pipe, void* buf, DWORD len, DWORD& bytesRead)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    BOOL ok = ReadFile(pipe, buf, len, &bytesRead, &ov);
    if (!ok)
    {
        // 读取尚未完成 等待事件后取结果
        if (GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(pipe, &ov, &bytesRead, FALSE);
            }
            else
            {
                ok = FALSE;
            }
        }
        else
        {
            // 管道断开等错误
            ok = FALSE;
        }
    }

    CloseHandle(ov.hEvent);
    return ok != FALSE;
}

// 重叠 I/O 写入（阻塞等待完成）
// 返回 false 表示写入失败或未写满
static bool WritePipeOverlapped(HANDLE pipe, const void* buf, DWORD len)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(pipe, buf, len, &written, &ov);
    if (!ok)
    {
        // 写入尚未完成 等待事件后取结果
        if (GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(pipe, &ov, &written, FALSE);
            }
            else
            {
                ok = FALSE;
            }
        }
        else
        {
            ok = FALSE;
        }
    }

    CloseHandle(ov.hEvent);
    return ok != FALSE && written == len;
}


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
// 句柄以 FILE_FLAG_OVERLAPPED 打开 必须提供 OVERLAPPED 结构
static DWORD WINAPI ConnectWorkerProc(LPVOID lpParam)
{
    // 获取上下文
    ConnectContext* ctx = static_cast<ConnectContext*>(lpParam);

    // 创建事件并调用重叠 ConnectNamedPipe
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr)
    {
        ctx->success = false;
        return 0;
    }

    BOOL ok = ConnectNamedPipe(ctx->pipe, &ov);
    if (!ok)
    {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING)
        {
            // 等待连接完成
            if (WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0)
            {
                DWORD dummy = 0;
                ok = GetOverlappedResult(ctx->pipe, &ov, &dummy, FALSE);
            }
            else
            {
                ok = FALSE;
            }
        }
        else if (err == ERROR_PIPE_CONNECTED)
        {
            // 客户端在调用前已连接 这也是成功情况
            ok = TRUE;
        }
        else
        {
            ok = FALSE;
        }
    }

    CloseHandle(ov.hEvent);
    ctx->success = (ok != FALSE);
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
    // FILE_FLAG_OVERLAPPED: 重叠模式
    //   后台读取线程的挂起读不会阻塞主线程的写
    m_pipe = CreateNamedPipeW(
        pipeName,                       // 管道名称
        PIPE_ACCESS_DUPLEX |
        FILE_FLAG_OVERLAPPED,           // 双向 + 重叠模式
        PIPE_TYPE_BYTE |                // 字节模式传输
        PIPE_READMODE_BYTE |            // 字节模式读取
        PIPE_WAIT |                     // 阻塞模式
        PIPE_REJECT_REMOTE_CLIENTS,     // 拒绝远程连接（安全）
        1,                              // 最大实例数（单实例）
        65536,                          // 输出缓冲区（64KB EXE→DLL 方向）
        65536,                          // 输入缓冲区（64KB DLL→EXE 方向）
        0,                              // 默认超时
        nullptr);                       // 默认安全属性

    // 检查创建结果
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    // 重置停止标志
    m_stopFlag = false;

    return true;
}


// ============================================================
// 后台读取线程主循环
// ============================================================
// 持续从管道读取完整帧:
// ·MSG_LOG（Lua print / hook 回调输出）→ 通过日志回调实时打印
// ·其余帧（OK / ERROR / EXIT）→ 入队供 RecvFrame 消费
// 管道断开或 Stop 关闭句柄时退出
void PipeServer::ReaderLoop()
{
    while (!m_stopFlag)
    {
        // ---- 读取帧头: 1 字节类型 + 4 字节长度（小端）----
        uint8_t header[protocol::HEADER_SIZE]{};
        DWORD total = 0;
        bool ok = true;

        while (total < protocol::HEADER_SIZE)
        {
            DWORD chunk = 0;
            if (!ReadPipeOverlapped(m_pipe, header + total,
                    static_cast<DWORD>(protocol::HEADER_SIZE) - total, chunk) || chunk == 0)
            {
                ok = false;
                break;
            }
            total += chunk;
        }
        if (!ok) break;

        // 解析帧头
        uint8_t type = header[0];
        uint32_t len = static_cast<uint32_t>(header[1])
                     | (static_cast<uint32_t>(header[2]) << 8)
                     | (static_cast<uint32_t>(header[3]) << 16)
                     | (static_cast<uint32_t>(header[4]) << 24);

        // 长度校验：防止恶意/损坏的帧头导致缓冲区溢出
        if (len > protocol::MAX_PAYLOAD) break;

        // ---- 读取负载 ----
        std::vector<uint8_t> payload;
        if (len > 0)
        {
            payload.resize(len);
            total = 0;
            while (total < len)
            {
                DWORD chunk = 0;
                if (!ReadPipeOverlapped(m_pipe, payload.data() + total, len - total, chunk) || chunk == 0)
                {
                    ok = false;
                    break;
                }
                total += chunk;
            }
            if (!ok) break;
        }

        // ---- 分发帧 ----
        if (type == protocol::MSG_LOG)
        {
            // 日志帧: 实时输出 不进入响应队列
            if (m_logCallback)
            {
                std::string text(payload.begin(), payload.end());
                m_logCallback(text.c_str());
            }
        }
        else
        {
            // 响应帧: 入队供 RecvFrame 消费
            Frame f;
            f.type = type;
            f.payload = std::move(payload);
            {
                std::lock_guard<std::mutex> lk(m_queueMutex);
                m_frameQueue.push_back(std::move(f));
            }
            m_queueCv.notify_one();
        }
    }

    // 读取线程结束（管道断开或被 Stop 关闭）
    m_connected = false;
    m_queueCv.notify_all();
}


// ============================================================
// 停止服务器
// ============================================================
void PipeServer::Stop()
{
    // 设置停止标志
    m_stopFlag = true;

    // 加锁确保没有其他线程正在写入
    std::lock_guard<std::mutex> lock(m_writeMutex);

    // 关闭管道句柄
    // 关闭句柄会使后台读取线程的挂起读立即失败 线程随后退出
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    // 等待后台读取线程退出
    if (m_readerThread.joinable())
    {
        m_readerThread.join();
    }

    // 清空响应队列
    {
        std::lock_guard<std::mutex> qk(m_queueMutex);
        m_frameQueue.clear();
    }
    m_queueCv.notify_all();

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
    m_connected = true;
    m_stopFlag  = false;

    // 启动后台读取线程
    // 句柄已用 FILE_FLAG_OVERLAPPED 打开 挂起的读不会阻塞主线程的写
    // 该线程持续读取 DLL 发来的帧: 日志帧实时打印 响应帧入队
    m_readerThread = std::thread([this] { ReaderLoop(); });

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

    // 构造帧头: 1 字节类型 + 4 字节长度（小端）
    uint8_t header[protocol::HEADER_SIZE]{};
    header[0] = type;
    header[1] = static_cast<uint8_t>(len & 0xFF);
    header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>((len >> 16) & 0xFF);
    header[4] = static_cast<uint8_t>((len >> 24) & 0xFF);

    // 重叠 I/O 写（后台读取线程的挂起读不会阻塞本写入）
    if (!WritePipeOverlapped(m_pipe, header, protocol::HEADER_SIZE)) return false;

    // 写负载
    if (len > 0 && data != nullptr)
    {
        if (!WritePipeOverlapped(m_pipe, data, len)) return false;
    }
    return true;
}


// ============================================================
// 帧接收（基于响应帧队列 带超时）
// ============================================================
// 后台读取线程把 OK / ERROR / EXIT 等响应帧放入队列
// 本函数在队列上等待（condition_variable + 超时）并取出一个帧
// 日志帧不进入队列 由日志回调直接输出
bool PipeServer::RecvFrame(Frame& out, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_queueMutex);

    // 等待条件: 队列非空 或 管道断开/停止
    auto ready = [this] { return !m_frameQueue.empty() || !m_connected || m_stopFlag; };

    if (timeoutMs < 0)
    {
        // 无限等待
        m_queueCv.wait(lk, ready);
    }
    else if (timeoutMs == 0)
    {
        // 非阻塞模式：立即返回
        if (!ready()) return false;
    }
    else
    {
        // 带超时模式
        m_queueCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), ready);
    }

    // 队列为空: 超时或管道断开
    if (m_frameQueue.empty()) return false;

    // 取出一个帧
    out = std::move(m_frameQueue.front());
    m_frameQueue.pop_front();
    return true;
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
