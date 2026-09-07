/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include "MCP.hpp"

#include <Core/Logging/Logger.hpp>

#include <Core/Threading/AtomicFlag.hpp>
#include <Core/Threading/AtomicVar.hpp>
#include <Core/Threading/Mutex.hpp>
#include <Core/Threading/TaskThread.hpp>

#include <Core/Memory/UniquePtr.hpp>

namespace Hyperion {
namespace MCP {

class MCPServer;

struct MCPLogLine
{
    uint64 timestampMs;
    int level; // LogLevel
    ANSIString channel;
    String text;
};

class MCPServerThread final : public TaskThread
{
public:
    explicit MCPServerThread(MCPServer* server)
        : TaskThread(NAME("MCPServerThread")),
          m_server(server)
    {
    }

    ~MCPServerThread() override = default;

private:
    MCPServer* m_server;
};

class MCPServer final
{
public:
    static MCPServer& GetInstance();

    MCPServer() = default;
    ~MCPServer() = default;

    MCPServer(const MCPServer& other) = delete;
    MCPServer& operator=(const MCPServer& other) = delete;

    bool AutoStart();

    bool Start(uint16 port);
    void Stop();

    bool IsRunning() const
    {
        return m_running.Load();
    }

    Array<MCPLogLine> GetLogLines(uint32 maxLines, int minLevel) const;

private:
    void ServerThreadProc(uint16 port);
    void HandleConnection(int clientSocket);
    bool HandleHttpRequest(int clientSocket, const ANSIString& body);

    void InstallLogRedirect();
    void RemoveLogRedirect();

    static bool LogRedirectProc(void* context, const LogChannel& channel, const LogMessage& message);

    UniquePtr<MCPServerThread> m_thread;
    AtomicFlag m_running;
    AtomicVar<intptr_t> m_listenSocket { -1 };

    int m_logRedirectId = -1;

    mutable Mutex m_logMutex;
    Array<MCPLogLine> m_logLines;
};

} // namespace MCP
} // namespace Hyperion
