#pragma once

#include <print>
#include <csignal>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <string>
#include <memory>
#include <atomic>
#include <random>
#include <vector>
#include <ranges>
#include <chrono>
#include <iostream>
#include <sstream>


// Utilities

inline std::vector<std::string> split(const std::string& msg, char delimiter) {
    auto parts_view = std::views::split(msg, delimiter);
    std::vector<std::string> parts;
    for (auto part : parts_view) {
        parts.push_back(std::string(part.begin(), part.end()));
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
    std::lock_guard lock(io_mutex);
    io_memory[address] = value ? 1.0 : 0.0;
    return true;
}

static bool read_digital(std::size_t address) {
    std::lock_guard lock(io_mutex);
    return io_memory[address] > 0.5;
}

static double read_analog(std::size_t address) {
    std::lock_guard lock(io_mutex);
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
            std::print("[MockChannel] Connected successfully\n");
        }
        return connected_;
    }

    int loop() override {
        if (connected_ && !reachable_) {
            connected_ = false;
            std::print("[MockChannel] Disconnected due to reachability loss\n");
        }
        return connected_ ? OK : ERR_DISCONNECTED;
    }

    void send(const std::string& msg) override {
        if (!connected_) {
            return;
        }
        std::print("[Tx] Msg: {}\n", msg);
    }

    std::vector<std::string> get_messages() override {
        std::lock_guard lock(received_messages_mutex_);
        auto messages = received_messages_;
        received_messages_.clear();
        return messages;
    }

    // Simula la ricezione di comandi dal centro di controllo
    void add_received_message(const std::string& msg) {
        if (!connected_) {
            return;
        }
        std::lock_guard lock(received_messages_mutex_);
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

inline void interactive_console(std::stop_token stop_token, std::atomic<bool>& is_running, MockChannel& channel) {
    auto print_help = []() {
        std::println("\n====================================================");
        std::println("  INTERACTIVE CONSOLE");
        std::println("  Supported commands:");
        std::println("    link up|down           -> Set the link status to up or down");
        std::println("    set <address> <value>  -> Set an analog or digital value");
        std::println("    dump                   -> Print the current register state");
        std::println("    cmd <DO> <value>       -> Simulate a message for a digital output command");
        std::println("    sensor <input>         -> Simulate a message requiring a sensor value");
        std::println("    sensors                -> Simulate a message requiring all sensors value");
        std::println("    help                   -> Show this help message");
        std::println("    exit                   -> Exit the interactive console");
        std::println("====================================================\n");
    };
    print_help();

    while (!stop_token.stop_requested() && is_running.load(std::memory_order_relaxed)) {

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
                std::lock_guard lock(io_mutex);
                io_memory[addr] = val;
                std::println("[CLI] Register {} set to {}", addr, val);
            } else {
                std::println("[CLI] Usage: set <address> <value>");
            }
        } else if (cmd == "dump") {
            std::lock_guard lock(io_mutex);
            std::println("\n--- REGISTER DUMP ---");
            for (const auto& [addr, val] : io_memory) {
                std::println("  Address {:2d} : {}", addr, val);
            }
            std::println("---------------------");
        } else if (cmd == "link") {
            std::string status;
            if (command_stream >> status) {
                if (status == "up") {
                    channel.set_reachable(true);
                } else if (status == "down") {
                    channel.set_reachable(false);
                } else {
                    std::println("[CLI] Usage: link up|down");
                }
            } else {
                std::println("[CLI] Usage: link up|down");
            }
        } else if (cmd == "cmd") {
            std::string digout;
            int value;
            if (command_stream >> digout >> value) {
                std::println("[CLI] Executing command: {} {}", digout, value);
                channel.add_received_message("CMD:" + digout + ":" + std::to_string(value));
            } else {
                std::println("[CLI] Usage: cmd <DO> <value>");
            }
        } else if (cmd == "sensor") {
            std::string sensor_id;
            if (command_stream >> sensor_id) {
                std::println("[CLI] Simulating sensor input for: {}", sensor_id);
                channel.add_received_message("SENSOR:" + sensor_id);
            } else {
                std::println("[CLI] Usage: sensor <input>");
            }
        } else if (cmd == "sensors") {
            std::println("[CLI] Simulating input for all sensors");
            channel.add_received_message("SENSOR:<ALL>");
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "exit") {
            is_running.store(false, std::memory_order_relaxed);
            break;
        } else {
            std::println("[CLI] Unknown command: {}", cmd);
        }
    }
}

