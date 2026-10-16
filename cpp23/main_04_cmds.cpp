#include <print>
#include <csignal>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <variant>
#include <string>
#include <type_traits>
#include <cmath>
#include "utilities.hpp"

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

    static Value from_string(const std::string& str) {
        if (str == "0" || str == "1") {
            return Value(str == "1");
        } else {
            try {
                return Value(std::stod(str));
            } catch (const std::invalid_argument&) {
                throw std::runtime_error("Invalid string for Value conversion");
            }
        }
    }

private:
    std::variant<bool, double> value_;
};

class Output {
public:
    virtual ~Output() = default;
    virtual bool write(const Value& value) = 0;
    virtual Value read() const = 0;
};

class Input {
public:
    virtual ~Input() = default;
    virtual Value read() const = 0;
};

class DigitalOutput final : public Output {
public:
    DigitalOutput(uint16_t address) : address_(address) {}
    bool write(const Value& value) override {
        if (!value.is_digital()) return false;
        return write_digital(address_, value.get_digital());
    }
    Value read() const override {
        return Value(read_digital(address_));
    }
private:
    const uint16_t address_;
};

class DigitalInput final : public Input {
public:
    DigitalInput(uint16_t address) : address_(address) {}
    Value read() const override {
        return Value(read_digital(address_));
    }
private:
    const uint16_t address_;
};

class AnalogInput final : public Input {
public:
    AnalogInput(uint16_t address) : address_(address) {}
    Value read() const override {
        return Value(read_analog(address_));
    }
private:
    const uint16_t address_;
};

class Filter {
public:
    virtual ~Filter() = default;
    virtual Value apply(const Value& value) const = 0;
};

class DebounceFilter final : public Filter {
public:
    explicit DebounceFilter(std::chrono::milliseconds debounce_time)
        : debounce_timer_(debounce_time) {}

    Value apply(const Value& value) const override {
        if (!value.is_digital()) {
            return value;
        }

        if (!has_output_) {
            filtered_value_ = value;
            has_output_ = true;
            return filtered_value_;
        }

        if (value == filtered_value_) {
            pending_change_ = false;
            debounce_timer_.stop();
            return filtered_value_;
        }

        if (!pending_change_ || value != candidate_value_) {
            candidate_value_ = value;
            pending_change_ = true;
            debounce_timer_.restart();
            return filtered_value_;
        }

        if (debounce_timer_.expired()) {
            filtered_value_ = candidate_value_;
            pending_change_ = false;
            debounce_timer_.stop();
        }
        return filtered_value_;
    }

private:
    mutable Value filtered_value_;
    mutable Value candidate_value_;
    mutable bool has_output_ = false;
    mutable bool pending_change_ = false;
    mutable Timer debounce_timer_;
};

class DeadbandFilter final : public Filter {
public:
    explicit DeadbandFilter(double threshold) : threshold_(threshold) {}

    Value apply(const Value& value) const override {
        if (!value.is_analog()) {
            return value;
        }

        if (!has_output_) {
            filtered_value_ = value;
            has_output_ = true;
            return filtered_value_;
        }

        if (std::abs(value.get_analog() - filtered_value_.get_analog()) < threshold_) {
            return filtered_value_;
        }

        filtered_value_ = value;
        return filtered_value_;
    }

private:
    const double threshold_;
    mutable Value filtered_value_;
    mutable bool has_output_ = false;
};

class InputPoint final {
public:
    InputPoint(const std::string& id, std::unique_ptr<Input> binding, bool inverted = false, std::unique_ptr<Filter> filter = nullptr)
        : id_(id), binding_(std::move(binding)), inverted_(inverted), filter_(std::move(filter)) {}
    const std::string& get_id() const { return id_; }
    const std::optional<Value>& get_value() const { return value_; }
    void update() {
        previous_value_ = value_;
        Value raw = binding_->read();
        // Normalizza la polarità elettrica in polarità logica PRIMA del filtro,
        // così debounce/deadband vedono già il segnale "vero".
        Value logical = fix_polarity(raw);
        value_ = filter_ ? filter_->apply(logical) : logical;
    }
    [[nodiscard]] bool has_changed() const {
        return value_.has_value() && value_ != previous_value_;
    }

private:
    Value fix_polarity(const Value& value) const {
        if (!inverted_ || !value.is_digital()) {
            return value;
        }
        return Value(!value.get_digital());
    }

