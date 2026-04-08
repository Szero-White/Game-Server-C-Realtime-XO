#include "GameServer.h"

#include "Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <random>
#include <sstream>

namespace {
constexpr int kReconnectSeconds = 30;

std::string safeRoomName(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return "Room";
    }

    std::ostringstream oss;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) {
            oss << ' ';
        }
        oss << args[i];
    }
    return oss.str();
}
} // namespace

GameServer::GameServer(unsigned short port) : port_(port) {}

GameServer::~GameServer() {
    stop();
}

bool GameServer::start() {
    if (running_.load()) {
        return true;
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Logger::instance().error("WSAStartup failed");
        return false;
    }

    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        Logger::instance().error("socket() failed");
        WSACleanup();
        return false;
    }

    sockaddr_in service{};
    service.sin_family = AF_INET;
    service.sin_addr.s_addr = INADDR_ANY;
    service.sin_port = htons(port_);

    if (bind(listenSocket_, reinterpret_cast<SOCKADDR*>(&service), sizeof(service)) == SOCKET_ERROR) {
        Logger::instance().error("bind() failed");
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        Logger::instance().error("listen() failed");
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&GameServer::acceptLoop, this);
    maintenanceThread_ = std::thread(&GameServer::maintenanceLoop, this);

    Logger::instance().info("Server started on port " + std::to_string(port_));
    return true;
}

void GameServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (maintenanceThread_.joinable()) {
        maintenanceThread_.join();
    }

    std::vector<std::shared_ptr<ClientConnection>> liveConnections;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (auto& entry : sessions_) {
            const auto& session = entry.second;
            if (auto connection = session->connection.lock()) {
                liveConnections.push_back(connection);
            }
            session->online = false;
        }
        sessions_.clear();
        rooms_.clear();
    }

    for (const auto& connection : liveConnections) {
        shutdown(connection->socket, SD_BOTH);
        closesocket(connection->socket);
    }

    WSACleanup();
}

void GameServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_in clientInfo{};
        int clientInfoLen = sizeof(clientInfo);
        SOCKET clientSocket = accept(listenSocket_, reinterpret_cast<SOCKADDR*>(&clientInfo), &clientInfoLen);
        if (clientSocket == INVALID_SOCKET) {
            if (running_.load()) {
                Logger::instance().warn("accept() returned invalid socket");
            }
            continue;
        }

        std::thread(&GameServer::clientLoop, this, clientSocket).detach();
    }
}

void GameServer::maintenanceLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::vector<std::pair<std::string, std::string>> messages;
        std::vector<std::string> logs;

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const auto now = std::chrono::steady_clock::now();

            for (auto& roomPair : rooms_) {
                RoomState& room = roomPair.second;
                if (!room.started || room.finished) {
                    continue;
                }

                auto sessionX = sessions_.find(room.playerX);
                auto sessionO = sessions_.find(room.playerO);
                if (sessionX == sessions_.end() || sessionO == sessions_.end()) {
                    continue;
                }

                bool xExpired = !sessionX->second->online && now > sessionX->second->reconnectDeadline;
                bool oExpired = !sessionO->second->online && now > sessionO->second->reconnectDeadline;

                if (!xExpired && !oExpired) {
                    continue;
                }

                room.finished = true;

                if (xExpired && !oExpired) {
                    messages.push_back({room.playerO, "EVENT WIN " + room.playerO + " OPPONENT_TIMEOUT"});
                    messages.push_back({room.playerX, "EVENT LOSE " + room.playerX + " RECONNECT_TIMEOUT"});
                    logs.push_back("Room " + std::to_string(room.id) + ": " + room.playerX + " timeout, " + room.playerO + " wins");
                } else if (oExpired && !xExpired) {
                    messages.push_back({room.playerX, "EVENT WIN " + room.playerX + " OPPONENT_TIMEOUT"});
                    messages.push_back({room.playerO, "EVENT LOSE " + room.playerO + " RECONNECT_TIMEOUT"});
                    logs.push_back("Room " + std::to_string(room.id) + ": " + room.playerO + " timeout, " + room.playerX + " wins");
                } else {
                    messages.push_back({room.playerX, "EVENT DRAW BOTH_TIMEOUT"});
                    messages.push_back({room.playerO, "EVENT DRAW BOTH_TIMEOUT"});
                    logs.push_back("Room " + std::to_string(room.id) + ": both players timeout");
                }

                sessionX->second->roomId = -1;
                sessionX->second->symbol = ' ';
                sessionO->second->roomId = -1;
                sessionO->second->symbol = ' ';
            }

            for (auto it = sessions_.begin(); it != sessions_.end();) {
                const auto& session = it->second;
                if (!session->online && session->roomId == -1 && now > session->reconnectDeadline) {
                    it = sessions_.erase(it);
                } else {
                    ++it;
                }
            }

            for (auto it = rooms_.begin(); it != rooms_.end();) {
                const RoomState& room = it->second;
                if (room.finished) {
                    it = rooms_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (const auto& msg : messages) {
            sendToUser(msg.first, msg.second);
        }
        for (const auto& log : logs) {
            Logger::instance().info(log);
        }
    }
}

