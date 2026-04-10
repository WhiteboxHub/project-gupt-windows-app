#include "TcpNetwork.h"
#include <iostream>
#include <thread>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace gupt {
namespace core {
namespace network {

// --- TcpServer Implementation ---

TcpServer::TcpServer(uint16_t port) : m_Port(port) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

TcpServer::~TcpServer() {
    Stop();
    WSACleanup();
}

bool TcpServer::Start() {
    m_ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_ListenSocket == INVALID_SOCKET) return false;

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_Port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_ListenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(m_ListenSocket);
        return false;
    }

    if (listen(m_ListenSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(m_ListenSocket);
        return false;
    }

    m_IsRunning = true;
    std::thread(&TcpServer::AcceptThread, this).detach();
    return true;
}

void TcpServer::Stop() {
    m_IsRunning = false;
    if (m_ListenSocket != INVALID_SOCKET) {
        closesocket(m_ListenSocket);
        m_ListenSocket = INVALID_SOCKET;
    }
    if (m_ClientSocket != INVALID_SOCKET) {
        closesocket(m_ClientSocket);
        m_ClientSocket = INVALID_SOCKET;
    }
}

bool TcpServer::SendRaw(const std::vector<uint8_t>& data) {
    if (m_ClientSocket == INVALID_SOCKET) return false;
    
    int totalSent = 0;
    int dataSize = static_cast<int>(data.size());
    const char* ptr = reinterpret_cast<const char*>(data.data());

    while (totalSent < dataSize) {
        int bytesSent = send(m_ClientSocket, ptr + totalSent, dataSize - totalSent, 0);
        if (bytesSent == SOCKET_ERROR) return false;
        totalSent += bytesSent;
    }
    return true;
}

void TcpServer::AcceptThread() {
    while (m_IsRunning) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);
        SOCKET clientSocket = accept(m_ListenSocket, (sockaddr*)&clientAddr, &clientAddrSize);
        
        if (clientSocket != INVALID_SOCKET) {
            int flag = 1;
            setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
            
            if (m_ClientSocket != INVALID_SOCKET) {
                closesocket(m_ClientSocket);
            }
            m_ClientSocket = clientSocket;
            std::thread(&TcpServer::ReceiveThread, this).detach();
        }
    }
}

void TcpServer::ReceiveThread() {
    std::vector<uint8_t> buffer(65536); // Use a larger chunk size
    std::vector<uint8_t> messageBuffer;
    size_t readPos = 0;

    while (m_IsRunning && m_ClientSocket != INVALID_SOCKET) {
        int bytesReceived = recv(m_ClientSocket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        
        if (bytesReceived > 0) {
            messageBuffer.insert(messageBuffer.end(), buffer.begin(), buffer.begin() + bytesReceived);

            while (messageBuffer.size() - readPos >= sizeof(shared::MessageHeader)) {
                shared::MessageHeader* header = reinterpret_cast<shared::MessageHeader*>(messageBuffer.data() + readPos);
                uint32_t totalMessageSize = sizeof(shared::MessageHeader) + header->payloadSize;

                // Sanity check preventing OOM crashes on broken streams
                if (totalMessageSize > 150000000) {
                    closesocket(m_ClientSocket);
                    m_ClientSocket = INVALID_SOCKET;
                    break;
                }

                if (messageBuffer.size() - readPos >= totalMessageSize) {
                    // Extract strictly the payload, omitting the header bytes!
                    std::vector<uint8_t> msgData(messageBuffer.data() + readPos + sizeof(shared::MessageHeader), messageBuffer.data() + readPos + totalMessageSize);
                    
                    if (m_Callback) {
                        m_Callback(header->type, msgData);
                    }

                    readPos += totalMessageSize;
                } else {
                    // Not enough bytes yet, wait for next recv
                    break;
                }
            }

            if (readPos > messageBuffer.size() / 2) {
                messageBuffer.erase(messageBuffer.begin(), messageBuffer.begin() + readPos);
                readPos = 0;
            }
        } else if (bytesReceived == 0 || bytesReceived == SOCKET_ERROR) {
            closesocket(m_ClientSocket);
            m_ClientSocket = INVALID_SOCKET;
            if (m_Callback) {
                m_Callback(shared::MessageType::Disconnect, {});
            }
            break;
        }
    }
}

// --- TcpClient Implementation ---

TcpClient::TcpClient() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

TcpClient::~TcpClient() {
    Disconnect();
    WSACleanup();
}

bool TcpClient::Connect(const std::string& host, uint16_t port) {
    m_Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_Socket == INVALID_SOCKET) return false;

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);

    // Set non-blocking mode for timed connect (2 second timeout)
    u_long nonBlocking = 1;
    ioctlsocket(m_Socket, FIONBIO, &nonBlocking);

    connect(m_Socket, (sockaddr*)&serverAddr, sizeof(serverAddr));

    // Wait up to 2 seconds for the connection to succeed
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(m_Socket, &writeSet);
    timeval tv = { 2, 0 }; // 2 seconds timeout
    int sel = select(0, NULL, &writeSet, NULL, &tv);

    if (sel <= 0 || !FD_ISSET(m_Socket, &writeSet)) {
        // Timed out or error — close and return false
        closesocket(m_Socket);
        m_Socket = INVALID_SOCKET;
        return false;
    }

    // Verify connection actually succeeded (check SO_ERROR)
    int err = 0;
    int errLen = sizeof(err);
    getsockopt(m_Socket, SOL_SOCKET, SO_ERROR, (char*)&err, &errLen);
    if (err != 0) {
        closesocket(m_Socket);
        m_Socket = INVALID_SOCKET;
        return false;
    }

    // Restore blocking mode
    u_long blocking = 0;
    ioctlsocket(m_Socket, FIONBIO, &blocking);

    int flag = 1;
    setsockopt(m_Socket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    m_IsConnected = true;
    std::thread(&TcpClient::ReceiveThread, this).detach();
    return true;
}

