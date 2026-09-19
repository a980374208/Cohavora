#include <asio.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "data_stream.h"
#include "log_redaction.h"
#include "operation.h"
#include "room.h"

namespace {

using namespace std::chrono_literals;

constexpr int kExitPassed = 0;
constexpr int kExitFailed = 1;
constexpr int kExitInconclusive = 2;
constexpr std::size_t kDefaultTextBytes = 45'000;
constexpr std::size_t kDefaultByteBytes = 4 * 1024 * 1024;
constexpr std::size_t kDefaultBatchBytes = 1024 * 1024;
constexpr std::size_t kDefaultMaxBytes = 256 * 1024 * 1024;
constexpr std::size_t kMaximumConfiguredBytes = 1024ull * 1024 * 1024;

std::mutex g_output_mutex;

template <typename... Values>
void PrintLine(Values&&... values) {
    std::lock_guard lock(g_output_mutex);
    (std::cout << ... << std::forward<Values>(values)) << '\n';
    std::cout.flush();
}

std::string SafeOutput(std::string_view value) {
    return livekit::secure_log::SanitizeForOutput(value);
}

enum class Role { Sender, Receiver };
enum class RuntimeCase { Baseline, Backpressure, SoftResume, FullRestart };

struct Config {
    Role role = Role::Receiver;
    RuntimeCase runtime_case = RuntimeCase::Baseline;
    std::string url;
    std::string token;
    std::string topic = "lk.l3.stream";
    std::string run_id;
    std::string destination;
    std::string expected_sender;
    std::size_t text_bytes = kDefaultTextBytes;
    std::size_t byte_bytes = kDefaultByteBytes;
    std::size_t batch_bytes = kDefaultBatchBytes;
    std::size_t max_bytes = kDefaultMaxBytes;
    int timeout_seconds = 60;
    int connect_timeout_seconds = 15;
    int start_delay_seconds = 2;
    int fault_delay_seconds = 10;
    int recovery_delay_seconds = 10;
    int settle_seconds = 2;
    int incomplete_grace_seconds = 5;
    int expected_complete = -1;
    int expected_incomplete = -1;
};

struct ParseResult {
    std::optional<Config> config;
    int exit_code = kExitFailed;
};

class RuntimeFailure final : public std::runtime_error {
public:
    explicit RuntimeFailure(std::string code)
        : std::runtime_error("runtime validation failed"), code_(std::move(code)) {}

    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

const char* RoleName(Role role) {
    return role == Role::Sender ? "sender" : "receiver";
}

const char* CaseName(RuntimeCase value) {
    switch (value) {
    case RuntimeCase::Baseline: return "baseline";
    case RuntimeCase::Backpressure: return "backpressure";
    case RuntimeCase::SoftResume: return "soft-resume";
    case RuntimeCase::FullRestart: return "full-restart";
    }
    return "unknown";
}

const char* StateName(livekit::ConnectionState state) {
    switch (state) {
    case livekit::ConnectionState::Disconnected: return "disconnected";
    case livekit::ConnectionState::Connecting: return "connecting";
    case livekit::ConnectionState::Connected: return "connected";
    case livekit::ConnectionState::Reconnecting: return "reconnecting";
    }
    return "unknown";
}

const char* OperationName(livekit::OperationKind operation) {
    switch (operation) {
    case livekit::OperationKind::Connect: return "Connect";
    case livekit::OperationKind::PublishTrack: return "PublishTrack";
    case livekit::OperationKind::UnpublishTrack: return "UnpublishTrack";
    case livekit::OperationKind::Negotiate: return "Negotiate";
    case livekit::OperationKind::Reconnect: return "Reconnect";
    case livekit::OperationKind::Disconnect: return "Disconnect";
    case livekit::OperationKind::SendData: return "SendData";
    }
    return "Unknown";
}

const char* ErrorCodeName(livekit::OperationErrorCode code) {
    switch (code) {
    case livekit::OperationErrorCode::InvalidState: return "InvalidState";
    case livekit::OperationErrorCode::Cancelled: return "Cancelled";
    case livekit::OperationErrorCode::SignalConnectFailed: return "SignalConnectFailed";
    case livekit::OperationErrorCode::JoinTimeout: return "JoinTimeout";
    case livekit::OperationErrorCode::JoinRejected: return "JoinRejected";
    case livekit::OperationErrorCode::PeerConnectionCreateFailed: return "PeerConnectionCreateFailed";
    case livekit::OperationErrorCode::NegotiationFailed: return "NegotiationFailed";
    case livekit::OperationErrorCode::PeerConnectionTimeout: return "PeerConnectionTimeout";
    case livekit::OperationErrorCode::PermissionDenied: return "PermissionDenied";
    case livekit::OperationErrorCode::TrackPublishTimeout: return "TrackPublishTimeout";
    case livekit::OperationErrorCode::TrackPublishRejected: return "TrackPublishRejected";
    case livekit::OperationErrorCode::TrackUnpublishTimeout: return "TrackUnpublishTimeout";
    case livekit::OperationErrorCode::ReconnectExhausted: return "ReconnectExhausted";
    case livekit::OperationErrorCode::SessionClosed: return "SessionClosed";
    case livekit::OperationErrorCode::StateUncertain: return "StateUncertain";
    case livekit::OperationErrorCode::SessionInvalid: return "SessionInvalid";
    case livekit::OperationErrorCode::DataChannelUnavailable: return "DataChannelUnavailable";
    case livekit::OperationErrorCode::SerializationFailed: return "SerializationFailed";
    case livekit::OperationErrorCode::DataChannelRejected: return "DataChannelRejected";
    }
    return "Unknown";
}

void PrintUsage(const char* executable) {
    std::cout
        << "Usage:\n"
        << "  " << executable << " --role sender|receiver --case CASE --run-id ID [options]\n\n"
        << "CASES:\n"
        << "  baseline | backpressure | soft-resume | full-restart\n\n"
        << "COMMON OPTIONS:\n"
        << "  --url URL                    or LIVEKIT_URL\n"
        << "  --token TOKEN                or LIVEKIT_TOKEN (environment preferred)\n"
        << "  --topic TOPIC                default: lk.l3.stream\n"
        << "  --run-id ID                  required; isolates one matrix execution\n"
        << "  --timeout-sec N              default: 60\n"
        << "  --connect-timeout-sec N      default: 15\n"
        << "  --start-delay-sec N          sender wait for receiver; default: 2\n"
        << "  --settle-sec N               post-send observation; default: 2\n\n"
        << "SENDER OPTIONS:\n"
        << "  --destination ID             recommended receiver identity\n"
        << "  --text-bytes N               baseline text bytes; default: 45000\n"
        << "  --byte-bytes N               baseline byte bytes; default: 4194304\n"
        << "  --batch-bytes N              backpressure batch; default: 1048576\n"
        << "  --max-bytes N                bounded backpressure total; default: 268435456\n"
        << "  --fault-delay-sec N          delay after accepted prefix; default: 10\n"
        << "  --recovery-delay-sec N       delay to remove shaping; default: 10\n\n"
        << "RECEIVER OPTIONS:\n"
        << "  --expect-sender ID           required sender identity\n"
        << "  --expect-complete N          override expected normal streams\n"
        << "  --expect-incomplete N        override expected incomplete streams\n"
        << "  --incomplete-grace-sec N     collection time before local close; default: 5\n\n"
        << "Examples:\n"
        << "  Receiver: --role receiver --case baseline --run-id run-001 --expect-sender sender-a\n"
        << "  Sender:   --role sender --case baseline --run-id run-001 --destination receiver-b\n";
}

template <typename Integer>
bool ParseUnsigned(std::string_view text, Integer& output) {
    if (text.empty()) return false;
    Integer value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    output = value;
    return true;
}

std::optional<RuntimeCase> ParseCase(std::string_view value) {
    if (value == "baseline") return RuntimeCase::Baseline;
    if (value == "backpressure") return RuntimeCase::Backpressure;
    if (value == "soft-resume") return RuntimeCase::SoftResume;
    if (value == "full-restart") return RuntimeCase::FullRestart;
    return std::nullopt;
}

ParseResult ParseArguments(int argc, char** argv) {
    Config config;
    bool role_set = false;
    bool case_set = false;
    bool complete_set = false;
    bool incomplete_set = false;

    if (const char* value = std::getenv("LIVEKIT_URL")) config.url = value;
    if (const char* value = std::getenv("LIVEKIT_TOKEN")) config.token = value;
    if (const char* value = std::getenv("LIVEKIT_L3_RUN_ID")) config.run_id = value;

    auto require_value = [&](int& index) -> std::optional<std::string> {
        if (index + 1 >= argc) {
            PrintLine("[CONFIG_ERROR] missing option value");
            return std::nullopt;
        }
        return std::string(argv[++index]);
    };

    const auto is_known_option = [](std::string_view option) {
        return option == "--role" || option == "--case" || option == "--url" ||
            option == "--token" || option == "--topic" ||
            option == "--run-id" || option == "--destination" ||
            option == "--expect-sender" || option == "--text-bytes" ||
            option == "--byte-bytes" || option == "--batch-bytes" ||
            option == "--max-bytes" || option == "--timeout-sec" ||
            option == "--connect-timeout-sec" ||
            option == "--start-delay-sec" || option == "--fault-delay-sec" ||
            option == "--recovery-delay-sec" || option == "--settle-sec" ||
            option == "--incomplete-grace-sec" ||
            option == "--expect-complete" || option == "--expect-incomplete";
    };

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "-h" || option == "--help") {
            PrintUsage(argv[0]);
            return {{}, kExitPassed};
        }
        if (!is_known_option(option)) {
            PrintLine("[CONFIG_ERROR] unknown option");
            return {};
        }
        auto value = require_value(i);
        if (!value) return {};

