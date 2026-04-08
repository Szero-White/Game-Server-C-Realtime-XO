#pragma once

#include <fstream>
#include <mutex>
#include <string>

class Logger {
public:
    static Logger& instance();

    void init(const std::string& filePath);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

private:
    Logger() = default;

    void log(const std::string& level, const std::string& message);

    std::mutex mutex_;
    std::ofstream file_;
};