void TcpClient::Disconnect() {
    m_IsConnected = false;
    if (m_Socket != INVALID_SOCKET) {
        closesocket(m_Socket);
        m_Socket = INVALID_SOCKET;
    }
}

bool TcpClient::SendRaw(const std::vector<uint8_t>& data) {
    if (m_Socket == INVALID_SOCKET || !m_IsConnected) return false;
    
    int totalSent = 0;
    int dataSize = static_cast<int>(data.size());
    const char* ptr = reinterpret_cast<const char*>(data.data());

    while (totalSent < dataSize) {
        int bytesSent = send(m_Socket, ptr + totalSent, dataSize - totalSent, 0);
        if (bytesSent == SOCKET_ERROR) return false;
        totalSent += bytesSent;
    }
    return true;
}

void TcpClient::ReceiveThread() {
    std::vector<uint8_t> buffer(65536);
    std::vector<uint8_t> messageBuffer;
    size_t readPos = 0;

    while (m_IsConnected && m_Socket != INVALID_SOCKET) {
        int bytesReceived = recv(m_Socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        
        if (bytesReceived > 0) {
            messageBuffer.insert(messageBuffer.end(), buffer.begin(), buffer.begin() + bytesReceived);

            while (messageBuffer.size() - readPos >= sizeof(shared::MessageHeader)) {
                shared::MessageHeader* header = reinterpret_cast<shared::MessageHeader*>(messageBuffer.data() + readPos);
                uint32_t totalMessageSize = sizeof(shared::MessageHeader) + header->payloadSize;

                if (totalMessageSize > 150000000) { // Increased tolerance to 150MB just in case
                    Disconnect();
                    break;
                }

                if (messageBuffer.size() - readPos >= totalMessageSize) {
                    // Extract strictly the payload, omitting the header bytes!
                    std::vector<uint8_t> msgData(messageBuffer.data() + readPos + sizeof(shared::MessageHeader), messageBuffer.data() + readPos + totalMessageSize);
                    
                    if (m_Callback) {
                        m_Callback(header->type, msgData);
                    }

                    readPos += totalMessageSize;
                } else {
                    break;
                }
            }
            
            if (readPos > messageBuffer.size() / 2) {
                messageBuffer.erase(messageBuffer.begin(), messageBuffer.begin() + readPos);
                readPos = 0;
            }
        } else if (bytesReceived == 0 || bytesReceived == SOCKET_ERROR) {
            Disconnect();
            if (m_Callback) {
                m_Callback(shared::MessageType::Disconnect, {});
            }
            break;
        }
    }
}

} // namespace network
} // namespace core
} // namespace gupt
