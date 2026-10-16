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
#include <cmath>
#include <optional>
#include <functional>

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


class Condition {
public:
    virtual ~Condition() = default;
    // the parameter current_active indicates whether the alarm is currently active
    // returns true if the condition for activating the alarm is met
    // it allows the implementation to apply hysteresis: the "effective" threshold depends on the current state.
    virtual bool evaluate(const Value& v, bool currently_active) const = 0;    
};

class BinaryCondition final : public Condition {
public:
    BinaryCondition(const Value& trigger_value = Value(true)) : trigger_value_(trigger_value) {}
    bool evaluate(const Value& v, bool currently_active) const override {
        return v == trigger_value_;
    }
private:
    Value trigger_value_;
};

enum class CompareOp { Gt, Lt, Ge, Le, Eq, Ne };

class ThresholdCondition final : public Condition {
public:
    ThresholdCondition(CompareOp op,
                       double threshold,
                       double hysteresis = 0.0,
                       double equality_tolerance = 0.001)
        : op_(op),
          threshold_(threshold),
          hysteresis_(hysteresis),
          equality_tolerance_(equality_tolerance) {}

    bool evaluate(const Value& v, bool currently_active) const override {
        if (!v.is_analog())
            return false;
        double x = v.get_analog();
        double effective = currently_active ? Relaxed(threshold_) : threshold_;
        return Compare(x, effective);
    }

private:
    double Relaxed(double t) const {
        switch (op_) {
            case CompareOp::Gt: case CompareOp::Ge: return t - hysteresis_;
            case CompareOp::Lt: case CompareOp::Le: return t + hysteresis_;
            default: return t;   // Eq/Ne: l'isteresi non ha senso, ignorata
        }
    }
    bool Compare(double x, double t) const {
        const double delta = std::abs(x - t);
        switch (op_) {
            case CompareOp::Gt: return x > t;
            case CompareOp::Lt: return x < t;
            case CompareOp::Ge: return x >= t;
            case CompareOp::Le: return x <= t;
            case CompareOp::Eq: return delta <= equality_tolerance_;
            case CompareOp::Ne: return delta > equality_tolerance_;
        }
        return false;
    }

    CompareOp op_;
    double threshold_;
    double hysteresis_;
    double equality_tolerance_;
};

#include <functional> // std::function

class Alarm final {
public:
    Alarm(InputPoint& target, std::unique_ptr<Condition> condition)
        : target_(target), condition_(std::move(condition)) {}

    void update() {
        const auto& target_value = target_.get_value();
        if (!target_value.has_value()) {
            has_changed_ = false;
            return;
        }
        if (!active_ && condition_->evaluate(*target_value, active_)) {
            has_changed_ = true;
            active_ = true;
            alarmId_ = nextAlarmId_++;
        } else if (active_ && !condition_->evaluate(*target_value, active_)) {
            has_changed_ = true;
            active_ = false;
        } else {
            has_changed_ = false;
        }
    }

    bool is_active() const {
        return active_;
    }

    std::string get_id() const {
        return target_.get_id();
    }

    unsigned get_alarm_id() const {
        return alarmId_;
    }

    [[nodiscard]] bool has_changed() const {
        return has_changed_;
    }

private:

    InputPoint& target_;
    std::unique_ptr<Condition> condition_;
    static unsigned nextAlarmId_;
    unsigned alarmId_ = 0;
    bool active_ = false;
    bool has_changed_ = false;
};

unsigned Alarm::nextAlarmId_ = 1;

struct Configuration {
    // communication settings
    // ...

    struct AlarmConfig {
        std::string point_id;
        std::string condition_kind = "binary";
        Value binary_trigger_value = Value(true);
        CompareOp threshold_op = CompareOp::Gt;
        double threshold_value = 0.0;
        double hysteresis = 0.0;
        double equality_tolerance = 0.001;
    };

    // sensor and command mappings
    struct Item {
        enum class Type {
            DIGITAL,
            ANALOG
        };

        struct FilterConfig {
            bool enabled = false;
            std::string kind;
            uint32_t debounce_time_ms = 0;
            double deadband_threshold = 0.0;
        };

        struct CommandModeConfig {
            std::string kind = "latch";
            uint32_t pulse_time_ms = 0;
            Value release_value = Value(false);
        };

        Type type;

        std::string id;    // e.g., DI03
        uint16_t address;  // e.g., 3
        std::string binding_kind;
        bool inverted = false;
        FilterConfig filter;
        CommandModeConfig command_mode;
        std::optional<Value> startup_value = std::nullopt;
        std::optional<Value> safe_value = std::nullopt;
    };
    std::vector<Item> sensors;
    std::vector<Item> commands;
    std::vector<AlarmConfig> alarms;
};

