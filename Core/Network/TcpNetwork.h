#pragma once

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <vector>
#include <functional>
#include "../../Shared/Protocol.h"

// Tell linker to pull in Winsock
#pragma comment(lib, "ws2_32.lib")

namespace gupt {
namespace core {
namespace network {

using MessageCallback = std::function<void(shared::MessageType, const std::vector<uint8_t>&)>;

class TcpServer {
public:
    TcpServer(uint16_t port);
    ~TcpServer();

    bool Start();
    void Stop();
    bool SendRaw(const std::vector<uint8_t>& data);
    void SetMessageCallback(MessageCallback cb) { m_Callback = cb; }
    bool IsClientConnected() { return m_ClientSocket != INVALID_SOCKET; }

private:
    void AcceptThread();
    void ReceiveThread();

    uint16_t m_Port;
    SOCKET m_ListenSocket = INVALID_SOCKET;
    SOCKET m_ClientSocket = INVALID_SOCKET;
    MessageCallback m_Callback;
    bool m_IsRunning = false;
};

class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    bool Connect(const std::string& host, uint16_t port);
    void Disconnect();
    bool SendRaw(const std::vector<uint8_t>& data);
    void SetMessageCallback(MessageCallback cb) { m_Callback = cb; }
    bool IsConnected() const { return m_IsConnected && m_Socket != INVALID_SOCKET; }

private:
    void ReceiveThread();

    SOCKET m_Socket = INVALID_SOCKET;
    MessageCallback m_Callback;
    bool m_IsConnected = false;
};

} // namespace network
} // namespace core
} // namespace gupt