    const std::string id_;
    std::unique_ptr<Input> binding_;
    const bool inverted_;
    std::unique_ptr<Filter> filter_;
    std::optional<Value> value_;
    std::optional<Value> previous_value_;
};

class OutputPoint final {
public:
    OutputPoint(const std::string& id, std::unique_ptr<Output> binding, bool inverted = false)
        : id_(id), binding_(std::move(binding)), inverted_(inverted) {}
    const std::string& get_id() const { return id_; }
    const std::optional<Value>& get_value() const { return value_; }
    bool set_value(const Value& value) {
        if (value_ && *value_ == value) {
            return true;
        }
        Value physical = fix_polarity(value);
        bool result = binding_->write(physical);
        if (result) {
            // immediate echo: for read-write points, it will be overwritten at the next Update()
            // with the value read from the binding during the next Update()
            value_ = value; // the cache always remains in logical polarity
        }
        return result;
    }

private:
    Value fix_polarity(const Value& value) const {
        if (!inverted_ || !value.is_digital()) {
            return value;
        }
        return Value(!value.get_digital());
    }

    const std::string id_;
    std::unique_ptr<Output> binding_;
    const bool inverted_;
    std::optional<Value> value_;
};

class CommandMode {
public:
    virtual ~CommandMode() = default;
    virtual bool on_receive(const Value& value, OutputPoint& target) = 0;
    virtual void update([[maybe_unused]] OutputPoint& target) {} // default no op
    virtual void to_safe_state(OutputPoint& target, const std::optional<Value>& safe_value) {
        if (safe_value) {
            target.set_value(*safe_value);
        }
    }
};

class PulseCommandMode final : public CommandMode
{
public:
    explicit PulseCommandMode(std::chrono::milliseconds duration,
                               Value releaseValue = Value(false))
        : releaseValue_(releaseValue), pending_(false), timer_(duration) {}

    bool on_receive(const Value& value, OutputPoint& target) override
    {
        bool result = target.set_value(value);
        if (result) {
            pending_ = true;
            timer_.restart();
        }
        return result;
    }

    void update(OutputPoint& target) override
    {
        if (pending_ && timer_.expired()) {
            pending_ = !target.set_value(releaseValue_);
            if (!pending_) {
                timer_.stop();
            }
        }
    }

    void to_safe_state(OutputPoint& target, const std::optional<Value>& safe_value) override
    {
        pending_ = false;
        timer_.stop();
        if (safe_value) {
            target.set_value(*safe_value);
        }
    }
private:
    Value releaseValue_;
    bool pending_;
    Timer timer_;
};

class LatchCommandMode final : public CommandMode
{
public:
    bool on_receive(const Value& value, OutputPoint& target) override
    {
        return target.set_value(value);
    }
};

