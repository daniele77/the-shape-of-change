#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
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
            std::cout << "[MockChannel] Connected successfully\n";
        }
        return connected_;
    }

    int loop() override {
        if (connected_ && !reachable_) {
            connected_ = false;
            std::cout << "[MockChannel] Disconnected due to reachability loss\n";
        }
        return connected_ ? OK : ERR_DISCONNECTED;
    }

    void send(const std::string& msg) override {
        if (!connected_) {
            return;
        }
        std::cout << "[Tx] Msg: " << msg << '\n';
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
        std::cout << "\n====================================================\n";
        std::cout << "  INTERACTIVE CONSOLE\n";
        std::cout << "  Supported commands:\n";
        std::cout << "    link up|down           -> Set the link status to up or down\n";
        std::cout << "    set <address> <value>  -> Set an analog or digital value\n";
        std::cout << "    dump                   -> Print the current register state\n";
        std::cout << "    cmd <DO> <value>       -> Simulate a message for a digital output command\n";
        std::cout << "    sensor <input>         -> Simulate a message requiring a sensor value\n";
        std::cout << "    sensors                -> Simulate a message requiring all sensors value\n";
        std::cout << "    help                   -> Show this help message\n";
        std::cout << "    exit                   -> Exit the interactive console\n";
        std::cout << "====================================================\n\n";
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
                std::cout << "[CLI] Register " << addr << " set to " << val << '\n';
            } else {
                std::cout << "[CLI] Usage: set <address> <value>\n";
            }
        } else if (cmd == "dump") {
            std::lock_guard<std::mutex> lock(io_mutex);
            std::cout << "\n--- REGISTER DUMP ---\n";
            for (auto it = io_memory.begin(); it != io_memory.end(); ++it) {
                std::cout << "  Address " << std::setw(2) << it->first << " : " << it->second << '\n';
            }
            std::cout << "---------------------\n";
        } else if (cmd == "link") {
            std::string status;
            if (command_stream >> status) {
                if (status == "up") {
                    channel.set_reachable(true);
                } else if (status == "down") {
                    channel.set_reachable(false);
                } else {
                    std::cout << "[CLI] Usage: link up|down\n";
                }
            } else {
                std::cout << "[CLI] Usage: link up|down\n";
            }
        } else if (cmd == "cmd") {
            std::string digout;
            int value;
            if (command_stream >> digout >> value) {
                std::cout << "[CLI] Executing command: " << digout << ' ' << value << '\n';
                channel.add_received_message("CMD:" + digout + ":" + std::to_string(value));
            } else {
                std::cout << "[CLI] Usage: cmd <DO> <value>\n";
            }
        } else if (cmd == "sensor") {
            std::string sensor_id;
            if (command_stream >> sensor_id) {
                std::cout << "[CLI] Simulating sensor input for: " << sensor_id << '\n';
                channel.add_received_message("SENSOR:" + sensor_id);
            } else {
                std::cout << "[CLI] Usage: sensor <input>\n";
            }
        } else if (cmd == "sensors") {
            std::cout << "[CLI] Simulating input for all sensors\n";
            channel.add_received_message("SENSOR:<ALL>");
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "exit") {
            is_running.store(false, std::memory_order_relaxed);
            break;
        } else {
            std::cout << "[CLI] Unknown command: " << cmd << '\n';
        }
    }
}
