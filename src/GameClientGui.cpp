#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace {
constexpr UINT kMsgServerLine = WM_APP + 1;
constexpr int kBoardCellIds[9] = {1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009};

struct AppState {
    HWND hwnd = nullptr;
    HWND usernameEdit = nullptr;
    HWND roomNameEdit = nullptr;
    HWND roomIdEdit = nullptr;
    HWND statusLabel = nullptr;
    HWND logEdit = nullptr;
    std::array<HWND, 9> boardButtons{};

    SOCKET socketHandle = INVALID_SOCKET;
    std::thread receiverThread;
    std::mutex socketMutex;
    std::mutex queueMutex;
    std::queue<std::string> inbound;

    std::array<char, 9> board{'.', '.', '.', '.', '.', '.', '.', '.', '.'};
    std::atomic<bool> running{true};
    bool connected = false;
    std::string status = "Disconnected";
};

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

        if (out.size() > 2048) {
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

std::string trimSpaces(std::string value) {
    while (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    return value;
}

std::string getText(HWND hwnd) {
    char buffer[256] = {};
    GetWindowTextA(hwnd, buffer, sizeof(buffer) - 1);
    return trimSpaces(std::string(buffer));
}

void appendLog(HWND logEdit, const std::string& line) {
    const std::string message = line + "\r\n";
    const int endPos = GetWindowTextLengthA(logEdit);
    SendMessageA(logEdit, EM_SETSEL, endPos, endPos);
    SendMessageA(logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(message.c_str()));
}

void setStatus(HWND statusLabel, const std::string& text) {
    SetWindowTextA(statusLabel, text.c_str());
}

char boardLabelForCell(char cell, int index) {
    return (cell == '.') ? static_cast<char>('1' + index) : cell;
}

void refreshBoard(AppState& app) {
    for (int i = 0; i < 9; ++i) {
        char label[2] = {boardLabelForCell(app.board[i], i), '\0'};
        SetWindowTextA(app.boardButtons[i], label);
    }
}

void enqueueLine(AppState& app, const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(app.queueMutex);
        app.inbound.push(line);
    }
    PostMessageA(app.hwnd, kMsgServerLine, 0, 0);
}

void handleServerLine(AppState& app, const std::string& line) {
    appendLog(app.logEdit, "SERVER < " + line);

    const std::string boardPrefix = "EVENT BOARD ";
    const std::string loginPrefix = "OK LOGIN ";
    const std::string roomCreatedPrefix = "OK ROOM_CREATED ";
    const std::string roomJoinedPrefix = "OK ROOM_JOINED ";
    const std::string roomReadyPrefix = "EVENT ROOM_READY ";
    const std::string nextTurnPrefix = "EVENT NEXT_TURN ";
    const std::string winPrefix = "EVENT WIN ";
    const std::string drawPrefix = "EVENT DRAW";
    const std::string errPrefix = "ERR ";

    if (line.rfind(boardPrefix, 0) == 0 && line.size() >= boardPrefix.size() + 9) {
        for (int i = 0; i < 9; ++i) {
            app.board[i] = line[boardPrefix.size() + i];
        }
        refreshBoard(app);
        return;
    }

    if (line.rfind(loginPrefix, 0) == 0) {
        setStatus(app.statusLabel, "Logged in");
        return;
    }

    if (line.rfind(roomCreatedPrefix, 0) == 0) {
        setStatus(app.statusLabel, "Room created");
        return;
    }

    if (line.rfind(roomJoinedPrefix, 0) == 0) {
        setStatus(app.statusLabel, "Joined room");
        return;
    }

    if (line.rfind(roomReadyPrefix, 0) == 0) {
        setStatus(app.statusLabel, "Match ready");
        return;
    }

    if (line.rfind(nextTurnPrefix, 0) == 0) {
        setStatus(app.statusLabel, "Next turn: " + line.substr(nextTurnPrefix.size()));
        return;
    }

    if (line.rfind(winPrefix, 0) == 0) {
        setStatus(app.statusLabel, "Match finished: win");
        return;
    }

    if (line.rfind(drawPrefix, 0) == 0) {
        setStatus(app.statusLabel, "Match finished: draw");
        return;
    }

    if (line.rfind(errPrefix, 0) == 0) {
        setStatus(app.statusLabel, line);
        return;
    }
}

void sendCommand(AppState& app, const std::string& line) {
    appendLog(app.logEdit, "DEBUG> sendCommand called with: " + line);
    
    if (!app.connected || app.socketHandle == INVALID_SOCKET) {
        appendLog(app.logEdit, "CLIENT> Not connected (socket=" + std::to_string(app.socketHandle != INVALID_SOCKET) + ")");
        return;
    }

    std::lock_guard<std::mutex> lock(app.socketMutex);
    appendLog(app.logEdit, "DEBUG> About to send to socket");
    if (!sendLine(app.socketHandle, line)) {
        appendLog(app.logEdit, "CLIENT> Send failed");
        setStatus(app.statusLabel, "Connection lost");
        return;
    }

    appendLog(app.logEdit, "YOU    > " + line);
}

void connectAndLogin(AppState& app) {
    if (app.connected) {
        appendLog(app.logEdit, "CLIENT> Already connected");
        return;
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        appendLog(app.logEdit, "CLIENT> WSAStartup failed");
        return;
    }

    app.socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (app.socketHandle == INVALID_SOCKET) {
        appendLog(app.logEdit, "CLIENT> Failed to create socket");
        WSACleanup();
        return;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(7777);
    inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr);

    if (connect(app.socketHandle, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) == SOCKET_ERROR) {
        appendLog(app.logEdit, "CLIENT> Failed to connect to 127.0.0.1:7777");
        closesocket(app.socketHandle);
        app.socketHandle = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    app.connected = true;
    setStatus(app.statusLabel, "Connected");
    appendLog(app.logEdit, "CLIENT> Connected to 127.0.0.1:7777");

    const std::string username = getText(app.usernameEdit);
    if (username.empty()) {
        appendLog(app.logEdit, "CLIENT> Username cannot be empty");
        shutdown(app.socketHandle, SD_BOTH);
        closesocket(app.socketHandle);
        app.socketHandle = INVALID_SOCKET;
        app.connected = false;
        return;
    }

    appendLog(app.logEdit, "CLIENT> Logging in as: " + username);
    app.receiverThread = std::thread([&app]() {
        std::string line;
        while (app.running.load()) {
            if (!recvLine(app.socketHandle, line)) {
                break;
            }
            enqueueLine(app, line);
        }

        enqueueLine(app, "ERR DISCONNECTED");
    });

    sendCommand(app, "LOGIN " + username);
}

void disconnect(AppState& app) {
    app.running = false;

    if (app.socketHandle != INVALID_SOCKET) {
        shutdown(app.socketHandle, SD_BOTH);
        closesocket(app.socketHandle);
        app.socketHandle = INVALID_SOCKET;
    }

    if (app.receiverThread.joinable()) {
        app.receiverThread.join();
    }

    if (app.connected) {
        WSACleanup();
    }

    app.connected = false;
}

void createControls(HWND hwnd, AppState& app) {
    CreateWindowA("STATIC", "Username:", WS_CHILD | WS_VISIBLE,
                  16, 16, 70, 20, hwnd, nullptr, nullptr, nullptr);
    app.usernameEdit = CreateWindowA("EDIT", "alice",
                                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                     90, 14, 160, 24, hwnd, reinterpret_cast<HMENU>(2001), nullptr, nullptr);

    CreateWindowA("STATIC", "Room name:", WS_CHILD | WS_VISIBLE,
                  16, 48, 70, 20, hwnd, nullptr, nullptr, nullptr);
    app.roomNameEdit = CreateWindowA("EDIT", "DemoRoom",
                                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                     90, 46, 160, 24, hwnd, reinterpret_cast<HMENU>(2002), nullptr, nullptr);

    CreateWindowA("STATIC", "Room ID:", WS_CHILD | WS_VISIBLE,
                  16, 80, 70, 20, hwnd, nullptr, nullptr, nullptr);
    app.roomIdEdit = CreateWindowA("EDIT", "1",
                                   WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                   90, 78, 160, 24, hwnd, reinterpret_cast<HMENU>(2003), nullptr, nullptr);

    CreateWindowA("BUTTON", "Connect + Login", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  270, 14, 130, 28, hwnd, reinterpret_cast<HMENU>(3001), nullptr, nullptr);
    CreateWindowA("BUTTON", "Create Room", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  270, 46, 130, 28, hwnd, reinterpret_cast<HMENU>(3002), nullptr, nullptr);
    CreateWindowA("BUTTON", "Join Room", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  270, 78, 130, 28, hwnd, reinterpret_cast<HMENU>(3003), nullptr, nullptr);
    CreateWindowA("BUTTON", "List Rooms", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  410, 14, 120, 28, hwnd, reinterpret_cast<HMENU>(3004), nullptr, nullptr);
    CreateWindowA("BUTTON", "Leave Room", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  410, 46, 120, 28, hwnd, reinterpret_cast<HMENU>(3005), nullptr, nullptr);
    CreateWindowA("BUTTON", "Ping", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  410, 78, 120, 28, hwnd, reinterpret_cast<HMENU>(3006), nullptr, nullptr);
    CreateWindowA("BUTTON", "Quit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  540, 14, 90, 92, hwnd, reinterpret_cast<HMENU>(3007), nullptr, nullptr);

    CreateWindowA("STATIC", "Board", WS_CHILD | WS_VISIBLE,
                  16, 120, 80, 20, hwnd, nullptr, nullptr, nullptr);

    const int startX = 16;
    const int startY = 150;
    const int cellSize = 72;
    const int gap = 8;
    int idIndex = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            app.boardButtons[idIndex] = CreateWindowA(
                "BUTTON",
                std::to_string(idIndex + 1).c_str(),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                startX + col * (cellSize + gap),
                startY + row * (cellSize + gap),
                cellSize,
                cellSize,
                hwnd,
                reinterpret_cast<HMENU>(kBoardCellIds[idIndex]),
                nullptr,
                nullptr);
            ++idIndex;
        }
    }

    app.statusLabel = CreateWindowA("STATIC", "Status: disconnected", WS_CHILD | WS_VISIBLE,
                                    16, 390, 740, 20, hwnd, nullptr, nullptr, nullptr);

    app.logEdit = CreateWindowA("EDIT", "",
                                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                    ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
                                16, 420, 740, 220,
                                hwnd, reinterpret_cast<HMENU>(4001), nullptr, nullptr);

    refreshBoard(app);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        auto* app = new AppState();
        app->hwnd = hwnd;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)app);
        createControls(hwnd, *app);
        appendLog(app->logEdit, "CLIENT> Starting visible game client");
        appendLog(app->logEdit, "CLIENT> Enter username and click 'Connect + Login'");
        return 0;
    }

    case WM_COMMAND: {
        auto* app = (AppState*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        if (!app) {
            break;
        }

        const int id = LOWORD(wParam);
        if (id == 3001) {
            connectAndLogin(*app);
        } else if (id == 3002) {
            sendCommand(*app, "CREATE_ROOM " + getText(app->roomNameEdit));
        } else if (id == 3003) {
            sendCommand(*app, "JOIN_ROOM " + getText(app->roomIdEdit));
        } else if (id == 3004) {
            sendCommand(*app, "LIST_ROOMS");
        } else if (id == 3005) {
            sendCommand(*app, "LEAVE_ROOM");
        } else if (id == 3006) {
            sendCommand(*app, "PING");
        } else if (id == 3007) {
            DestroyWindow(hwnd);
        } else if (id >= 1001 && id <= 1009) {
            const int cell = id - 1001;
            sendCommand(*app, "MOVE " + std::to_string(cell));
        }
        return 0;
    }

    case kMsgServerLine: {
        auto* app = (AppState*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        if (!app) {
            return 0;
        }

        std::queue<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(app->queueMutex);
            std::swap(lines, app->inbound);
        }

        while (!lines.empty()) {
            const std::string line = lines.front();
            lines.pop();
            if (line == "ERR DISCONNECTED") {
                app->connected = false;
                setStatus(app->statusLabel, "Disconnected");
                appendLog(app->logEdit, "CLIENT> Disconnected");
                continue;
            }
            handleServerLine(*app, line);
        }
        return 0;
    }

    case WM_DESTROY: {
        auto* app = (AppState*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        if (app) {
            disconnect(*app);
            delete app;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}
} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = "NexonMiniGameClientGui";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassA(&wc)) {
        MessageBoxA(nullptr, "Failed to register window class.", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowA(
        wc.lpszClassName,
        "Nexon Mini Game Client",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        800,
        700,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd) {
        MessageBoxA(nullptr, "Failed to create window.", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return static_cast<int>(msg.wParam);
}