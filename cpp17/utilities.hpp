#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rtu {
inline void print_impl(std::ostream& os, const std::string& fmt) {
    os << fmt;
}

template <typename T, typename... Ts>
inline void print_impl(std::ostream& os, const std::string& fmt, T&& value, Ts&&... rest) {
    const std::size_t pos = fmt.find("{}");
    if (pos == std::string::npos) {
        os << fmt;
        return;
    }
    os << fmt.substr(0, pos) << value;
    print_impl(os, fmt.substr(pos + 2), std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void print(const std::string& fmt, Ts&&... args) {
    print_impl(std::cout, fmt, std::forward<Ts>(args)...);
}

template <typename... Ts>
inline void println(const std::string& fmt, Ts&&... args) {
    print(fmt, std::forward<Ts>(args)...);
    std::cout << '\n';
}
}

// Utilities

inline std::vector<std::string> split(const std::string& msg, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(msg);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

class Timer {
public:
    Timer(std::chrono::milliseconds duration) : duration_(duration) {}
    void start() {
        start_time_ = std::chrono::steady_clock::now();
        running_ = true;
    }
    bool expired() const {
        return running_ && (std::chrono::steady_clock::now() - start_time_ >= duration_);
    }
    void restart() {
        start_time_ = std::chrono::steady_clock::now();
        running_ = true;
    }
    void stop() {
        start_time_ = std::chrono::steady_clock::time_point::min();
        running_ = false;
    }
private:
    std::chrono::steady_clock::time_point start_time_;
    const std::chrono::milliseconds duration_{0};
    bool running_{false};
};

// --- Hardware Abstraction Layer / HAL ---

static std::mutex io_mutex;
static std::unordered_map<std::size_t, double> io_memory;

static bool write_digital(std::size_t address, bool value) {
    std::lock_guard<std::mutex> lock(io_mutex);
    io_memory[address] = value ? 1.0 : 0.0;
    return true;
}

static bool read_digital(std::size_t address) {
    std::lock_guard<std::mutex> lock(io_mutex);
    return io_memory[address] > 0.5;
}

static double read_analog(std::size_t address) {
    std::lock_guard<std::mutex> lock(io_mutex);
    return io_memory[address];
}

// --- Simulated Communication Channel ---

class Channel {
public:
    static constexpr int OK = 0;
    static constexpr int ERR_DISCONNECTED = -1;

    virtual ~Channel() = default;
    virtual bool connect() = 0;
    virtual int loop() = 0;
    virtual void send(const std::string& msg) = 0;
    virtual std::vector<std::string> get_messages() = 0;
};

class MockChannel final : public Channel {
public:

    bool connect() override {
        connected_ = reachable_;
        if (connected_) {
            rtu::println("[MockChannel] Connected successfully");
        }
        return connected_;
    }

    int loop() override {
        if (connected_ && !reachable_) {
            connected_ = false;
            rtu::println("[MockChannel] Disconnected due to reachability loss");
        }
        return connected_ ? OK : ERR_DISCONNECTED;
    }

    void send(const std::string& msg) override {
        if (!connected_) {
            return;
        }
        rtu::println("[Tx] Msg: {}", msg);
    }

    std::vector<std::string> get_messages() override {
        std::lock_guard<std::mutex> lock(received_messages_mutex_);
        auto messages = received_messages_;
        received_messages_.clear();
        return messages;
    }

    // Simula la ricezione di comandi dal centro di controllo
    void add_received_message(const std::string& msg) {
        if (!connected_) {
            return;
        }
        std::lock_guard<std::mutex> lock(received_messages_mutex_);
        received_messages_.push_back(msg);
    }

    // simula lo stato di connessione al centro di controllo
    void set_reachable(bool reachable) {
        reachable_ = reachable;
    }

private:
    bool connected_{false};
    bool reachable_{true};
    std::vector<std::string> received_messages_;
    std::mutex received_messages_mutex_;
};

// Interactive CLI thread for terminal-driven I/O simulation.

inline void interactive_console(std::atomic<bool>& is_running, MockChannel& channel) {
    auto print_help = []() {
        rtu::println("\n====================================================");
        rtu::println("  INTERACTIVE CONSOLE");
        rtu::println("  Supported commands:");
        rtu::println("    link up|down           -> Set the link status to up or down");
        rtu::println("    set <address> <value>  -> Set an analog or digital value");
        rtu::println("    dump                   -> Print the current register state");
        rtu::println("    cmd <DO> <value>       -> Simulate a message for a digital output command");
        rtu::println("    sensor <input>         -> Simulate a message requiring a sensor value");
        rtu::println("    sensors                -> Simulate a message requiring all sensors value");
        rtu::println("    help                   -> Show this help message");
        rtu::println("    exit                   -> Exit the interactive console");
        rtu::println("====================================================\n");
    };
    print_help();

    while (is_running.load(std::memory_order_relaxed)) {
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }

        std::istringstream command_stream(line);
        std::string cmd;
        if (!(command_stream >> cmd)) {
            continue;
        }

        if (cmd == "set") {
            std::size_t addr;
            double val;
            if (command_stream >> addr >> val) {
                std::lock_guard<std::mutex> lock(io_mutex);
                io_memory[addr] = val;
                rtu::println("[CLI] Register {} set to {}", addr, val);
            } else {
                rtu::println("[CLI] Usage: set <address> <value>");
            }
        } else if (cmd == "dump") {
            std::lock_guard<std::mutex> lock(io_mutex);
            rtu::println("\n--- REGISTER DUMP ---");
            for (const auto& [addr, val] : io_memory) {
                rtu::println("  Address {:2d} : {}", addr, val);
            }
            rtu::println("---------------------");
        } else if (cmd == "link") {
            std::string status;
            if (command_stream >> status) {
                if (status == "up") {
                    channel.set_reachable(true);
                } else if (status == "down") {
                    channel.set_reachable(false);
                } else {
                    rtu::println("[CLI] Usage: link up|down");
                }
            } else {
                rtu::println("[CLI] Usage: link up|down");
            }
        } else if (cmd == "cmd") {
            std::string digout;
            int value;
            if (command_stream >> digout >> value) {
                rtu::println("[CLI] Executing command: {} {}", digout, value);
                channel.add_received_message("CMD:" + digout + ":" + std::to_string(value));
            } else {
                rtu::println("[CLI] Usage: cmd <DO> <value>");
            }
        } else if (cmd == "sensor") {
            std::string sensor_id;
            if (command_stream >> sensor_id) {
                rtu::println("[CLI] Simulating sensor input for: {}", sensor_id);
                channel.add_received_message("SENSOR:" + sensor_id);
            } else {
                rtu::println("[CLI] Usage: sensor <input>");
            }
        } else if (cmd == "sensors") {
            rtu::println("[CLI] Simulating input for all sensors");
            channel.add_received_message("SENSOR:<ALL>");
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "exit") {
            is_running.store(false, std::memory_order_relaxed);
            break;
        } else {
            rtu::println("[CLI] Unknown command: {}", cmd);
        }
    }
}
