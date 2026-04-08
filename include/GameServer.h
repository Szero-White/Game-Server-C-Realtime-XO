#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct PlayerSession;

struct ClientConnection {
    SOCKET socket = INVALID_SOCKET;
    std::mutex sendMutex;
    std::atomic<bool> connected{true};
    std::string username;
    std::weak_ptr<PlayerSession> session;
};

struct PlayerSession {
    std::string username;
    std::string token;
    std::weak_ptr<ClientConnection> connection;
    bool online = false;
    int roomId = -1;
    char symbol = ' ';
    std::chrono::steady_clock::time_point reconnectDeadline;
};

struct RoomState {
    int id = 0;
    std::string name;
    std::string playerX;
    std::string playerO;
    std::string turn;
    bool started = false;
    bool finished = false;
    std::vector<char> board = std::vector<char>(9, '.');
};

class GameServer {
public:
    explicit GameServer(unsigned short port);
    ~GameServer();

    bool start();
    void stop();

private:
    void acceptLoop();
    void maintenanceLoop();
    void clientLoop(SOCKET clientSocket);

    void handleDisconnect(const std::shared_ptr<ClientConnection>& connection);

    void onLogin(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args);
    void onReconnect(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args);
    void onCreateRoom(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args);
    void onJoinRoom(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args);
    void onListRooms(const std::shared_ptr<ClientConnection>& connection);
    void onMove(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args);
    void onLeave(const std::shared_ptr<ClientConnection>& connection);

    static bool recvLine(SOCKET socket, std::string& out);
    static std::vector<std::string> split(const std::string& input);
    static bool isValidUsername(const std::string& username);

    bool sendLine(const std::shared_ptr<ClientConnection>& connection, const std::string& line);
    bool sendToUser(const std::string& username, const std::string& line);

    std::string boardString(const RoomState& room) const;
    bool checkWin(const RoomState& room, char symbol) const;
    bool checkDraw(const RoomState& room) const;
    std::string makeToken() const;

private:
    unsigned short port_;
    SOCKET listenSocket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::thread maintenanceThread_;

    mutable std::mutex stateMutex_;
    std::unordered_map<std::string, std::shared_ptr<PlayerSession>> sessions_;
    std::unordered_map<int, RoomState> rooms_;
    int nextRoomId_ = 1;
};