        if (option == "--role") {
            if (*value == "sender") config.role = Role::Sender;
            else if (*value == "receiver") config.role = Role::Receiver;
            else {
                PrintLine("[CONFIG_ERROR] invalid role");
                return {};
            }
            role_set = true;
        } else if (option == "--case") {
            const auto parsed = ParseCase(*value);
            if (!parsed) {
                PrintLine("[CONFIG_ERROR] invalid case");
                return {};
            }
            config.runtime_case = *parsed;
            case_set = true;
        } else if (option == "--url") config.url = *value;
        else if (option == "--token") config.token = *value;
        else if (option == "--topic") config.topic = *value;
        else if (option == "--run-id") config.run_id = *value;
        else if (option == "--destination") config.destination = *value;
        else if (option == "--expect-sender") config.expected_sender = *value;
        else {
            const bool size_option = option == "--text-bytes" ||
                option == "--byte-bytes" || option == "--batch-bytes" ||
                option == "--max-bytes";
            const bool int_option = option == "--timeout-sec" ||
                option == "--connect-timeout-sec" ||
                option == "--start-delay-sec" ||
                option == "--fault-delay-sec" ||
                option == "--recovery-delay-sec" || option == "--settle-sec" ||
                option == "--incomplete-grace-sec" ||
                option == "--expect-complete" ||
                option == "--expect-incomplete";
            if (!size_option && !int_option) {
                PrintLine("[CONFIG_ERROR] unknown option");
                return {};
            }

            uint64_t number = 0;
            if (!ParseUnsigned<uint64_t>(*value, number)) {
                PrintLine("[CONFIG_ERROR] invalid unsigned value option=", option);
                return {};
            }
            if ((size_option &&
                 number > std::numeric_limits<std::size_t>::max()) ||
                (int_option &&
                 number > static_cast<uint64_t>(std::numeric_limits<int>::max()))) {
                PrintLine("[CONFIG_ERROR] value out of range option=", option);
                return {};
            }
            if (option == "--text-bytes") config.text_bytes = static_cast<std::size_t>(number);
            else if (option == "--byte-bytes") config.byte_bytes = static_cast<std::size_t>(number);
            else if (option == "--batch-bytes") config.batch_bytes = static_cast<std::size_t>(number);
            else if (option == "--max-bytes") config.max_bytes = static_cast<std::size_t>(number);
            else if (option == "--timeout-sec") config.timeout_seconds = static_cast<int>(number);
            else if (option == "--connect-timeout-sec") config.connect_timeout_seconds = static_cast<int>(number);
            else if (option == "--start-delay-sec") config.start_delay_seconds = static_cast<int>(number);
            else if (option == "--fault-delay-sec") config.fault_delay_seconds = static_cast<int>(number);
            else if (option == "--recovery-delay-sec") config.recovery_delay_seconds = static_cast<int>(number);
            else if (option == "--settle-sec") config.settle_seconds = static_cast<int>(number);
            else if (option == "--incomplete-grace-sec") config.incomplete_grace_seconds = static_cast<int>(number);
            else if (option == "--expect-complete") {
                config.expected_complete = static_cast<int>(number);
                complete_set = true;
            } else if (option == "--expect-incomplete") {
                config.expected_incomplete = static_cast<int>(number);
                incomplete_set = true;
            }
        }
    }