void GameServer::clientLoop(SOCKET clientSocket) {
    auto connection = std::make_shared<ClientConnection>();
    connection->socket = clientSocket;

    sendLine(connection, "WELCOME NEXON_DEV_VINA MINI TURN SERVER");
    sendLine(connection, "INFO COMMANDS: LOGIN <name>, RECONNECT <name> <token>, CREATE_ROOM <name>, JOIN_ROOM <id>, LIST_ROOMS, MOVE <0-8>, LEAVE_ROOM, PING, QUIT");

    Logger::instance().info("Client connected");

    std::string line;
    while (running_.load() && connection->connected.load()) {
        if (!recvLine(connection->socket, line)) {
            break;
        }

        const auto args = split(line);
        if (args.empty()) {
            continue;
        }

        const std::string& cmd = args[0];

        if (cmd == "LOGIN") {
            onLogin(connection, args);
        } else if (cmd == "RECONNECT") {
            onReconnect(connection, args);
        } else if (cmd == "CREATE_ROOM") {
            onCreateRoom(connection, args);
        } else if (cmd == "JOIN_ROOM") {
            onJoinRoom(connection, args);
        } else if (cmd == "LIST_ROOMS") {
            onListRooms(connection);
        } else if (cmd == "MOVE") {
            onMove(connection, args);
        } else if (cmd == "LEAVE_ROOM") {
            onLeave(connection);
        } else if (cmd == "PING") {
            sendLine(connection, "PONG");
        } else if (cmd == "QUIT") {
            sendLine(connection, "BYE");
            break;
        } else {
            sendLine(connection, "ERR UNKNOWN_COMMAND");
        }
    }

    handleDisconnect(connection);
    shutdown(connection->socket, SD_BOTH);
    closesocket(connection->socket);

    Logger::instance().info("Client disconnected");
}

void GameServer::handleDisconnect(const std::shared_ptr<ClientConnection>& connection) {
    if (!connection->connected.exchange(false)) {
        return;
    }

    std::vector<std::pair<std::string, std::string>> notifications;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto session = connection->session.lock();
        if (!session) {
            return;
        }

        session->online = false;
        session->connection.reset();
        session->reconnectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(kReconnectSeconds);

        if (session->roomId != -1) {
            auto roomIt = rooms_.find(session->roomId);
            if (roomIt != rooms_.end()) {
                RoomState& room = roomIt->second;
                if (room.started && !room.finished) {
                    const std::string opponent = (room.playerX == session->username) ? room.playerO : room.playerX;
                    notifications.push_back({opponent, "EVENT OPPONENT_DISCONNECTED " + session->username + " 30"});
                }
            }
        }

        Logger::instance().warn("User " + session->username + " disconnected, reconnect window 30s");
    }

    for (const auto& msg : notifications) {
        sendToUser(msg.first, msg.second);
    }
}

void GameServer::onLogin(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args) {
    if (args.size() < 2 || !isValidUsername(args[1])) {
        sendLine(connection, "ERR LOGIN_USAGE LOGIN <alnum_or_underscore_name>");
        return;
    }

    const std::string username = args[1];
    std::string token;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (connection->session.lock()) {
            sendLine(connection, "ERR ALREADY_AUTHENTICATED");
            return;
        }

        auto existing = sessions_.find(username);
        if (existing != sessions_.end()) {
            auto& session = existing->second;
            if (session->online) {
                sendLine(connection, "ERR USER_ALREADY_ONLINE");
                return;
            }
            if (session->roomId != -1) {
                sendLine(connection, "ERR USE_RECONNECT");
                return;
            }
            sessions_.erase(existing);
        }

        token = makeToken();

        auto session = std::make_shared<PlayerSession>();
        session->username = username;
        session->token = token;
        session->online = true;
        session->roomId = -1;
        session->symbol = ' ';
        session->reconnectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(kReconnectSeconds);
        session->connection = connection;

        connection->username = username;
        connection->session = session;
        sessions_[username] = session;
    }

    sendLine(connection, "OK LOGIN " + username + " " + token);
    Logger::instance().info("User logged in: " + username);
}

