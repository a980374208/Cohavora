#include <iostream>
#include <cassert>
#include <string>
#include <memory>
#include <asio.hpp>
#include "room.h"
#include "participant.h"
#include "chat_message.h"

class TestAdvancedListener : public livekit::RoomListener {
public:
    bool chat_received = false;
    livekit::ChatMessage last_chat_msg;
    bool backpressure_changed = false;
    uint64_t last_low_amount = 0;
    bool last_reliable = false;

    void OnChatMessage(const livekit::ChatMessage& message, std::shared_ptr<livekit::Participant> participant) override {
        chat_received = true;
        last_chat_msg = message;
        std::cout << "[Listener] Received ChatMessage id=" << message.id
                  << " text='" << message.message << "'"
                  << " sender='" << message.sender_identity << "'" << std::endl;
    }

    void OnDataChannelBufferedAmountLowThresholdChanged(uint64_t amount, bool reliable) override {
        backpressure_changed = true;
        last_low_amount = amount;
        last_reliable = reliable;
        std::cout << "[Listener] DataChannel Backpressure Low Threshold Changed amount=" << amount
                  << " reliable=" << (reliable ? "true" : "false") << std::endl;
    }
};

int main() {
    std::cout << "==================================================\n";
    std::cout << " Running Advanced Communication & Flow Control Tests \n";
    std::cout << "==================================================\n";

    asio::io_context io_ctx;
    auto room = livekit::Room::Create(io_ctx.get_executor());
    auto listener = std::make_shared<TestAdvancedListener>();
    room->AddListener(listener);

    // 1. 测试 ChatMessage 结构体 Encode / Decode 编解码
    std::cout << "[Test 1] Testing ChatMessage Encode & Decode..." << std::endl;
    livekit::ChatMessage original_msg;
    original_msg.id = "chat_test_12345";
    original_msg.timestamp = 1700000000000;
    original_msg.message = "Hello LiveKit AI Assistant!";
    original_msg.sender_identity = "user_agent_007";
    original_msg.destination_identities = {"agent_pi_1"};

    std::string encoded_str = original_msg.Encode();
    auto decoded_opt = livekit::ChatMessage::Decode(encoded_str, "user_agent_007");

    assert(decoded_opt.has_value() && "ChatMessage Decode failed!");
    assert(decoded_opt->id == original_msg.id);
    assert(decoded_opt->message == original_msg.message);
    assert(decoded_opt->destination_identities.size() == 1);
    assert(decoded_opt->destination_identities[0] == "agent_pi_1");
    std::cout << "[Test 1 PASSED] ChatMessage encoded JSON: " << encoded_str << "\n" << std::endl;

    // 2. 测试 LocalParticipant::SendChatMessage 与 RoomListener::OnChatMessage 分发
    std::cout << "[Test 2] Testing LocalParticipant SendChatMessage & EditChatMessage..." << std::endl;
    
    // 构造模拟已连接的 LocalParticipant
    auto local_p = std::make_shared<livekit::LocalParticipant>(
        "PA_local_123", "user_alice",
        [](const livekit::proto::SignalRequest&) {}
    );
    local_p->SetPublishDataHandler([room](const std::vector<uint8_t>& payload, bool reliable, const std::vector<std::string>& dest, const std::string& topic) {
        room->OnIncomingDataPacket(payload, "user_alice", topic);
    });

    auto sent_chat = local_p->SendChatMessage("Testing SendChatMessage API");
    assert(listener->chat_received && "RoomListener should have received OnChatMessage!");
    assert(listener->last_chat_msg.id == sent_chat.id);
    assert(listener->last_chat_msg.message == "Testing SendChatMessage API");
    std::cout << "[Test 2.1 PASSED] SendChatMessage dispatched correctly!" << std::endl;

    // 测试编辑已有 Chat 消息
    listener->chat_received = false;
    auto edited_chat = local_p->EditChatMessage("Testing EditChatMessage API (Updated)", sent_chat.id);
    assert(listener->chat_received);
    assert(listener->last_chat_msg.id == sent_chat.id);
    assert(listener->last_chat_msg.message == "Testing EditChatMessage API (Updated)");
    assert(listener->last_chat_msg.edit_timestamp.has_value());
    std::cout << "[Test 2.2 PASSED] EditChatMessage edited message successfully!\n" << std::endl;

    // 3. 测试 DataChannel 缓冲背压控制 (BufferedAmountLowThreshold)
    std::cout << "[Test 3] Testing DataChannel Backpressure Flow Control..." << std::endl;
    room->SetDataChannelBufferedAmountLowThreshold(32768, /*reliable=*/true);
    assert(room->GetDataChannelBufferedAmount(/*reliable=*/true) == 0);

    // 触发背压水线变动通知
    room->OnDataChannelBufferedAmountLow(65536, /*reliable=*/true);
    assert(listener->backpressure_changed && "OnDataChannelBufferedAmountLowThresholdChanged listener should be notified!");
    assert(listener->last_reliable == true);
    std::cout << "[Test 3 PASSED] DataChannel backpressure low threshold notification triggered successfully!\n" << std::endl;

    std::cout << "==================================================\n";
    std::cout << " ALL ADVANCED COMMUNICATION TESTS PASSED 100%! \n";
    std::cout << "==================================================\n";

    return 0;
}
