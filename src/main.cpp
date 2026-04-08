#include "GameServer.h"
#include "Logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};

void signalHandler(int) {
    g_stop = true;
}
} // namespace

int main() {
    constexpr unsigned short kPort = 7777;

    Logger::instance().init("server.log");
    Logger::instance().info("Starting mini game server...");

    GameServer server(kPort);
    if (!server.start()) {
        Logger::instance().error("Server failed to start");
        return 1;
    }

    std::signal(SIGINT, signalHandler);

    std::cout << "Server running on port " << kPort << ". Press Ctrl+C to stop.\n";
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    server.stop();
    Logger::instance().info("Server stopped");
    return 0;
}