    if (!role_set || !case_set || config.url.empty() || config.token.empty() ||
        config.run_id.empty() || config.topic.empty()) {
        PrintLine("[CONFIG_ERROR] role, case, URL, token, topic, and run-id are required");
        return {};
    }
    if (config.role == Role::Receiver && config.expected_sender.empty()) {
        PrintLine("[CONFIG_ERROR] receiver requires --expect-sender");
        return {};
    }
    if (config.text_bytes == 0 || config.byte_bytes == 0 || config.batch_bytes == 0 ||
        config.max_bytes == 0 || config.text_bytes > kMaximumConfiguredBytes ||
        config.byte_bytes > kMaximumConfiguredBytes ||
        config.batch_bytes > kMaximumConfiguredBytes ||
        config.max_bytes > kMaximumConfiguredBytes ||
        config.batch_bytes > config.max_bytes) {
        PrintLine("[CONFIG_ERROR] byte sizes must be within 1..1073741824 and batch <= max");
        return {};
    }
    if (config.timeout_seconds <= 0 || config.connect_timeout_seconds <= 0 ||
        config.start_delay_seconds < 0 || config.fault_delay_seconds < 0 ||
        config.recovery_delay_seconds < 0 || config.settle_seconds < 0 ||
        config.incomplete_grace_seconds < 0) {
        PrintLine("[CONFIG_ERROR] invalid timeout or delay");
        return {};
    }

    if (!complete_set) {
        config.expected_complete = config.runtime_case == RuntimeCase::Baseline ? 2 : 1;
    }
    if (!incomplete_set) {
        config.expected_incomplete =
            config.runtime_case == RuntimeCase::Backpressure ? 1 : 0;
    }
    if (config.expected_complete < 0 || config.expected_incomplete < 0) {
        PrintLine("[CONFIG_ERROR] expected counts cannot be negative");
        return {};
    }
    return {config, kExitPassed};
}

class Sha256Accumulator final {
public:
    Sha256Accumulator() {
#pragma warning(push)
#pragma warning(disable: 4996)
        SHA256_Init(&context_);
#pragma warning(pop)
    }

    void Update(const void* data, std::size_t size) {
        if (finished_ || size == 0) return;
#pragma warning(push)
#pragma warning(disable: 4996)
        SHA256_Update(&context_, data, size);
#pragma warning(pop)
    }

    std::string Finish() {
        if (!finished_) {
#pragma warning(push)
#pragma warning(disable: 4996)
            SHA256_Final(digest_.data(), &context_);
#pragma warning(pop)
            finished_ = true;
        }
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto byte : digest_) output << std::setw(2) << static_cast<int>(byte);
        return output.str();
    }

private:
    SHA256_CTX context_{};
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest_{};
    bool finished_ = false;
};

void FillBytes(std::vector<uint8_t>& buffer, std::size_t offset, uint32_t seed) {
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const uint64_t position = offset + i;
        buffer[i] = static_cast<uint8_t>((position * 131u + seed * 17u +
                                         (position >> 7u)) & 0xffu);
    }
}

std::vector<uint8_t> MakeBytes(std::size_t size, uint32_t seed) {
    std::vector<uint8_t> result(size);
    FillBytes(result, 0, seed);
    return result;
}

std::string MakeText(std::size_t size, uint32_t seed) {
    std::string result(size, 'a');
    for (std::size_t i = 0; i < size; ++i) {
        result[i] = static_cast<char>('a' + ((i + seed * 11u) % 26u));
    }
    return result;
}

std::string Sha256Of(const void* data, std::size_t size) {
    Sha256Accumulator hash;
    hash.Update(data, size);
    return hash.Finish();
}

std::string Sha256ForGeneratedBytes(std::size_t total,
                                    std::size_t batch_size,
                                    uint32_t seed) {
    Sha256Accumulator hash;
    std::size_t offset = 0;
    while (offset < total) {
        std::vector<uint8_t> batch(std::min(batch_size, total - offset));
        FillBytes(batch, offset, seed);
        hash.Update(batch.data(), batch.size());
        offset += batch.size();
    }
    return hash.Finish();
}

std::map<std::string, std::string> StreamAttributes(
    const Config& config,
    std::string kind,
    std::string sequence,
    std::size_t bytes,
    std::string sha256) {
    return {
        {"l3_run_id", config.run_id},
        {"l3_case", CaseName(config.runtime_case)},
        {"l3_kind", std::move(kind)},
        {"l3_sequence", std::move(sequence)},
        {"l3_bytes", std::to_string(bytes)},
        {"l3_sha256", std::move(sha256)},
    };
}