void GameServer::onReconnect(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        sendLine(connection, "ERR RECONNECT_USAGE RECONNECT <name> <token>");
        return;
    }

    const std::string username = args[1];
    const std::string token = args[2];

    RoomState roomSnapshot;
    bool hasRoom = false;
    std::string symbol;
    std::string turn;
    std::string board;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (connection->session.lock()) {
            sendLine(connection, "ERR ALREADY_AUTHENTICATED");
            return;
        }

        auto it = sessions_.find(username);
        if (it == sessions_.end()) {
            sendLine(connection, "ERR SESSION_NOT_FOUND");
            return;
        }

        auto& session = it->second;
        if (session->token != token) {
            sendLine(connection, "ERR INVALID_TOKEN");
            return;
        }

        if (session->online) {
            sendLine(connection, "ERR USER_ALREADY_ONLINE");
            return;
        }

        if (std::chrono::steady_clock::now() > session->reconnectDeadline) {
            sendLine(connection, "ERR RECONNECT_TIMEOUT");
            if (session->roomId == -1) {
                sessions_.erase(it);
            }
            return;
        }

        session->online = true;
        session->connection = connection;
        connection->username = username;
        connection->session = session;

        if (session->roomId != -1) {
            auto roomIt = rooms_.find(session->roomId);
            if (roomIt != rooms_.end()) {
                hasRoom = true;
                roomSnapshot = roomIt->second;
                symbol = std::string(1, session->symbol);
                turn = roomSnapshot.turn;
                board = boardString(roomSnapshot);
            }
        }
    }

    sendLine(connection, "OK RECONNECTED " + username);
    if (hasRoom) {
        sendLine(connection, "EVENT ROOM_STATE " + std::to_string(roomSnapshot.id) + " " + symbol + " TURN " + turn);
        sendLine(connection, "EVENT BOARD " + board);

        const std::string opponent = (roomSnapshot.playerX == username) ? roomSnapshot.playerO : roomSnapshot.playerX;
        sendToUser(opponent, "EVENT OPPONENT_RECONNECTED " + username);
    }

    Logger::instance().info("User reconnected: " + username);
}

void GameServer::onCreateRoom(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args) {
    std::shared_ptr<PlayerSession> session;
    RoomState room;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        session = connection->session.lock();
        if (!session) {
            sendLine(connection, "ERR LOGIN_REQUIRED");
            return;
        }
        if (session->roomId != -1) {
            sendLine(connection, "ERR ALREADY_IN_ROOM");
            return;
        }

        room.id = nextRoomId_++;
        room.name = safeRoomName(args);
        room.playerX = session->username;
        room.playerO = "";
        room.turn = session->username;

        session->roomId = room.id;
        session->symbol = 'X';
        rooms_[room.id] = room;
    }

    sendLine(connection, "OK ROOM_CREATED " + std::to_string(room.id) + " X");
    Logger::instance().info("Room created id=" + std::to_string(room.id) + " by " + connection->username);
}

