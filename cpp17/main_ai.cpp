#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utilities.hpp"

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_running.store(false, std::memory_order_relaxed);
    }
}

struct Configuration {
    struct Item {
        enum class Type {
            DIGITAL,
            ANALOG
        } type;

        std::string id;
        uint16_t address;
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

std::unordered_map<std::string, double> get_sensors_status(const Configuration& config) {
    std::unordered_map<std::string, double> status;
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

std::unordered_map<std::string, double> get_commands_status(const Configuration& config) {
    std::unordered_map<std::string, double> status;
    for (const auto& command : config.commands) {
        status[command.id] = read_digital(command.address);
    }
    return status;
}

class RTUController {
public:
    RTUController() : timer_(std::chrono::seconds(30)), channel_(std::make_unique<MockChannel>()) {
    }

    int run() {
        std::signal(SIGTERM, signal_handler);
        std::signal(SIGINT, signal_handler);

        rtu::print("Starting RTU...\n");

        std::ios::sync_with_stdio(false);
        start_cli_thread();
        timer_.start();

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            const int ch_status = channel_->loop();
            if (ch_status != Channel::OK) {
                channel_->connect();
            }

            auto sensors_status = get_sensors_status(configuration_);
            auto commands_status = get_commands_status(configuration_);

            process_messages(sensors_status, commands_status);
            publish_sensor_changes(sensors_status);

            if (timer_.expired()) {
                publish_periodic_status(sensors_status, commands_status);
                timer_.restart();
            }
        }

        stop_cli_thread();
        rtu::print("RTU shut down...\n");
        return 0;
    }

private:
    void start_cli_thread() {
        cli_thread_ = std::thread(interactive_console, std::ref(g_running), std::ref(*channel_));
    }

    void stop_cli_thread() {
        if (cli_thread_.joinable()) {
            cli_thread_.join();
        }
    }

    void process_messages(
        const std::unordered_map<std::string, double>& sensors_status,
        const std::unordered_map<std::string, double>& commands_status) {
        const std::vector<std::string> messages = channel_->get_messages();
        for (const auto& msg : messages) {
            rtu::print("[RX MESSAGE] Received: {}\n", msg);

            const auto parts = split(msg, ':');
            if (parts.empty()) {
                continue;
            }

            if (parts[0] == "CMD") {
                handle_command(parts, commands_status);
            } else if (parts[0] == "SENSOR") {
                handle_sensor(parts, sensors_status);
            }
        }
    }

    void handle_command(
        const std::vector<std::string>& parts,
        const std::unordered_map<std::string, double>& commands_status) {
        rtu::print(" -> [Output] Valid command\n");

        if (parts.size() != 3) {
            return;
        }

        const std::string& id = parts[1];
        const std::string& value_text = parts[2];
        rtu::print(" -> [Output] ID: {} | Value: {}\n", id, value_text);

        auto command_it = std::find_if(
            configuration_.commands.begin(),
            configuration_.commands.end(),
            [&id](const auto& cmd) { return cmd.id == id; });

        if (command_it == configuration_.commands.end()) {
            rtu::print(" -> [Output] Unknown command ID\n");
            return;
        }

        try {
            const int numeric_value = std::stoi(value_text);
            write_digital(command_it->address, numeric_value == 1);
        } catch (const std::exception&) {
            rtu::print(" -> [Output] Invalid numeric value\n");
        }
    }

    void handle_sensor(
        const std::vector<std::string>& parts,
        const std::unordered_map<std::string, double>& sensors_status) {
        rtu::print(" -> [Sensor] Valid message\n");

        if (parts.size() != 2) {
            return;
        }

        const std::string& id = parts[1];
        rtu::print(" -> [Sensor] ID: {}\n", id);

        if (id == "<ALL>") {
            for (const auto& [sensor_id, value] : sensors_status) {
                channel_->send("SENSOR/" + sensor_id + ":" + std::to_string(value));
            }
            return;
        }

        const auto sensor_it = sensors_status.find(id);
        if (sensor_it != sensors_status.end()) {
            channel_->send("SENSOR/" + id + ":" + std::to_string(sensor_it->second));
        } else {
            rtu::print(" -> [Sensor] Unknown sensor ID\n");
        }
    }

    void publish_sensor_changes(const std::unordered_map<std::string, double>& sensors_status) {
        for (const auto& [id, value] : sensors_status) {
            if (previous_sensors_.find(id) == previous_sensors_.end() || previous_sensors_[id] != value) {
                channel_->send("SENSOR/" + id + ":" + std::to_string(value));
                previous_sensors_[id] = value;
            }
        }
    }

    void publish_periodic_status(
        const std::unordered_map<std::string, double>& sensors_status,
        const std::unordered_map<std::string, double>& commands_status) {
        for (const auto& [id, value] : sensors_status) {
            channel_->send("SENSOR/" + id + ":" + std::to_string(value));
        }

        for (const auto& [id, value] : commands_status) {
            channel_->send("CMD/" + id + ":" + std::to_string(value));
        }
    }

private:
    Configuration configuration_;
    Timer timer_;
    std::unique_ptr<MockChannel> channel_;
    std::unordered_map<std::string, double> previous_sensors_;
    std::thread cli_thread_;
};

}  // namespace

int main() {
    RTUController app;
    return app.run();
}