std::string AckTopic(const Config& config) {
    return config.topic + ".ack";
}

std::vector<uint8_t> AckPayload(const Config& config) {
    const std::string value = std::string("l3-ack-v1\n") +
        CaseName(config.runtime_case) + "\n" + config.run_id + "\n" +
        std::to_string(config.expected_complete) + "\n" +
        std::to_string(config.expected_incomplete);
    return {value.begin(), value.end()};
}

struct ReceivedResult {
    std::string kind;
    std::string stream_id;
    std::string sequence;
    std::string sender_identity;
    std::string close_reason;
    std::string actual_sha256;
    std::string expected_sha256;
    std::size_t actual_bytes = 0;
    std::size_t expected_bytes = 0;
    bool metadata_valid = false;
    bool sender_valid = false;
    bool complete = false;
    bool integrity_valid = false;
};

struct ReceiveSummary {
    std::size_t opened = 0;
    std::size_t finished = 0;
    std::size_t complete = 0;
    std::size_t incomplete = 0;
    std::size_t invalid = 0;
};

class RuntimeListener final : public livekit::RoomListener {
public:
    explicit RuntimeListener(Config config) : config_(std::move(config)) {}

    void OnConnected() override {
        connected_.fetch_add(1, std::memory_order_release);
        PrintLine("[STATE] connected");
    }

    void OnReconnecting() override {
        reconnecting_.fetch_add(1, std::memory_order_release);
        PrintLine("[STATE] reconnecting");
    }

    void OnReconnected() override {
        reconnected_.fetch_add(1, std::memory_order_release);
        PrintLine("[STATE] reconnected");
    }

    void OnDisconnected(livekit::RoomDisconnectReason reason,
                        const std::string&) override {
        disconnected_.fetch_add(1, std::memory_order_release);
        PrintLine("[STATE] disconnected reason=", static_cast<int>(reason));
    }

    void OnTextStreamOpened(
        std::shared_ptr<livekit::TextStreamReader> reader,
        std::shared_ptr<livekit::Participant>) override {
        if (!Matches(reader->info())) return;
        const auto info = reader->info();
        StartWorker([this, reader = std::move(reader), info]() mutable {
            Sha256Accumulator hash;
            std::size_t bytes = 0;
            std::string chunk;
            while (reader->ReadNext(chunk)) {
                hash.Update(chunk.data(), chunk.size());
                bytes += chunk.size();
            }
            Record(BuildResult("text", info, bytes, hash.Finish(),
                               reader->close_reason()));
        });
    }

    void OnByteStreamOpened(
        std::shared_ptr<livekit::ByteStreamReader> reader,
        std::shared_ptr<livekit::Participant>) override {
        if (!Matches(reader->info())) return;
        const auto info = reader->info();
        StartWorker([this, reader = std::move(reader), info]() mutable {
            Sha256Accumulator hash;
            std::size_t bytes = 0;
            std::vector<uint8_t> chunk;
            while (reader->ReadNext(chunk)) {
                hash.Update(chunk.data(), chunk.size());
                bytes += chunk.size();
            }
            Record(BuildResult("byte", info, bytes, hash.Finish(),
                               reader->close_reason()));
        });
    }

    void OnDataReceived(
        const std::vector<uint8_t>& payload,
        std::shared_ptr<livekit::RemoteParticipant> participant,
        const std::string& topic) override {
        if (config_.role != Role::Sender || topic != AckTopic(config_) ||
            payload != AckPayload(config_)) {
            return;
        }
        if (!participant ||
            (!config_.destination.empty() &&
             participant->identity() != config_.destination)) {
            return;
        }
        ack_received_.store(true, std::memory_order_release);
        PrintLine("[ACK] receiver_validation_received=true");
    }

    int reconnecting() const noexcept {
        return reconnecting_.load(std::memory_order_acquire);
    }

    int reconnected() const noexcept {
        return reconnected_.load(std::memory_order_acquire);
    }

    bool ack_received() const noexcept {
        return ack_received_.load(std::memory_order_acquire);
    }

    ReceiveSummary Summary() const {
        ReceiveSummary summary;
        summary.opened = opened_.load(std::memory_order_acquire);
        summary.invalid = worker_failures_.load(std::memory_order_acquire);
        std::lock_guard lock(results_mutex_);
        summary.finished = results_.size();
        for (const auto& result : results_) {
            if (!result.metadata_valid || !result.sender_valid ||
                (result.complete && !result.integrity_valid)) {
                ++summary.invalid;
            }
            if (result.complete) ++summary.complete;
            else ++summary.incomplete;
        }
        return summary;
    }

    std::vector<ReceivedResult> Results() const {
        std::lock_guard lock(results_mutex_);
        return results_;
    }

    void JoinWorkers() {
        for (;;) {
            std::vector<std::thread> workers;
            {
                std::lock_guard lock(workers_mutex_);
                workers.swap(workers_);
            }
            if (workers.empty()) break;
            for (auto& worker : workers) {
                if (worker.joinable()) worker.join();
            }
        }
    }

private:
    template <typename Info>
    bool Matches(const Info& info) const {
        const auto run = info.attributes.find("l3_run_id");
        return info.topic == config_.topic &&
               run != info.attributes.end() && run->second == config_.run_id;
    }

    void StartWorker(std::function<void()> worker) {
        opened_.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard lock(workers_mutex_);
        workers_.emplace_back([this, worker = std::move(worker)]() mutable {
            try {
                worker();
            } catch (...) {
                worker_failures_.fetch_add(1, std::memory_order_acq_rel);
                PrintLine("[ERROR] ",
                          livekit::secure_log::ExceptionSummary("receive_worker"));
            }
        });
    }

