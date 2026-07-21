#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace livekit {

struct ChatMessage {
    std::string id;
    int64_t timestamp = 0; // 发送毫秒时间戳
    std::optional<int64_t> edit_timestamp; // 修改毫秒时间戳（若已修改）
    std::string message; // 消息内容
    std::string sender_identity; // 发送者 Identity
    std::vector<std::string> destination_identities; // 指定接收者（若为空则广播给全房间）
    bool is_deleted = false;

    // 序列化为 LiveKit lk.chat JSON 格式
    std::string Encode() const {
        nlohmann::json j;
        j["id"] = id;
        j["timestamp"] = timestamp;
        j["message"] = message;
        if (edit_timestamp.has_value()) {
            j["edit_timestamp"] = edit_timestamp.value();
        }
        if (!destination_identities.empty()) {
            j["destination_identities"] = destination_identities;
        }
        if (is_deleted) {
            j["is_deleted"] = true;
        }
        return j.dump();
    }

    // 从 LiveKit lk.chat JSON 格式反序列化
    static std::optional<ChatMessage> Decode(const std::string& payload, const std::string& sender = "") {
        try {
            auto j = nlohmann::json::parse(payload);
            ChatMessage msg;
            msg.id = j.value("id", "");
            msg.timestamp = j.value("timestamp", static_cast<int64_t>(0));
            msg.message = j.value("message", "");
            msg.sender_identity = sender;
            if (j.contains("edit_timestamp") && !j["edit_timestamp"].is_null()) {
                msg.edit_timestamp = j["edit_timestamp"].get<int64_t>();
            }
            if (j.contains("destination_identities") && j["destination_identities"].is_array()) {
                msg.destination_identities = j["destination_identities"].get<std::vector<std::string>>();
            }
            msg.is_deleted = j.value("is_deleted", false);
            return msg;
        } catch (...) {
            return std::nullopt;
        }
    }
};

} // namespace livekit