class CommandTarget final {
public:
    CommandTarget(OutputPoint& target, std::unique_ptr<CommandMode> mode,
        std::optional<Value> startup_value = std::nullopt, std::optional<Value> safe_value = std::nullopt) 
    : target_(target), mode_(std::move(mode)), safe_value_(safe_value) {
        if (startup_value) {
            target_.set_value(*startup_value);
        }
    }
    bool activate(const Value& value) {
        return mode_->on_receive(value, target_);
    }
    void update() {
        mode_->update(target_);
    }
    void to_safe_state() {
        mode_->to_safe_state(target_, safe_value_);
    }
private:
    OutputPoint& target_;
    std::unique_ptr<CommandMode> mode_;
    std::optional<Value> safe_value_ = std::nullopt;
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
        };

        enum class CommandModeType {
            LATCH,
            PULSE
        };

        enum class FilterType {
            NONE,
            DEBOUNCE,
            DEADBAND
        };

        struct FilterConfig {
            bool enabled = false;
            FilterType type = FilterType::NONE;
            uint32_t debounce_time_ms = 0;
            double deadband_threshold = 0.0;
        };

        struct CommandModeConfig {
            CommandModeType type = CommandModeType::LATCH;
            uint32_t pulse_time_ms = 0;
            Value release_value = Value(false);
        };

        Type type;

        std::string id;    // e.g., DI03
        uint16_t address;  // e.g., 3
        bool inverted = false;
        FilterConfig filter;
        CommandModeConfig command_mode;
        std::optional<Value> startup_value = std::nullopt;
        std::optional<Value> safe_value = std::nullopt;
    };
    std::vector<Item> sensors = {
        {.type = Configuration::Item::Type::DIGITAL, .id = "DI01", .address = 16},
        {.type = Configuration::Item::Type::DIGITAL, .id = "DI02", .address = 17},
        {.type = Configuration::Item::Type::DIGITAL, .id = "DI03", .address = 18, .inverted = true},
        {.type = Configuration::Item::Type::DIGITAL, .id = "DI04", .address = 19,
            .filter = {.type = Configuration::Item::FilterType::DEBOUNCE, .debounce_time_ms = 1000}},
        {.type = Configuration::Item::Type::ANALOG,  .id = "AI05", .address = 32},
        {.type = Configuration::Item::Type::ANALOG,  .id = "AI06", .address = 33,
            .filter = {.type = Configuration::Item::FilterType::DEADBAND, .deadband_threshold = 0.05}},
        {.type = Configuration::Item::Type::ANALOG,  .id = "AI07", .address = 34,
            .filter = {.type = Configuration::Item::FilterType::DEADBAND, .deadband_threshold = 0.05}}
    };
    std::vector<Item> commands = {
        {.type = Configuration::Item::Type::DIGITAL, .id = "DO03", .address = 0,
            .command_mode = {.type = Configuration::Item::CommandModeType::PULSE, .pulse_time_ms = 3000}},
        {.type = Configuration::Item::Type::DIGITAL, .id = "DO04", .address = 1,
            .command_mode = {.type = Configuration::Item::CommandModeType::LATCH},
            .startup_value = Value(true),
            .safe_value = Value(true)},
    };
};

std::unique_ptr<Filter> make_filter(const Configuration::Item& item) {
    switch (item.filter.type) {
        case Configuration::Item::FilterType::DEBOUNCE:
            return std::make_unique<DebounceFilter>(std::chrono::milliseconds(item.filter.debounce_time_ms));
        case Configuration::Item::FilterType::DEADBAND:
            return std::make_unique<DeadbandFilter>(item.filter.deadband_threshold);
        case Configuration::Item::FilterType::NONE:
            return nullptr;
    }
    return nullptr;
}

std::unique_ptr<CommandMode> make_command_mode(const Configuration::Item& item) {
    switch (item.command_mode.type) {
        case Configuration::Item::CommandModeType::LATCH:
            return std::make_unique<LatchCommandMode>();
        case Configuration::Item::CommandModeType::PULSE:
            return std::make_unique<PulseCommandMode>(
                std::chrono::milliseconds(item.command_mode.pulse_time_ms),
                item.command_mode.release_value);
    }
    return std::make_unique<LatchCommandMode>();
}