    template <typename Info>
    ReceivedResult BuildResult(const char* actual_kind,
                               const Info& info,
                               std::size_t actual_bytes,
                               std::string actual_hash,
                               std::string close_reason) const {
        ReceivedResult result;
        result.kind = actual_kind;
        result.stream_id = info.stream_id;
        result.sender_identity = info.sender_identity;
        result.close_reason = std::move(close_reason);
        result.actual_bytes = actual_bytes;
        result.actual_sha256 = std::move(actual_hash);
        result.complete = result.close_reason == "complete";
        result.sender_valid = config_.expected_sender.empty() ||
                              result.sender_identity == config_.expected_sender;

        const auto kind = info.attributes.find("l3_kind");
        const auto sequence = info.attributes.find("l3_sequence");
        const auto bytes = info.attributes.find("l3_bytes");
        const auto sha = info.attributes.find("l3_sha256");
        const auto case_name = info.attributes.find("l3_case");
        uint64_t expected_bytes = 0;
        result.metadata_valid = kind != info.attributes.end() &&
            kind->second == actual_kind && sequence != info.attributes.end() &&
            bytes != info.attributes.end() && sha != info.attributes.end() &&
            case_name != info.attributes.end() &&
            case_name->second == CaseName(config_.runtime_case) &&
            ParseUnsigned<uint64_t>(bytes->second, expected_bytes) &&
            expected_bytes <= kMaximumConfiguredBytes;
        if (sequence != info.attributes.end()) result.sequence = sequence->second;
        if (sha != info.attributes.end()) result.expected_sha256 = sha->second;
        result.expected_bytes = static_cast<std::size_t>(expected_bytes);
        result.integrity_valid = result.metadata_valid &&
            result.actual_bytes == result.expected_bytes &&
            result.actual_sha256 == result.expected_sha256;
        return result;
    }

    void Record(ReceivedResult result) {
        PrintLine("[STREAM] kind=", result.kind,
                  " sequence=", SafeOutput(result.sequence),
                  " bytes=", result.actual_bytes,
                  " complete=", result.complete ? "true" : "false",
                  " integrity=", result.integrity_valid ? "true" : "false",
                  " sender_valid=", result.sender_valid ? "true" : "false");
        std::lock_guard lock(results_mutex_);
        results_.push_back(std::move(result));
    }

    Config config_;
    std::atomic<int> connected_{0};
    std::atomic<int> reconnecting_{0};
    std::atomic<int> reconnected_{0};
    std::atomic<int> disconnected_{0};
    std::atomic<std::size_t> opened_{0};
    std::atomic<std::size_t> worker_failures_{0};
    std::atomic<bool> ack_received_{false};
    mutable std::mutex results_mutex_;
    std::vector<ReceivedResult> results_;
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
};

asio::awaitable<void> Delay(asio::any_io_executor executor, int seconds) {
    if (seconds <= 0) co_return;
    asio::steady_timer timer(executor, std::chrono::seconds(seconds));
    std::error_code error;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
}

template <typename Predicate>
asio::awaitable<bool> WaitUntil(asio::any_io_executor executor,
                                std::chrono::steady_clock::time_point deadline,
                                Predicate predicate) {
    asio::steady_timer timer(executor);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) co_return false;
        timer.expires_after(50ms);
        std::error_code error;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
    }
    co_return true;
}

asio::awaitable<bool> WaitForDestination(
    const std::shared_ptr<livekit::Room>& room,
    const Config& config) {
    if (config.destination.empty()) co_return true;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config.timeout_seconds);
    co_return co_await WaitUntil(room->executor(), deadline, [&] {
        const auto participants = room->remote_participants();
        return std::any_of(participants.begin(), participants.end(),
            [&](const auto& item) {
                return item.second && item.second->identity() == config.destination;
            });
    });
}

std::vector<std::string> Destinations(const Config& config) {
    if (config.destination.empty()) return {};
    return {config.destination};
}

void SendText(const std::shared_ptr<livekit::Room>& room,
              const Config& config,
              std::string sequence,
              std::size_t size,
              uint32_t seed) {
    auto payload = MakeText(size, seed);
    const auto hash = Sha256Of(payload.data(), payload.size());
    auto writer = room->CreateTextStreamWriter(
        config.topic,
        StreamAttributes(config, "text", sequence, payload.size(), hash),
        config.run_id + "-" + sequence,
        payload.size(),
        {},
        Destinations(config));
    writer->Write(payload);
    writer->Close("complete", {{"l3_result", "complete"}});
    PrintLine("[SEND] kind=text sequence=", sequence,
              " bytes=", payload.size(), " sha256=", hash);
}

void SendBytes(const std::shared_ptr<livekit::Room>& room,
               const Config& config,
               std::string sequence,
               std::size_t size,
               uint32_t seed) {
    auto payload = MakeBytes(size, seed);
    const auto hash = Sha256Of(payload.data(), payload.size());
    auto writer = room->CreateByteStreamWriter(
        sequence + ".bin",
        config.topic,
        StreamAttributes(config, "byte", sequence, payload.size(), hash),
        config.run_id + "-" + sequence,
        payload.size(),
        "application/octet-stream",
        Destinations(config));
    writer->Write(payload);
    writer->Close("complete", {{"l3_result", "complete"}});
    PrintLine("[SEND] kind=byte sequence=", sequence,
              " bytes=", payload.size(), " sha256=", hash);
}

struct ErrorFingerprint {
    livekit::OperationKind operation;
    livekit::OperationErrorCode code;
    std::string stage;
    bool retryable = false;
};

ErrorFingerprint Fingerprint(const livekit::OperationError& error) {
    return {error.operation(), error.code(), error.stage(), error.retryable()};
}

