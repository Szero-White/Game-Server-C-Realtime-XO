#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace {
std::atomic<bool> g_running{true};

bool recvLine(SOCKET socket, std::string& out) {
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

bool sendLine(SOCKET socket, const std::string& line) {
    std::string payload = line + "\n";
    const char* data = payload.c_str();
    int totalSent = 0;
    int remaining = static_cast<int>(payload.size());

    while (remaining > 0) {
        const int sent = send(socket, data + totalSent, remaining, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        totalSent += sent;
        remaining -= sent;
    }

    return true;
}

void printBoard(const std::string& board) {
    if (board.size() != 9) {
        return;
    }

    std::cout << "Board:\n";
    for (int row = 0; row < 3; ++row) {
        std::cout << "  " << board[row * 3] << " | " << board[row * 3 + 1] << " | " << board[row * 3 + 2] << '\n';
        if (row < 2) {
            std::cout << "  --+---+--\n";
        }
    }
}

void printServerLine(const std::string& line) {
    std::cout << "SERVER < " << line << '\n';

    constexpr const char* kBoardPrefix = "EVENT BOARD ";
    constexpr const char* kRoomReadyPrefix = "EVENT ROOM_READY ";

    if (line.rfind(kBoardPrefix, 0) == 0) {
        printBoard(line.substr(std::char_traits<char>::length(kBoardPrefix)));
        return;
    }

    if (line.rfind(kRoomReadyPrefix, 0) == 0) {
        std::cout << "  Match is ready. Use MOVE 0-8 to play.\n";
    }
}

void printUsage() {
    std::cout << "Commands:\n";
    std::cout << "  LOGIN <name>\n";
    std::cout << "  CREATE_ROOM <name>\n";
    std::cout << "  JOIN_ROOM <id>\n";
    std::cout << "  LIST_ROOMS\n";
    std::cout << "  MOVE <0-8>\n";
    std::cout << "  LEAVE_ROOM\n";
    std::cout << "  PING\n";
    std::cout << "  QUIT\n";
    std::cout << "Type /quit to exit the client.\n";
}
} // namespace

int main(int argc, char* argv[]) {
    const char* host = (argc >= 2) ? argv[1] : "127.0.0.1";
    const unsigned short port = (argc >= 3) ? static_cast<unsigned short>(std::stoi(argv[2])) : 7777;

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Failed to initialize Winsock.\n";
        return 1;
    }

    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        std::cerr << "Failed to create socket.\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &serverAddress.sin_addr) != 1) {
        std::cerr << "Invalid host address: " << host << '\n';
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) == SOCKET_ERROR) {
        std::cerr << "Failed to connect to " << host << ':' << port << '\n';
        closesocket(socketHandle);
        WSACleanup();
        return 1;
    }

    std::cout << "Connected to " << host << ':' << port << '\n';
    printUsage();

    std::thread receiver([&]() {
        std::string line;
        while (g_running.load()) {
            if (!recvLine(socketHandle, line)) {
                break;
            }
            printServerLine(line);
        }
        g_running = false;
    });

    std::string input;
    while (g_running.load() && std::getline(std::cin, input)) {
        if (input == "/quit") {
            sendLine(socketHandle, "QUIT");
            break;
        }

        if (input.empty()) {
            continue;
        }

        std::cout << "YOU    > " << input << '\n';
        if (!sendLine(socketHandle, input)) {
            std::cout << "Send failed.\n";
            break;
        }
    }

    g_running = false;
    shutdown(socketHandle, SD_BOTH);
    closesocket(socketHandle);

    if (receiver.joinable()) {
        receiver.join();
    }

    WSACleanup();
    return 0;
}