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

std::unordered_map<std::string,double> get_sensors_status(const Configuration& config) {
    std::unordered_map<std::string,double> status;
    for (const auto& sensor : config.sensors) {
        switch (sensor.type) {
            case Configuration::Item::Type::DIGITAL:
                status[sensor.id] = read_digital(sensor.address);
                break;
            case Configuration::Item::Type::ANALOG:
                status[sensor.id] = read_analog(sensor.address);
                break;
        }
    }
    return status;
}

std::unordered_map<std::string,double> get_commands_status(const Configuration& config) {
    std::unordered_map<std::string,double> status;
    for (const auto& command : config.commands) {
        status[command.id] = read_digital(command.address);
    }
    return status;
}

int main() {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    std::cout << "Starting RTU...\n";

    Configuration configuration;

    auto channel = std::make_unique<MockChannel>();

    std::ios::sync_with_stdio(false);
    // Start the interactive console thread.
    std::thread cli_thread(interactive_console, std::ref(running), std::ref(*channel));

    Timer timer(std::chrono::seconds(30));
    timer.start();

    std::unordered_map<std::string,double> previous_sensors;

    while(running.load(std::memory_order_relaxed)) {

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        std::unordered_map<std::string,double> sensors_status = get_sensors_status(configuration);
        std::unordered_map<std::string,double> commands_status = get_commands_status(configuration);

        int ch_status = channel->loop();
        if( ch_status != Channel::OK) {
            channel->connect(); // attempt to reconnect
        }

        const std::vector<std::string> messages = channel->get_messages();
        for (const auto& msg : messages) {
            std::cout << "[RX MESSAGE] Received: " << msg << '\n';

            // format of the incoming message: CMD:<id>:<value> or SENSOR:<id>
            const auto parts = split(msg, ':');
            if (parts.empty()) {
                continue; // skip invalid messages
            }

            if (parts[0] == "CMD") {
                std::cout << " -> [Output] Valid command\n";

                // format CMD:<id>:<value>
                if (parts.size() == 3) {
                    const std::string& id = parts[1];
                    const std::string& value_text = parts[2];
                    std::cout << " -> [Output] ID: " << id << " | Value: " << value_text << '\n';

                    auto command_it = std::find_if(configuration.commands.begin(), configuration.commands.end(), [id](const auto& cmd) {
                        return cmd.id == id;
                    });

                    if (command_it == configuration.commands.end()) {
                        std::cout << " -> [Output] Unknown command ID\n";
                        continue;
                    }

                    try {
                        const int numeric_value = std::stoi(value_text);
                        write_digital(command_it->address, numeric_value == 1);
                    } catch (const std::exception&) {
                        std::cout << " -> [Output] Invalid numeric value\n";
                    }
                }
            } else if (parts[0] == "SENSOR") {
                std::cout << " -> [Sensor] Valid message\n";

                // format SENSOR:<id>
                if (parts.size() == 2) {
                    const std::string& id = parts[1];
                    std::cout << " -> [Sensor] ID: " << id << '\n';
                    if (id == "<ALL>") {
                        for (auto it = sensors_status.begin(); it != sensors_status.end(); ++it) {
                            channel->send("SENSOR/" + it->first + ":" + std::to_string(it->second));
                        }
                    } else {
                        const auto sensor_it = sensors_status.find(id);
                        if (sensor_it != sensors_status.end()) {
                            channel->send("SENSOR/" + id + ":" + std::to_string(sensor_it->second));
                        } else {
                            std::cout << " -> [Sensor] Unknown sensor ID\n";
                        }
                    }
                }
            }
        }

        // check for changes
        for (auto it = sensors_status.begin(); it != sensors_status.end(); ++it) {
            const std::string& id = it->first;
            const auto& value = it->second;
            if (previous_sensors.find(id) == previous_sensors.end() || previous_sensors[id] != value) {
                channel->send("SENSOR/" + id + ":" + std::to_string(value));
                previous_sensors[id] = value;
            }
        }

        // periodic update of sensor and command statuses
        if (timer.expired()) {
            for (auto it = sensors_status.begin(); it != sensors_status.end(); ++it) {
                const std::string& id = it->first;
                const auto& value = it->second;
                channel->send("SENSOR/" + id + ":" + std::to_string(value));
            }
            for (auto it = commands_status.begin(); it != commands_status.end(); ++it) {
                const std::string& id = it->first;
                const auto& value = it->second;
                channel->send("CMD/" + id + ":" + std::to_string(value));
            }
            timer.restart();
        }
    }
    
    if (cli_thread.joinable()) {
        cli_thread.join();
    }

    std::cout << "RTU shut down...\n";
    return 0;
}