bool SameError(const ErrorFingerprint& expected,
               const livekit::OperationError& actual) {
    return expected.operation == actual.operation() &&
           expected.code == actual.code() &&
           expected.stage == actual.stage() &&
           expected.retryable == actual.retryable();
}

template <typename Action>
bool Rethrows(const ErrorFingerprint& expected, Action action) {
    try {
        action();
    } catch (const livekit::OperationError& error) {
        return SameError(expected, error);
    } catch (...) {
        return false;
    }
    return false;
}

asio::awaitable<int> RunBaseline(const std::shared_ptr<livekit::Room>& room,
                                 const Config& config) {
    SendText(room, config, "baseline-text", config.text_bytes, 11);
    SendBytes(room, config, "baseline-byte", config.byte_bytes, 23);
    co_return kExitPassed;
}

asio::awaitable<int> RunBackpressure(
    const std::shared_ptr<livekit::Room>& room,
    const Config& config) {
    constexpr uint32_t seed = 37;
    const auto expected_hash = Sha256ForGeneratedBytes(
        config.max_bytes, config.batch_bytes, seed);
    const std::string sequence = "backpressure-byte";
    auto writer = room->CreateByteStreamWriter(
        sequence + ".bin",
        config.topic,
        StreamAttributes(config, "byte", sequence,
                         config.max_bytes, expected_hash),
        config.run_id + "-" + sequence,
        config.max_bytes,
        "application/octet-stream",
        Destinations(config));

    std::size_t accepted_lower_bound = 0;
    uint64_t max_buffered = 0;
    std::optional<ErrorFingerprint> failure;
    std::vector<uint8_t> batch(std::min(config.batch_bytes, config.max_bytes));

    auto send_batch = [&](std::size_t offset) {
        batch.resize(std::min(config.batch_bytes, config.max_bytes - offset));
        FillBytes(batch, offset, seed);
        writer->Write(batch);
        accepted_lower_bound += batch.size();
        max_buffered = std::max(max_buffered,
            room->GetDataChannelBufferedAmount(true));
    };

    try {
        send_batch(0);
        PrintLine("[CONTROL] accepted_prefix=", accepted_lower_bound,
                  " apply_backpressure_now=true delay_sec=", config.fault_delay_seconds);
        co_await Delay(room->executor(), config.fault_delay_seconds);
        while (accepted_lower_bound < config.max_bytes) {
            send_batch(accepted_lower_bound);
            if ((accepted_lower_bound / config.batch_bytes) % 16 == 0) {
                PrintLine("[PROGRESS] accepted_lower_bound=", accepted_lower_bound,
                          " buffered_amount=", room->GetDataChannelBufferedAmount(true));
            }
        }
        writer->Close("complete", {{"l3_result", "complete"}});
    } catch (const livekit::OperationError& error) {
        failure = Fingerprint(error);
        PrintLine("[SEND_ERROR] operation=", OperationName(error.operation()),
                  " code=", ErrorCodeName(error.code()),
                  " stage=", error.stage(),
                  " accepted_lower_bound=", accepted_lower_bound,
                  " max_buffered_amount=", max_buffered);
    } catch (...) {
        PrintLine("[ERROR] ", livekit::secure_log::ExceptionSummary("backpressure_send"));
        co_return kExitFailed;
    }

    if (!failure) {
        PrintLine("[RESULT_DETAIL] backpressure_rejection_observed=false",
                  " accepted_lower_bound=", accepted_lower_bound,
                  " max_buffered_amount=", max_buffered);
        co_return kExitInconclusive;
    }
    if (accepted_lower_bound == 0) {
        PrintLine("[RESULT_DETAIL] accepted_prefix_observed=false");
        co_return kExitInconclusive;
    }
    if (failure->operation != livekit::OperationKind::SendData ||
        failure->code != livekit::OperationErrorCode::DataChannelRejected) {
        PrintLine("[RESULT_DETAIL] pure_backpressure=false code=",
                  ErrorCodeName(failure->code));
        co_return kExitInconclusive;
    }

    std::vector<uint8_t> retry{1, 2, 3};
    if (!Rethrows(*failure, [&] { writer->Write(retry); }) ||
        !Rethrows(*failure, [&] { writer->Close(); }) ||
        !Rethrows(*failure, [&] { writer->Cancel(); })) {
        PrintLine("[RESULT_DETAIL] stable_first_error=false");
        co_return kExitFailed;
    }
    PrintLine("[CHECK] stable_first_error=true no_retry_or_trailer=true");

    PrintLine("[CONTROL] remove_backpressure_now=true delay_sec=",
              config.recovery_delay_seconds);
    co_await Delay(room->executor(), config.recovery_delay_seconds);
    if (room->connection_state() != livekit::ConnectionState::Connected) {
        PrintLine("[RESULT_DETAIL] recovery_state=",
                  StateName(room->connection_state()));
        co_return kExitInconclusive;
    }
    SendText(room, config, "backpressure-recovery", 64 * 1024, 41);
    co_return kExitPassed;
}