void GameServer::onJoinRoom(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        sendLine(connection, "ERR JOIN_USAGE JOIN_ROOM <room_id>");
        return;
    }

    int roomId = 0;
    try {
        roomId = std::stoi(args[1]);
    } catch (...) {
        sendLine(connection, "ERR INVALID_ROOM_ID");
        return;
    }

    std::shared_ptr<PlayerSession> session;
    RoomState roomCopy;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        session = connection->session.lock();
        if (!session) {
            sendLine(connection, "ERR LOGIN_REQUIRED");
            return;
        }
        if (session->roomId != -1) {
            sendLine(connection, "ERR ALREADY_IN_ROOM");
            return;
        }

        auto it = rooms_.find(roomId);
        if (it == rooms_.end()) {
            sendLine(connection, "ERR ROOM_NOT_FOUND");
            return;
        }

        RoomState& room = it->second;
        if (room.finished) {
            sendLine(connection, "ERR ROOM_FINISHED");
            return;
        }
        if (!room.playerO.empty()) {
            sendLine(connection, "ERR ROOM_FULL");
            return;
        }
        if (room.playerX == session->username) {
            sendLine(connection, "ERR CANNOT_JOIN_OWN_ROOM");
            return;
        }

        room.playerO = session->username;
        room.started = true;
        room.turn = room.playerX;

        session->roomId = roomId;
        session->symbol = 'O';

        roomCopy = room;
    }

    const std::string board = boardString(roomCopy);

    sendLine(connection, "OK ROOM_JOINED " + std::to_string(roomId) + " O");
    sendLine(connection, "EVENT ROOM_READY " + std::to_string(roomId) + " TURN " + roomCopy.turn);
    sendLine(connection, "EVENT BOARD " + board);

    sendToUser(roomCopy.playerX, "EVENT ROOM_READY " + std::to_string(roomId) + " TURN " + roomCopy.turn);
    sendToUser(roomCopy.playerX, "EVENT BOARD " + board);

    Logger::instance().info("User " + connection->username + " joined room id=" + std::to_string(roomId));
}

void GameServer::onListRooms(const std::shared_ptr<ClientConnection>& connection) {
    std::ostringstream oss;
    oss << "OK ROOMS";

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto& pair : rooms_) {
            const RoomState& room = pair.second;
            if (room.finished) {
                continue;
            }

            const int players = room.playerO.empty() ? 1 : 2;
            oss << " " << room.id << ":" << players << ":" << room.name;
        }
    }

    sendLine(connection, oss.str());
}

void GameServer::onMove(const std::shared_ptr<ClientConnection>& connection, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        sendLine(connection, "ERR MOVE_USAGE MOVE <0-8>");
        return;
    }

    int cell = -1;
    try {
        cell = std::stoi(args[1]);
    } catch (...) {
        sendLine(connection, "ERR INVALID_CELL");
        return;
    }
    if (cell < 0 || cell > 8) {
        sendLine(connection, "ERR INVALID_CELL_RANGE");
        return;
    }

    std::shared_ptr<PlayerSession> session;
    RoomState roomCopy;
    std::string resultForX;
    std::string resultForO;
    std::string logLine;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        session = connection->session.lock();
        if (!session) {
            sendLine(connection, "ERR LOGIN_REQUIRED");
            return;
        }
        if (session->roomId == -1) {
            sendLine(connection, "ERR NOT_IN_ROOM");
            return;
        }

        auto roomIt = rooms_.find(session->roomId);
        if (roomIt == rooms_.end()) {
            sendLine(connection, "ERR ROOM_NOT_FOUND");
            session->roomId = -1;
            session->symbol = ' ';
            return;
        }

        RoomState& room = roomIt->second;
        if (!room.started || room.finished) {
            sendLine(connection, "ERR ROOM_NOT_ACTIVE");
            return;
        }
        if (room.turn != session->username) {
            sendLine(connection, "ERR NOT_YOUR_TURN");
            return;
        }
        if (room.board[cell] != '.') {
            sendLine(connection, "ERR CELL_OCCUPIED");
            return;
        }

        room.board[cell] = session->symbol;

        const bool win = checkWin(room, session->symbol);
        const bool draw = !win && checkDraw(room);

        if (win) {
            room.finished = true;
            resultForX = "EVENT WIN " + session->username + " NORMAL";
            resultForO = "EVENT WIN " + session->username + " NORMAL";

            auto sx = sessions_.find(room.playerX);
            auto so = sessions_.find(room.playerO);
            if (sx != sessions_.end()) {
                sx->second->roomId = -1;
                sx->second->symbol = ' ';
            }
            if (so != sessions_.end()) {
                so->second->roomId = -1;
                so->second->symbol = ' ';
            }

            logLine = "Room " + std::to_string(room.id) + " winner=" + session->username;
        } else if (draw) {
            room.finished = true;
            resultForX = "EVENT DRAW";
            resultForO = "EVENT DRAW";

            auto sx = sessions_.find(room.playerX);
            auto so = sessions_.find(room.playerO);
            if (sx != sessions_.end()) {
                sx->second->roomId = -1;
                sx->second->symbol = ' ';
            }
            if (so != sessions_.end()) {
                so->second->roomId = -1;
                so->second->symbol = ' ';
            }

            logLine = "Room " + std::to_string(room.id) + " draw";
        } else {
            room.turn = (room.turn == room.playerX) ? room.playerO : room.playerX;
            resultForX = "EVENT NEXT_TURN " + room.turn;
            resultForO = "EVENT NEXT_TURN " + room.turn;
        }

        roomCopy = room;
    }

    const std::string board = boardString(roomCopy);

    sendToUser(roomCopy.playerX, "EVENT BOARD " + board);
    sendToUser(roomCopy.playerO, "EVENT BOARD " + board);

    if (!resultForX.empty()) {
        sendToUser(roomCopy.playerX, resultForX);
    }
    if (!resultForO.empty()) {
        sendToUser(roomCopy.playerO, resultForO);
    }

    if (!logLine.empty()) {
        Logger::instance().info(logLine);
    }
}

