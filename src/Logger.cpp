#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.close();
    }
    file_.open(filePath, std::ios::out | std::ios::app);
}

void Logger::info(const std::string& message) {
    log("INFO", message);
}

void Logger::warn(const std::string& message) {
    log("WARN", message);
}

void Logger::error(const std::string& message) {
    log("ERROR", message);
}

void Logger::log(const std::string& level, const std::string& message) {
    const auto now = std::chrono::system_clock::now();
    const auto timeT = std::chrono::system_clock::to_time_t(now);

    std::tm localTm{};
#ifdef _WIN32
    localtime_s(&localTm, &timeT);
#else
    localTm = *std::localtime(&timeT);
#endif

    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S")
        << " [" << level << "] " << message;

    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << oss.str() << '\n';
    if (file_.is_open()) {
        file_ << oss.str() << '\n';
        file_.flush();
    }
}
