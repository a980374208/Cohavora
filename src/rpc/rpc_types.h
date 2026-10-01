#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <exception>
#include <asio.hpp>
#include <nlohmann/json.hpp>

namespace livekit {

enum class RpcErrorCode {
    APPLICATION_ERROR = 1,
    NETWORK_ERROR = 2,
    TIMEOUT = 3,
    UNSUPPORTED_METHOD = 4,
    REJECTED = 5,
    RESPONSE_PAYLOAD_TOO_LARGE = 6,
    REQUEST_PAYLOAD_TOO_LARGE = 7,
    UNSUPPORTED_VERSION = 8,
    CONNECTION_TIMEOUT = 9
};

class RpcError : public std::exception {
public:
    RpcError(RpcErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {
        full_msg_ = "RpcError [" + std::to_string(static_cast<int>(code_)) + "]: " + message_;
    }

    RpcErrorCode code() const { return code_; }
    const std::string& message() const { return message_; }
    const char* what() const noexcept override { return full_msg_.c_str(); }

private:
    RpcErrorCode code_;
    std::string message_;
    std::string full_msg_;
};

struct RpcInvocationData {
    std::string request_id;
    std::string caller_identity;
    std::string payload;
    double response_timeout_sec = 15.0;
};

using RpcHandler = std::function<asio::awaitable<std::string>(const RpcInvocationData&)>;

enum class RpcPacketType {
    Request,
    Response,
    Ack
};

enum class RpcWireFormat { ProtobufV1, LegacyJson, StreamV2 };

struct RpcPacket {
    static constexpr size_t kMaxPayloadBytes = 15360;
    RpcWireFormat wire_format = RpcWireFormat::ProtobufV1;
    uint32_t version = 1;
    bool unsupported_encoding = false;
    static int ToWireError(int code) {
        switch (static_cast<RpcErrorCode>(code)) {
        case RpcErrorCode::APPLICATION_ERROR: return 1500;
        case RpcErrorCode::NETWORK_ERROR: return 1505;
        case RpcErrorCode::TIMEOUT: return 1502;
        case RpcErrorCode::UNSUPPORTED_METHOD: return 1400;
        case RpcErrorCode::RESPONSE_PAYLOAD_TOO_LARGE: return 1504;
        case RpcErrorCode::REQUEST_PAYLOAD_TOO_LARGE: return 1402;
        case RpcErrorCode::UNSUPPORTED_VERSION: return 1404;
        case RpcErrorCode::CONNECTION_TIMEOUT: return 1501;
        default: return code;
        }
    }
    static int FromWireError(int code) {
        for (int local : {1, 2, 3, 4, 6, 7, 8, 9})
            if (ToWireError(local) == code) return local;
        return code;
    }
    RpcPacketType type = RpcPacketType::Request;
    std::string request_id;
    std::string method;
    std::string payload;
    std::string caller_identity;
    std::string destination_identity;
    double timeout_sec = 15.0;

    bool has_error = false;
    int error_code = 0;
    std::string error_message;

    std::string Encode() const {
        nlohmann::json j;
        j["type"] = (type == RpcPacketType::Request) ? "request" : "response";
        j["request_id"] = request_id;
        j["method"] = method;
        j["payload"] = payload;
        j["caller_identity"] = caller_identity;
        j["destination_identity"] = destination_identity;
        j["timeout_sec"] = timeout_sec;
        if (has_error) {
            j["error"] = {
                {"code", error_code},
                {"message", error_message}
            };
        }
        return j.dump();
    }

    static std::optional<RpcPacket> Decode(const std::string& json_str) {
        try {
            auto j = nlohmann::json::parse(json_str);
            RpcPacket packet;
            packet.wire_format = RpcWireFormat::LegacyJson;
            std::string type_str = j.value("type", "request");
            packet.type = (type_str == "response") ? RpcPacketType::Response : RpcPacketType::Request;
            packet.request_id = j.value("request_id", "");
            packet.method = j.value("method", "");
            packet.payload = j.value("payload", "");
            packet.caller_identity = j.value("caller_identity", "");
            packet.destination_identity = j.value("destination_identity", "");
            packet.timeout_sec = j.value("timeout_sec", 15.0);

            if (j.contains("error") && j["error"].is_object()) {
                packet.has_error = true;
                packet.error_code = j["error"].value("code", 1);
                packet.error_message = j["error"].value("message", "Unknown RPC Error");
            }
            return packet;
        } catch (...) {
            return std::nullopt;
        }
    }
};

} // namespace livekit