void GameServer::onLeave(const std::shared_ptr<ClientConnection>& connection) {
    std::string username;
    int roomId = -1;
    std::string opponent;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto session = connection->session.lock();
        if (!session) {
            sendLine(connection, "ERR LOGIN_REQUIRED");
            return;
        }

        username = session->username;
        roomId = session->roomId;

        if (roomId == -1) {
            sendLine(connection, "ERR NOT_IN_ROOM");
            return;
        }

        auto roomIt = rooms_.find(roomId);
        if (roomIt != rooms_.end()) {
            RoomState& room = roomIt->second;
            room.finished = true;
            opponent = (room.playerX == username) ? room.playerO : room.playerX;
        }

        session->roomId = -1;
        session->symbol = ' ';
    }

    sendLine(connection, "OK LEFT_ROOM " + std::to_string(roomId));

    if (!opponent.empty()) {
        sendToUser(opponent, "EVENT OPPONENT_LEFT " + username);
    }
}

bool GameServer::recvLine(SOCKET socket, std::string& out) {
    out.clear();
    char c = '\0';

    while (true) {
        const int bytes = recv(socket, &c, 1, 0);
        if (bytes <= 0) {
            return false;
        }

        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            out.push_back(c);
        }

        if (out.size() > 1024) {
            return false;
        }
    }

    return true;
}

std::vector<std::string> GameServer::split(const std::string& input) {
    std::istringstream iss(input);
    std::vector<std::string> parts;
    std::string token;

    while (iss >> token) {
        parts.push_back(token);
    }

    return parts;
}

bool GameServer::isValidUsername(const std::string& username) {
    if (username.size() < 3 || username.size() > 16) {
        return false;
    }

    return std::all_of(username.begin(), username.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}

bool GameServer::sendLine(const std::shared_ptr<ClientConnection>& connection, const std::string& line) {
    if (!connection || !connection->connected.load()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(connection->sendMutex);
    std::string payload = line + "\n";

    const char* data = payload.c_str();
    int totalSent = 0;
    int remaining = static_cast<int>(payload.size());

    while (remaining > 0) {
        const int sent = send(connection->socket, data + totalSent, remaining, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            connection->connected = false;
            return false;
        }
        totalSent += sent;
        remaining -= sent;
    }

    return true;
}

bool GameServer::sendToUser(const std::string& username, const std::string& line) {
    std::shared_ptr<ClientConnection> target;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = sessions_.find(username);
        if (it == sessions_.end()) {
            return false;
        }
        if (!it->second->online) {
            return false;
        }
        target = it->second->connection.lock();
    }

    return sendLine(target, line);
}

std::string GameServer::boardString(const RoomState& room) const {
    return std::string(room.board.begin(), room.board.end());
}

bool GameServer::checkWin(const RoomState& room, char symbol) const {
    static const std::array<std::array<int, 3>, 8> lines = {{
        {{0, 1, 2}},
        {{3, 4, 5}},
        {{6, 7, 8}},
        {{0, 3, 6}},
        {{1, 4, 7}},
        {{2, 5, 8}},
        {{0, 4, 8}},
        {{2, 4, 6}},
    }};

    for (const auto& line : lines) {
        if (room.board[line[0]] == symbol &&
            room.board[line[1]] == symbol &&
            room.board[line[2]] == symbol) {
            return true;
        }
    }

    return false;
}

bool GameServer::checkDraw(const RoomState& room) const {
    return std::none_of(room.board.begin(), room.board.end(), [](char c) { return c == '.'; });
}

std::string GameServer::makeToken() const {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr char kChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::string token;
    token.reserve(24);
    for (int i = 0; i < 24; ++i) {
        token.push_back(kChars[rng() % (sizeof(kChars) - 1)]);
    }
    return token;
}
