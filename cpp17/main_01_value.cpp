#include <algorithm>
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include "utilities.hpp"

// --- classes

#include <variant>
#include <string>
#include <type_traits>

class Value {
public:
    Value() = default;
    constexpr explicit Value(bool digital_value) : value_(digital_value) {}
    constexpr explicit Value(double analog_value) : value_(analog_value) {}

    [[nodiscard]] constexpr bool is_digital() const { return std::holds_alternative<bool>(value_); }
    [[nodiscard]] constexpr bool is_analog() const { return std::holds_alternative<double>(value_); }

    [[nodiscard]] constexpr bool get_digital() const { return std::get<bool>(value_); }
    [[nodiscard]] constexpr double get_analog() const { return std::get<double>(value_); }


    friend bool operator==(const Value& lhs, const Value& rhs) {
        return lhs.value_ == rhs.value_;
    }    

    friend bool operator!=(const Value& lhs, const Value& rhs) {
        return !(lhs == rhs);
    }    
    
    [[nodiscard]] std::string to_string() const {
        return std::visit([](auto&& arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, bool>) {
                return arg ? "1" : "0";
            } else if constexpr (std::is_same_v<T, double>) {
                return std::to_string(arg);
            }
            return "";
        }, value_);
    }

private:
    std::variant<bool, double> value_;
};

// --- controller :-(

static std::atomic<bool> running{true};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        running.store(false, std::memory_order_relaxed);
    }
}

struct Configuration {
    // communication settings
    // ...

    // sensor and command mappings
    struct Item {
        enum class Type {
            DIGITAL,
            ANALOG
        } type;
        std::string id;    // e.g., DI03
        uint16_t address;  // e.g., 3
    };
    std::vector<Item> sensors = {
        {Configuration::Item::Type::DIGITAL, "DI01", 16},
        {Configuration::Item::Type::DIGITAL, "DI02", 17},
        {Configuration::Item::Type::DIGITAL, "DI03", 18},
        {Configuration::Item::Type::DIGITAL, "DI04", 19},
        {Configuration::Item::Type::ANALOG, "AI05", 32},
        {Configuration::Item::Type::ANALOG, "AI06", 33},
        {Configuration::Item::Type::ANALOG, "AI07", 34}
    };
    std::vector<Item> commands = {
        {Configuration::Item::Type::DIGITAL, "DO03", 0},
        {Configuration::Item::Type::DIGITAL, "DO04", 1}
    };
};

std::unordered_map<std::string,Value> get_sensors_status(const Configuration& config) {
    std::unordered_map<std::string,Value> status;
    for (const auto& sensor : config.sensors) {
        switch (sensor.type) {
            case Configuration::Item::Type::DIGITAL:
                status[sensor.id] = Value(read_digital(sensor.address));
                break;
            case Configuration::Item::Type::ANALOG:
                status[sensor.id] = Value(read_analog(sensor.address));
                break;
        }
    }
    return status;
}

std::unordered_map<std::string,Value> get_commands_status(const Configuration& config) {
    std::unordered_map<std::string,Value> status;
    for (const auto& command : config.commands) {
        status[command.id] = Value(read_digital(command.address));
    }
    return status;
}

int main() {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    rtu::print("Starting RTU...\n");

    Configuration configuration;

    auto channel = std::make_unique<MockChannel>();

    std::ios::sync_with_stdio(false);
    // Start the interactive console thread.
    std::thread cli_thread(interactive_console, std::ref(running), std::ref(*channel));

    Timer timer(std::chrono::seconds(30));
    timer.start();

    std::unordered_map<std::string,Value> previous_sensors;

    while(running.load(std::memory_order_relaxed)) {

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int ch_status = channel->loop();
        if( ch_status != Channel::OK) {
            channel->connect(); // attempt to reconnect
        }
        
        std::unordered_map<std::string,Value> sensors_status = get_sensors_status(configuration);
        std::unordered_map<std::string,Value> commands_status = get_commands_status(configuration);

        const std::vector<std::string> messages = channel->get_messages();
        for (const auto& msg : messages) {
            rtu::print("[RX MESSAGE] Received: {}\n", msg);

            // format of the incoming message: CMD:<id>:<value> or SENSOR:<id>
            const auto parts = split(msg, ':');
            if (parts.empty()) {
                continue; // skip invalid messages
            }

            if (parts[0] == "CMD") {
                rtu::print(" -> [Output] Valid command\n");

                // format CMD:<id>:<value>
                if (parts.size() == 3) {
                    const std::string& id = parts[1];
                    const std::string& value_text = parts[2];
                    rtu::print(" -> [Output] ID: {} | Value: {}\n", id, value_text);

                    auto command_it = std::find_if(configuration.commands.begin(), configuration.commands.end(), [id](const auto& cmd) {
                        return cmd.id == id;
                    });

                    if (command_it == configuration.commands.end()) {
                        rtu::print(" -> [Output] Unknown command ID\n");
                        channel->send("CMD_RESULT/" + id + ":" + std::to_string(false));
                        continue;
                    }

                    try {
                        bool result = write_digital(command_it->address, std::stoi(value_text) == 1);
                        channel->send("CMD_RESULT/" + id + ":" + std::to_string(result));
                    } catch (const std::exception&) {
                        rtu::print(" -> [Output] Invalid numeric value\n");
                        channel->send("CMD_RESULT/" + id + ":" + std::to_string(false));
                    }
                }
            } else if (parts[0] == "SENSOR") {
                rtu::print(" -> [Sensor] Valid message\n");

                // format SENSOR:<id>
                if (parts.size() == 2) {
                    const std::string& id = parts[1];
                    rtu::print(" -> [Sensor] ID: {}\n", id);
                    if (id == "<ALL>") {
                        for (const auto& [id, value] : sensors_status) {
                            channel->send("SENSOR/" + id + ":" + value.to_string());
                        }
                    } else {
                        const auto sensor_it = sensors_status.find(id);
                        if (sensor_it != sensors_status.end()) {
                            channel->send("SENSOR/" + id + ":" + sensor_it->second.to_string());
                        } else {
                            rtu::print(" -> [Sensor] Unknown sensor ID\n");
                        }
                    }
                }
            }
        }

        // check for changes
        for (const auto& [id, value] : sensors_status) {
            if (previous_sensors.find(id) == previous_sensors.end() || previous_sensors[id] != value) {
                channel->send("SENSOR/" + id + ":" + value.to_string());
                previous_sensors[id] = value;
            }
        }

        // periodic update of sensor and command statuses
        if (timer.expired()) {
            for (const auto& [id, value] : sensors_status) {
                channel->send("SENSOR/" + id + ":" + value.to_string());
            }
            for (const auto& [id, value] : commands_status) {
                channel->send("CMD/" + id + ":" + value.to_string());
            }
            timer.restart();
        }
    }
    
    if (cli_thread.joinable()) {
        cli_thread.join();
    }

    rtu::print("RTU shut down...\n");
    return 0;
}