class FactoryCatalog {
public:
    using InputFactory = std::function<std::unique_ptr<Input>(const Configuration::Item&)>;
    using OutputFactory = std::function<std::unique_ptr<Output>(const Configuration::Item&)>;
    using FilterFactory = std::function<std::unique_ptr<Filter>(const Configuration::Item::FilterConfig&)>;
    using CommandModeFactory = std::function<std::unique_ptr<CommandMode>(const Configuration::Item::CommandModeConfig&)>;
    using AlarmConditionFactory = std::function<std::unique_ptr<Condition>(const Configuration::AlarmConfig&)>;

    void register_input(std::string kind, InputFactory factory) {
        input_factories_[std::move(kind)] = std::move(factory);
    }

    void register_output(std::string kind, OutputFactory factory) {
        output_factories_[std::move(kind)] = std::move(factory);
    }

    void register_filter(std::string kind, FilterFactory factory) {
        filter_factories_[std::move(kind)] = std::move(factory);
    }

    void register_command_mode(std::string kind, CommandModeFactory factory) {
        command_mode_factories_[std::move(kind)] = std::move(factory);
    }

    void register_alarm_condition(std::string kind, AlarmConditionFactory factory) {
        alarm_condition_factories_[std::move(kind)] = std::move(factory);
    }

    std::unique_ptr<Input> make_input(const Configuration::Item& item) const {
        const auto it = input_factories_.find(item.binding_kind);
        if (it == input_factories_.end()) {
            return nullptr;
        }
        return it->second(item);
    }

    std::unique_ptr<Output> make_output(const Configuration::Item& item) const {
        const auto it = output_factories_.find(item.binding_kind);
        if (it == output_factories_.end()) {
            return nullptr;
        }
        return it->second(item);
    }

    std::unique_ptr<Filter> make_filter(const Configuration::Item::FilterConfig& config) const {
        if (!config.enabled) {
            return nullptr;
        }
        const auto it = filter_factories_.find(config.kind);
        if (it == filter_factories_.end()) {
            return nullptr;
        }
        return it->second(config);
    }

    std::unique_ptr<CommandMode> make_command_mode(const Configuration::Item::CommandModeConfig& config) const {
        const auto it = command_mode_factories_.find(config.kind);
        if (it == command_mode_factories_.end()) {
            return nullptr;
        }
        return it->second(config);
    }

    std::unique_ptr<Condition> make_alarm_condition(const Configuration::AlarmConfig& config) const {
        const auto it = alarm_condition_factories_.find(config.condition_kind);
        if (it == alarm_condition_factories_.end()) {
            return nullptr;
        }
        return it->second(config);
    }

private:
    std::unordered_map<std::string, InputFactory> input_factories_;
    std::unordered_map<std::string, OutputFactory> output_factories_;
    std::unordered_map<std::string, FilterFactory> filter_factories_;
    std::unordered_map<std::string, CommandModeFactory> command_mode_factories_;
    std::unordered_map<std::string, AlarmConditionFactory> alarm_condition_factories_;
};

class Registry {
public:
    explicit Registry(const FactoryCatalog& factories) : factories_(factories) {}

    void populate(const Configuration& configuration) {
        for (const auto& sensor : configuration.sensors) {
            auto input = factories_.make_input(sensor);
            if (!input) {
                rtu::print("[IO] Invalid binding configuration for sensor {}\n", sensor.id);
                continue;
            }
            inputs_[sensor.id] = std::make_unique<InputPoint>(
                        sensor.id,
                        std::move(input),
                        sensor.inverted,
                        factories_.make_filter(sensor.filter));
        }

        for (const auto& command : configuration.commands) {
            if (command.type == Configuration::Item::Type::DIGITAL) {
                auto output = factories_.make_output(command);
                if (!output) {
                    rtu::print("[IO] Invalid binding configuration for command {}\n", command.id);
                    continue;
                }
                auto mode = factories_.make_command_mode(command.command_mode);
                if (!mode) {
                    rtu::print("[Command] Invalid mode configuration for command {}\n", command.id);
                    continue;
                }
                outputs_[command.id] = std::make_unique<OutputPoint>(
                    command.id,
                    std::move(output),
                    command.inverted);
                command_targets_[command.id] = std::make_unique<CommandTarget>(
                    *outputs_[command.id],
                    std::move(mode),
                    command.startup_value,
                    command.safe_value
                );
            }
        }
        
        for (const auto& alarm_config : configuration.alarms) {
            const auto input_it = inputs_.find(alarm_config.point_id);
            if (input_it == inputs_.end()) {
                continue;
            }

            auto condition = factories_.make_alarm_condition(alarm_config);
            if (!condition) {
                rtu::print("[Alarm] Invalid condition configuration for point {}\n", alarm_config.point_id);
                continue;
            }

            all_alarms_.push_back(std::make_unique<Alarm>(
                *input_it->second,
                std::move(condition)
            ));
        }
    }
    void update() {
        for (auto& [id, input] : inputs_) {
            input->update();
        }
        for (auto& [id, command_target] : command_targets_) {
            command_target->update();
        }
        for (auto& alarm : all_alarms_) {
            alarm->update();
        }
    }
    const std::unordered_map<std::string, std::unique_ptr<InputPoint>>& inputs() const {
        return inputs_;
    }
    InputPoint* input(const std::string& id) const {
        auto it = inputs_.find(id);
        if (it != inputs_.end()) {
            return it->second.get();
        }
        return nullptr;
    }
    CommandTarget* command_target(const std::string& id) const {
        static const std::unique_ptr<CommandTarget> null_ptr = nullptr;
        auto it = command_targets_.find(id);
        if (it != command_targets_.end()) {
            return it->second.get();
        }
        return nullptr;
    }
    const std::unordered_map<std::string, std::unique_ptr<OutputPoint>>& outputs() const {
        return outputs_;
    }
    const std::vector<std::unique_ptr<Alarm>>& alarms() const {
        return all_alarms_;
    }
    void to_safe_state() {
        for (auto& [id, command_target] : command_targets_) {
            command_target->to_safe_state();
        }
    }
private:
    const FactoryCatalog& factories_;
    std::unordered_map<std::string, std::unique_ptr<InputPoint>> inputs_;
    std::unordered_map<std::string, std::unique_ptr<OutputPoint>> outputs_;
    std::unordered_map<std::string, std::unique_ptr<CommandTarget>> command_targets_;
    std::vector<std::unique_ptr<Alarm>> all_alarms_;
};