asio::awaitable<int> RunSoftResume(
    const std::shared_ptr<livekit::Room>& room,
    const std::shared_ptr<RuntimeListener>& listener,
    const Config& config) {
    const auto payload = MakeText(config.text_bytes, 53);
    const auto hash = Sha256Of(payload.data(), payload.size());
    const auto attributes = StreamAttributes(
        config, "text", "soft-survivor", payload.size(), hash);
    auto survivor = room->CreateTextStreamWriter(
        config.topic, attributes, config.run_id + "-soft-survivor",
        payload.size(), {}, Destinations(config));
    auto doomed = room->CreateTextStreamWriter(
        config.topic,
        StreamAttributes(config, "text", "soft-doomed", 1,
                         Sha256Of("x", 1)),
        config.run_id + "-soft-doomed", 1, {}, Destinations(config));

    const int reconnected_before = listener->reconnected();
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::SignalReconnect);
    if (room->connection_state() != livekit::ConnectionState::Reconnecting) {
        PrintLine("[RESULT_DETAIL] soft_resume_pause_not_observed state=",
                  StateName(room->connection_state()));
        co_return kExitInconclusive;
    }

    std::optional<ErrorFingerprint> failed_during_resume;
    try {
        doomed->Write("x");
    } catch (const livekit::OperationError& error) {
        failed_during_resume = Fingerprint(error);
        PrintLine("[SEND_ERROR] operation=", OperationName(error.operation()),
                  " code=", ErrorCodeName(error.code()),
                  " stage=", error.stage());
    } catch (...) {
        PrintLine("[ERROR] ", livekit::secure_log::ExceptionSummary("soft_resume_send"));
        co_return kExitFailed;
    }
    if (!failed_during_resume ||
        failed_during_resume->operation != livekit::OperationKind::SendData ||
        failed_during_resume->code != livekit::OperationErrorCode::SessionInvalid) {
        PrintLine("[RESULT_DETAIL] recovery_send_not_rejected=true");
        co_return kExitFailed;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config.timeout_seconds);
    const bool recovered = co_await WaitUntil(room->executor(), deadline, [&] {
        return room->connection_state() == livekit::ConnectionState::Connected &&
               listener->reconnected() > reconnected_before;
    });
    if (!recovered) {
        PrintLine("[RESULT_DETAIL] soft_resume_timeout=true");
        co_return kExitInconclusive;
    }
    if (!Rethrows(*failed_during_resume, [&] { doomed->Close(); }) ||
        !Rethrows(*failed_during_resume, [&] { doomed->Cancel(); })) {
        PrintLine("[RESULT_DETAIL] failed_writer_revived=true");
        co_return kExitFailed;
    }

    survivor->Write(payload);
    survivor->Close("complete", {{"l3_result", "complete"}});
    PrintLine("[SEND] kind=text sequence=soft-survivor bytes=",
              payload.size(), " sha256=", hash);
    co_return kExitPassed;
}

asio::awaitable<int> RunFullRestart(
    const std::shared_ptr<livekit::Room>& room,
    const std::shared_ptr<RuntimeListener>& listener,
    const Config& config) {
    auto stale = room->CreateTextStreamWriter(
        config.topic,
        StreamAttributes(config, "text", "full-stale", 1, Sha256Of("x", 1)),
        config.run_id + "-full-stale", 1, {}, Destinations(config));
    const int reconnected_before = listener->reconnected();
    co_await room->SimulateScenarioAsync(livekit::SimulateScenarioType::FullReconnect);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config.timeout_seconds);
    const bool recovered = co_await WaitUntil(room->executor(), deadline, [&] {
        return room->connection_state() == livekit::ConnectionState::Connected &&
               listener->reconnected() > reconnected_before;
    });
    if (!recovered) {
        PrintLine("[RESULT_DETAIL] full_restart_timeout=true");
        co_return kExitInconclusive;
    }

    std::optional<ErrorFingerprint> stale_error;
    try {
        stale->Write("x");
    } catch (const livekit::OperationError& error) {
        stale_error = Fingerprint(error);
        PrintLine("[SEND_ERROR] operation=", OperationName(error.operation()),
                  " code=", ErrorCodeName(error.code()),
                  " stage=", error.stage());
    } catch (...) {
        PrintLine("[ERROR] ", livekit::secure_log::ExceptionSummary("full_restart_stale"));
        co_return kExitFailed;
    }
    if (!stale_error || stale_error->operation != livekit::OperationKind::SendData ||
        stale_error->code != livekit::OperationErrorCode::SessionInvalid) {
        PrintLine("[RESULT_DETAIL] server_did_not_full_restart=true");
        co_return kExitInconclusive;
    }
    if (!Rethrows(*stale_error, [&] { stale->Close(); })) {
        PrintLine("[RESULT_DETAIL] stale_writer_close_not_isolated=true");
        co_return kExitFailed;
    }

    SendText(room, config, "full-fresh", config.text_bytes, 67);
    co_return kExitPassed;
}

asio::awaitable<int> RunSender(
    const std::shared_ptr<livekit::Room>& room,
    const std::shared_ptr<RuntimeListener>& listener,
    const Config& config) {
    co_await Delay(room->executor(), config.start_delay_seconds);
    if (!co_await WaitForDestination(room, config)) {
        PrintLine("[RESULT_DETAIL] destination_not_connected=true");
        co_return kExitInconclusive;
    }

    int result = kExitFailed;
    switch (config.runtime_case) {
    case RuntimeCase::Baseline:
        result = co_await RunBaseline(room, config);
        break;
    case RuntimeCase::Backpressure:
        result = co_await RunBackpressure(room, config);
        break;
    case RuntimeCase::SoftResume:
        result = co_await RunSoftResume(room, listener, config);
        break;
    case RuntimeCase::FullRestart:
        result = co_await RunFullRestart(room, listener, config);
        break;
    }
    if (result != kExitPassed) co_return result;

    const auto ack_deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(config.timeout_seconds);
    if (!co_await WaitUntil(room->executor(), ack_deadline, [&] {
            return listener->ack_received();
        })) {
        PrintLine("[RESULT_DETAIL] receiver_ack_timeout=true");
        co_return kExitInconclusive;
    }
    co_return kExitPassed;
}

bool ReceiverReady(const ReceiveSummary& summary, const Config& config) {
    if (summary.invalid != 0) return true;
    const std::size_t total = static_cast<std::size_t>(config.expected_complete) +
                              static_cast<std::size_t>(config.expected_incomplete);
    if (config.expected_incomplete > 0) {
        return summary.opened >= total &&
               summary.complete >= static_cast<std::size_t>(config.expected_complete);
    }
    return summary.finished >= total;
}