int main() {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    std::print("Starting RTU...\n");

    Configuration configuration;

    auto channel = std::make_unique<MockChannel>();
    
    std::ios::sync_with_stdio(false);
    // Start the interactive console thread.
    std::jthread cli_thread(interactive_console, std::ref(running), std::ref(*channel));

    std::unordered_map<std::string, std::unique_ptr<InputPoint>> inputs;
    for (const auto& sensor : configuration.sensors) {
        std::unique_ptr<Input> input;
        switch (sensor.type) {
            case Configuration::Item::Type::DIGITAL:
                input = std::make_unique<DigitalInput>(sensor.address);
                break;
            case Configuration::Item::Type::ANALOG:
                input = std::make_unique<AnalogInput>(sensor.address);
                break;
        }
        inputs[sensor.id] = std::make_unique<InputPoint>(
                    sensor.id,
                    std::move(input),
                    sensor.inverted,
                    make_filter(sensor));
    }
    std::unordered_map<std::string, std::unique_ptr<OutputPoint>> outputs;
    std::unordered_map<std::string, std::unique_ptr<CommandTarget>> command_targets;
    for (const auto& command : configuration.commands) {
        if (command.type == Configuration::Item::Type::DIGITAL) {
            outputs[command.id] = std::make_unique<OutputPoint>(
                command.id,
                std::make_unique<DigitalOutput>(command.address),
                command.inverted);
            command_targets[command.id] = std::make_unique<CommandTarget>(
                *outputs[command.id],
                make_command_mode(command),
                command.startup_value,
                command.safe_value
            );
        }
    }

    Timer timer(std::chrono::seconds(30));
    timer.start();

    while(running.load(std::memory_order_relaxed)) {

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int ch_status = channel->loop();
        if( ch_status != Channel::OK) {
            // If the channel is not OK, transition all command targets to their safe state.
            for (auto& [id, command_target] : command_targets) {
                command_target->to_safe_state();
            }
            // attempt to reconnect
            channel->connect();
        }

        for (auto& [id, input_point] : inputs) {
            input_point->update();
        }
        for (auto& [id, command_target] : command_targets) {
            command_target->update();
        }
        
        const std::vector<std::string> messages = channel->get_messages();
        for (const auto& msg : messages) {
            std::print("[RX MESSAGE] Received: {}\n", msg);

            // format of the incoming message: CMD:<id>:<value> or SENSOR:<id>
            const auto parts = split(msg, ':');
            if (parts.empty()) {
                continue; // skip invalid messages
            }

            if (parts[0] == "CMD") {
                std::print(" -> [Output] Valid command\n");

                // format CMD:<id>:<value>
                if (parts.size() == 3) {
                    const std::string& id = parts[1];
                    const std::string& value = parts[2];
                    std::print(" -> [Output] ID: {} | Value: {}\n", id, value);
                    if (command_targets.find(id) == command_targets.end()) {
                        std::print(" -> [Output] Unknown command ID\n");
                        channel->send("CMD_RESULT/" + id + ":" + std::to_string(false));
                        continue;
                    }

                    try {
                        bool result = command_targets[id]->activate(Value::from_string(value));
                        channel->send("CMD_RESULT/" + id + ":" + std::to_string(result));
                    } catch (const std::exception&) {
                        std::print(" -> [Output] Invalid numeric value\n");
                        channel->send("CMD_RESULT/" + id + ":" + std::to_string(false));
                    }
                }
            } else if (parts[0] == "SENSOR") {
                std::print(" -> [Sensor] Valid message\n");

                // format SENSOR:<id>
                if (parts.size() == 2) {
                    const std::string& id = parts[1];
                    std::print(" -> [Sensor] ID: {}\n", id);
                    if (id == "<ALL>") {
                        for (const auto& [id, input] : inputs) {
                            if (input->get_value()) {
                                channel->send("SENSOR/" + id + ":" + input->get_value()->to_string());
                            }
                        }
                    } else {
                        if (inputs.find(id) != inputs.end()) {
                            const auto& value = inputs[id]->get_value();
                            if (value) {
                                channel->send("SENSOR/" + id + ":" + value->to_string());
                            }
                        }
                    }
                }
            }
        }

        // check for changes
        for (const auto& [id, input] : inputs) {
            if (input->has_changed()) {
                if (input->get_value()) {
                    channel->send("SENSOR/" + id + ":" + input->get_value()->to_string());
                }
            }
        }

        // periodic update of sensor and command statuses
        if (timer.expired()) {
            for (const auto& [id, input] : inputs) {
                if (input->get_value()) {
                    channel->send("SENSOR/" + id + ":" + input->get_value()->to_string());
                }
            }
            for (const auto& [id, output] : outputs) {
                if (output->get_value()) {
                    channel->send("CMD/" + id + ":" + output->get_value()->to_string());
                }
            }

            timer.restart();
        }
    }
    
    cli_thread.request_stop();

    std::print("RTU shut down...\n");
    return 0;
}