class ControlCenterProxy {
public:
    ControlCenterProxy(Channel& channel, Registry& registry) 
        : channel_(channel), registry_(registry), timer_(std::chrono::seconds(30)) {
        timer_.start();
    }
    void update() {
        loop_channel();
        handle_messages();
        handle_changes();
        periodic_update();
    }
private:
    void loop_channel() {
        int ch_status = channel_.loop();
        if( ch_status != Channel::OK) {
            // If the channel is not OK, transition all command targets to their safe state.
            registry_.to_safe_state();
            // attempt to reconnect
            channel_.connect();
        }
    }

    void handle_messages() {
        const std::vector<std::string> messages = channel_.get_messages();
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
                    const std::string& value = parts[2];
                    rtu::print(" -> [Output] ID: {} | Value: {}\n", id, value);
                    CommandTarget* command_target = registry_.command_target(id);
                    if (command_target == nullptr) {
                        rtu::print(" -> [Output] Unknown command ID\n");
                        channel_.send("CMD_RESULT/" + id + ":" + std::to_string(false));
                        continue;
                    }

                    try {
                        bool result = command_target->activate(Value::from_string(value));
                        channel_.send("CMD_RESULT/" + id + ":" + std::to_string(result));
                    } catch (const std::exception&) {
                        rtu::print(" -> [Output] Invalid numeric value\n");
                        channel_.send("CMD_RESULT/" + id + ":" + std::to_string(false));
                    }
                }
            } else if (parts[0] == "SENSOR") {
                rtu::print(" -> [Sensor] Valid message\n");

                // format SENSOR:<id>
                if (parts.size() == 2) {
                    const std::string& id = parts[1];
                    rtu::print(" -> [Sensor] ID: {}\n", id);
                    if (id == "<ALL>") {
                        for (const auto& [id, input] : registry_.inputs()) {
                            if (input->get_value()) {
                                channel_.send("SENSOR/" + id + ":" + input->get_value()->to_string());
                            }
                        }
                    } else {
                        if (registry_.input(id) != nullptr) {
                            const auto& value = registry_.input(id)->get_value();
                            if (value) {
                                channel_.send("SENSOR/" + id + ":" + value->to_string());
                            }
                        }
                    }
                }
            }
        }
    }

    void handle_changes() {
        for (const auto& [id, input] : registry_.inputs()) {
            if (input->has_changed()) {
                channel_.send("SENSOR/" + id + ":" + input->get_value()->to_string());
            }
        }
        for (const auto& alarm : registry_.alarms()) {
            if (alarm->has_changed()) {
                channel_.send("ALARM/" + alarm->get_id() + "/" + std::to_string(alarm->get_alarm_id()) 
                    + ":" + std::string(alarm->is_active() ? "active" : "inactive"));
            }
        }
    }

    void periodic_update() {
        if (timer_.expired()) {
            for (const auto& [id, input] : registry_.inputs()) {
                if (input->get_value()) {
                    channel_.send("SENSOR/" + id + ":" + input->get_value()->to_string());
                }
            }
            for (const auto& [id, output] : registry_.outputs()) {
                if (output->get_value()) {
                    channel_.send("CMD/" + id + ":" + output->get_value()->to_string());
                }
            }
            for (auto& alarm : registry_.alarms()) {
                channel_.send("ALARM/" + alarm->get_id() + "/" + std::to_string(alarm->get_alarm_id()) 
                    + ":" + std::string(alarm->is_active() ? "active" : "inactive"));
            }

            timer_.restart();
        }
    }
    Channel& channel_;
    Registry& registry_;
    Timer timer_;
};