int ValidateReceiver(const RuntimeListener& listener, const Config& config) {
    const auto summary = listener.Summary();
    const std::size_t expected_total =
        static_cast<std::size_t>(config.expected_complete) +
        static_cast<std::size_t>(config.expected_incomplete);
    PrintLine("[RECEIVE_SUMMARY] opened=", summary.opened,
              " finished=", summary.finished,
              " complete=", summary.complete,
              " incomplete=", summary.incomplete,
              " invalid=", summary.invalid);
    if (summary.opened != expected_total || summary.finished != expected_total ||
        summary.complete != static_cast<std::size_t>(config.expected_complete) ||
        summary.incomplete != static_cast<std::size_t>(config.expected_incomplete) ||
        summary.invalid != 0) {
        return kExitFailed;
    }
    return kExitPassed;
}

bool ReceiverCanAcknowledge(const ReceiveSummary& summary,
                            const Config& config) {
    const std::size_t expected_complete =
        static_cast<std::size_t>(config.expected_complete);
    const std::size_t expected_incomplete =
        static_cast<std::size_t>(config.expected_incomplete);
    const std::size_t expected_total = expected_complete + expected_incomplete;
    if (summary.invalid != 0 || summary.opened != expected_total ||
        summary.complete != expected_complete) {
        return false;
    }
    if (expected_incomplete == 0) {
        return summary.finished == expected_total && summary.incomplete == 0;
    }
    return summary.finished <= expected_total &&
           summary.incomplete <= expected_incomplete;
}

asio::awaitable<int> RunReceiver(
    const std::shared_ptr<livekit::Room>& room,
    const std::shared_ptr<RuntimeListener>& listener,
    const Config& config) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config.timeout_seconds);
    const bool ready = co_await WaitUntil(room->executor(), deadline, [&] {
        return ReceiverReady(listener->Summary(), config);
    });
    if (!ready) {
        PrintLine("[RESULT_DETAIL] receiver_timeout=true");
        co_return kExitInconclusive;
    }
    if (config.expected_incomplete > 0) {
        co_await Delay(room->executor(), config.incomplete_grace_seconds);
    }
    const auto summary = listener->Summary();
    if (!ReceiverCanAcknowledge(summary, config)) {
        PrintLine("[RESULT_DETAIL] receiver_ack_validation_failed=true");
        co_return kExitFailed;
    }

    const auto ack = AckPayload(config);
    if (!room->PublishData(ack, /*reliable=*/true,
                           {config.expected_sender}, AckTopic(config))) {
        PrintLine("[RESULT_DETAIL] receiver_ack_send_failed=true");
        co_return kExitInconclusive;
    }
    PrintLine("[ACK] receiver_validation_sent=true");
    co_await Delay(room->executor(), std::max(1, config.settle_seconds));
    co_return kExitPassed;
}

asio::awaitable<int> RunHarness(Config config,
                                asio::any_io_executor executor) {
    auto room = livekit::Room::Create(executor);
    auto listener = std::make_shared<RuntimeListener>(config);
    room->AddListener(listener);
    int result = kExitFailed;

    try {
        livekit::SignalOptions options;
        options.auto_subscribe = true;
        options.single_peer_connection = true;
        options.connect_timeout = std::chrono::seconds(config.connect_timeout_seconds);
        options.timeouts.reconnect_total = std::chrono::seconds(config.timeout_seconds);

        PrintLine("[CONFIG] role=", RoleName(config.role),
                  " case=", CaseName(config.runtime_case),
                  " run_id=", SafeOutput(config.run_id),
                  " endpoint=", livekit::secure_log::EndpointSummary(config.url));
        co_await room->ConnectAsync(config.url, config.token, options);
        PrintLine("[SESSION] local_identity=",
                  room->local_participant()
                      ? SafeOutput(room->local_participant()->identity())
                      : "unavailable");

        if (config.role == Role::Sender) {
            result = co_await RunSender(room, listener, config);
            co_await Delay(executor, config.settle_seconds);
        } else {
            result = co_await RunReceiver(room, listener, config);
        }
    } catch (const RuntimeFailure& error) {
        PrintLine("[ERROR] runtime_code=", error.code());
        result = kExitFailed;
    } catch (const livekit::OperationError& error) {
        PrintLine("[ERROR] operation=", OperationName(error.operation()),
                  " code=", ErrorCodeName(error.code()),
                  " stage=", error.stage());
        result = kExitFailed;
    } catch (...) {
        PrintLine("[ERROR] ", livekit::secure_log::ExceptionSummary("runtime_harness"));
        result = kExitFailed;
    }

    room->Disconnect();
    listener->JoinWorkers();
    if (config.role == Role::Receiver && result == kExitPassed) {
        result = ValidateReceiver(*listener, config);
    } else if (config.role == Role::Sender) {
        const auto inbound = listener->Summary();
        if (inbound.opened != 0) {
            PrintLine("[RESULT_DETAIL] sender_received_own_stream=true count=",
                      inbound.opened);
            result = kExitFailed;
        }
    }

    const char* status = result == kExitPassed ? "PASS" :
        result == kExitInconclusive ? "INCONCLUSIVE" : "FAIL";
    PrintLine("[RESULT] status=", status,
              " role=", RoleName(config.role),
              " case=", CaseName(config.runtime_case),
              " run_id=", SafeOutput(config.run_id));
    co_return result;
}

} // namespace

int main(int argc, char** argv) {
    const auto parsed = ParseArguments(argc, argv);
    if (!parsed.config) return parsed.exit_code;

    asio::io_context io;
    auto completed = asio::co_spawn(
        io,
        RunHarness(*parsed.config, io.get_executor()),
        asio::use_future);
    io.run();
    try {
        return completed.get();
    } catch (...) {
        PrintLine("[ERROR] ", livekit::secure_log::ExceptionSummary("runtime_main"));
        return kExitFailed;
    }
}