static std::atomic<bool> running{true};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        running.store(false, std::memory_order_relaxed);
    }
}

int main() {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    rtu::print("Starting RTU...\n");

    Configuration configuration = {
        .sensors = {
            {.type = Configuration::Item::Type::DIGITAL, .id = "DI01", .address = 16, .binding_kind = "digital_input"},
            {.type = Configuration::Item::Type::DIGITAL, .id = "DI02", .address = 17, .binding_kind = "digital_input"},
            {.type = Configuration::Item::Type::DIGITAL, .id = "DI03", .address = 18, .binding_kind = "digital_input", .inverted = true},
            {.type = Configuration::Item::Type::DIGITAL, .id = "DI04", .address = 19, .binding_kind = "digital_input",
                .filter = {.enabled = true, .kind = "debounce", .debounce_time_ms = 1000}},
            {.type = Configuration::Item::Type::ANALOG,  .id = "AI05", .address = 32, .binding_kind = "analog_input"},
            {.type = Configuration::Item::Type::ANALOG,  .id = "AI06", .address = 33, .binding_kind = "analog_input",
                .filter = {.enabled = true, .kind = "deadband", .deadband_threshold = 0.05}},
            {.type = Configuration::Item::Type::ANALOG,  .id = "AI07", .address = 34, .binding_kind = "analog_input",
                .filter = {.enabled = true, .kind = "deadband", .deadband_threshold = 0.05}}
        },
        .commands = {
            {.type = Configuration::Item::Type::DIGITAL, .id = "DO03", .address = 0, .binding_kind = "digital_output",
                .command_mode = {.kind = "pulse", .pulse_time_ms = 3000},
                .safe_value = Value(false)},
            {.type = Configuration::Item::Type::DIGITAL, .id = "DO04", .address = 1, .binding_kind = "digital_output",
                .command_mode = {.kind = "latch"},
                .startup_value = Value(true),
                .safe_value = Value(true)},
        },
        .alarms = {
            {.point_id = "DI01", .condition_kind = "binary"},
            {.point_id = "DI02", .condition_kind = "binary"},
            {.point_id = "AI06",
             .condition_kind = "threshold",
             .threshold_op = CompareOp::Gt,
             .threshold_value = 1.5,
             .hysteresis = 0.1}
        }
    };

    FactoryCatalog factories;
    factories.register_input("digital_input", [](const Configuration::Item& item) {
        return std::make_unique<DigitalInput>(item.address);
    });
    factories.register_input("analog_input", [](const Configuration::Item& item) {
        return std::make_unique<AnalogInput>(item.address);
    });
    factories.register_output("digital_output", [](const Configuration::Item& item) {
        return std::make_unique<DigitalOutput>(item.address);
    });
    factories.register_filter("debounce", [](const Configuration::Item::FilterConfig& config) {
        return std::make_unique<DebounceFilter>(std::chrono::milliseconds(config.debounce_time_ms));
    });
    factories.register_filter("deadband", [](const Configuration::Item::FilterConfig& config) {
        return std::make_unique<DeadbandFilter>(config.deadband_threshold);
    });
    factories.register_command_mode("latch", [](const Configuration::Item::CommandModeConfig& config) {
        return std::make_unique<LatchCommandMode>();
    });
    factories.register_command_mode("pulse", [](const Configuration::Item::CommandModeConfig& config) {
        return std::make_unique<PulseCommandMode>(
            std::chrono::milliseconds(config.pulse_time_ms),
            config.release_value);
    });
    factories.register_alarm_condition("binary", [](const Configuration::AlarmConfig& config) {
        return std::make_unique<BinaryCondition>(config.binary_trigger_value);
    });
    factories.register_alarm_condition("threshold", [](const Configuration::AlarmConfig& config) {
        return std::make_unique<ThresholdCondition>(
            config.threshold_op,
            config.threshold_value,
            config.hysteresis,
            config.equality_tolerance);
    });

    auto channel = std::make_unique<MockChannel>();
    
    std::ios::sync_with_stdio(false);
    // Start the interactive console thread.
    std::thread cli_thread(interactive_console, std::ref(running), std::ref(*channel));

    Registry registry(factories);
    registry.populate(configuration);

    ControlCenterProxy proxy(*channel, registry);

    while(running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        registry.update();
        proxy.update();
    }
    
    if (cli_thread.joinable()) {
        cli_thread.join();
    }

    rtu::print("RTU shut down...\n");
    return 0;
}