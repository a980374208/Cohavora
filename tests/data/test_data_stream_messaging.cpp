// Phase2A1 delivered test copy; original input remains immutable and untracked.
// Original: tests/test_data_stream_messaging.cpp
// Original SHA256: 04af3c4625291638be7a52347b3c9e4a4cc64db71a30d2fa3d60367a91ad12db
// Provenance and exact adaptations: tests/remediation/restored/PROVENANCE.json
// Adaptation: provenance comments only; original active checks retained.

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>

#include "data_stream.h"
#include "livekit_models.pb.h"

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed: " #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while(0)

namespace {

void TestTextStreamSlicingAndReading() {
    std::cout << "[TestTextStreamSlicingAndReading] Starting..." << std::endl;

    // 1. 准备测试数据：40,000 字节的文本，预计分为 15000 + 15000 + 10000 三个 chunks
    std::string full_text;
    full_text.reserve(40000);
    for (size_t i = 0; i < 40000; ++i) {
        full_text.push_back(static_cast<char>('a' + (i % 26)));
    }

    std::vector<livekit::proto::DataPacket> delivered_packets;
    auto publisher = [&](const livekit::proto::DataPacket& pkt, bool reliable) -> bool {
        TEST_ASSERT(reliable);
        delivered_packets.push_back(pkt);
        return true;
    };

    std::map<std::string, std::string> initial_attrs = {{"key1", "val1"}};
    livekit::TextStreamWriter writer(publisher, "chat-topic", initial_attrs, "text_stream_01",
                                     full_text.size(), "reply_msg_123", {}, "alice");

    // 写入数据并关闭
    writer.Write(full_text);
    writer.Close("", {{"final_attr", "done"}});

    // 验证发出的报文结构：1 Header + 3 Chunks + 1 Trailer = 5 报文
    TEST_ASSERT(delivered_packets.size() == 5);
    TEST_ASSERT(delivered_packets[0].has_stream_header());
    TEST_ASSERT(delivered_packets[1].has_stream_chunk());
    TEST_ASSERT(delivered_packets[2].has_stream_chunk());
    TEST_ASSERT(delivered_packets[3].has_stream_chunk());
    TEST_ASSERT(delivered_packets[4].has_stream_trailer());

    // 验证 Header 字段
    const auto& header = delivered_packets[0].stream_header();
    TEST_ASSERT(header.stream_id() == "text_stream_01");
    TEST_ASSERT(header.topic() == "chat-topic");
    TEST_ASSERT(header.total_length() == 40000);
    TEST_ASSERT(header.text_header().reply_to_stream_id() == "reply_msg_123");
    TEST_ASSERT(header.attributes().at("key1") == "val1");

    // 2. 模拟 Reader 消费
    livekit::TextStreamInfo info;
    info.stream_id = header.stream_id();
    info.topic = header.topic();
    info.mime_type = header.mime_type();
    info.timestamp = header.timestamp();
    info.total_length = header.total_length();
    for (const auto& [k, v] : header.attributes()) {
        info.attributes[k] = v;
    }
    info.reply_to_stream_id = header.text_header().reply_to_stream_id();

    livekit::TextStreamReader reader(std::move(info));
    TEST_ASSERT(!reader.is_closed());

    // 投递 3 个 chunks
    for (size_t i = 1; i <= 3; ++i) {
        reader.OnChunkUpdate(delivered_packets[i].stream_chunk().content());
    }

    // 投递 trailer
    std::map<std::string, std::string> trailer_attrs;
    for (const auto& [k, v] : delivered_packets[4].stream_trailer().attributes()) {
        trailer_attrs[k] = v;
    }
    reader.OnStreamClose(delivered_packets[4].stream_trailer().reason(), trailer_attrs);

    TEST_ASSERT(reader.is_closed());
    TEST_ASSERT(reader.close_reason().empty());
    TEST_ASSERT(reader.info().attributes.at("final_attr") == "done");

    // 读取全部内容并校验完全一致
    std::string received_text = reader.ReadAll();
    TEST_ASSERT(received_text.size() == full_text.size());
    TEST_ASSERT(received_text == full_text);

    std::cout << "[TestTextStreamSlicingAndReading] Passed!" << std::endl;
}

void TestUtf8AwareTextSlicing() {
    std::string full_text(livekit::kStreamChunkSize - 1, 'a');
    full_text.append("\xE4\xB8\xAD", 3);

    std::vector<livekit::proto::DataPacket> delivered_packets;
    auto publisher = [&](const livekit::proto::DataPacket& packet,
                         bool reliable) -> bool {
        TEST_ASSERT(reliable);
        delivered_packets.push_back(packet);
        return true;
    };

    livekit::TextStreamWriter writer(
        publisher, "utf8-topic", {}, "utf8-boundary", full_text.size());
    writer.Write(full_text);
    writer.Close();

    TEST_ASSERT(delivered_packets.size() == 4);
    TEST_ASSERT(delivered_packets[1].stream_chunk().content().size() ==
                livekit::kStreamChunkSize - 1);
    TEST_ASSERT(delivered_packets[2].stream_chunk().content().size() == 3);

    livekit::TextStreamInfo info;
    info.stream_id = "utf8-boundary";
    info.total_length = full_text.size();
    livekit::TextStreamReader reader(std::move(info));
    reader.OnChunkUpdate(delivered_packets[1].stream_chunk().content());
    reader.OnChunkUpdate(delivered_packets[2].stream_chunk().content());
    reader.OnStreamClose("", {});
    TEST_ASSERT(reader.ReadAll() == full_text);
}

void TestByteStreamBinaryTransfer() {
    std::cout << "[TestByteStreamBinaryTransfer] Starting..." << std::endl;

    // 准备 50,000 字节的二进制数据
    std::vector<uint8_t> binary_data(50000);
    for (size_t i = 0; i < binary_data.size(); ++i) {
        binary_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    std::vector<livekit::proto::DataPacket> delivered_packets;
    auto publisher = [&](const livekit::proto::DataPacket& pkt, bool reliable) -> bool {
        TEST_ASSERT(reliable);
        delivered_packets.push_back(pkt);
        return true;
    };

    livekit::ByteStreamWriter writer(publisher, "presentation.pdf", "files", {}, "byte_stream_01",
                                     binary_data.size(), "application/pdf", {}, "bob");

    writer.Write(binary_data);
    writer.Close();

    // 50000 字节按 15000 分块：15000 + 15000 + 15000 + 5000 = 4 chunks
    // 总包数：1 Header + 4 Chunks + 1 Trailer = 6
    TEST_ASSERT(delivered_packets.size() == 6);
    TEST_ASSERT(delivered_packets[0].has_stream_header());
    TEST_ASSERT(delivered_packets[0].stream_header().byte_header().name() == "presentation.pdf");
    TEST_ASSERT(delivered_packets[0].stream_header().mime_type() == "application/pdf");

    // 构造 Reader 并验证增量进度
    livekit::ByteStreamInfo info;
    info.stream_id = delivered_packets[0].stream_header().stream_id();
    info.name = delivered_packets[0].stream_header().byte_header().name();
    info.mime_type = delivered_packets[0].stream_header().mime_type();
    info.total_length = binary_data.size();

    livekit::ByteStreamReader reader(std::move(info));
    TEST_ASSERT(reader.received_bytes() == 0);

    // 逐 chunk 送入并检查进度
    size_t accumulated = 0;
    for (size_t i = 1; i <= 4; ++i) {
        const auto& c = delivered_packets[i].stream_chunk().content();
        reader.OnChunkUpdate(reinterpret_cast<const uint8_t*>(c.data()), c.size());
        accumulated += c.size();
        TEST_ASSERT(reader.received_bytes() == accumulated);
    }
    reader.OnStreamClose("", {});
    TEST_ASSERT(reader.is_closed());

    std::vector<uint8_t> reconstructed = reader.ReadAll();
    TEST_ASSERT(reconstructed.size() == binary_data.size());
    TEST_ASSERT(reconstructed == binary_data);

    std::cout << "[TestByteStreamBinaryTransfer] Passed!" << std::endl;
}

void TestStreamCancellationAndInterruption() {
    std::cout << "[TestStreamCancellationAndInterruption] Starting..." << std::endl;

    std::vector<livekit::proto::DataPacket> delivered_packets;
    auto publisher = [&](const livekit::proto::DataPacket& pkt, bool reliable) -> bool {
        delivered_packets.push_back(pkt);
        return true;
    };

    livekit::TextStreamWriter writer(publisher, "cancel-topic", {}, "stream_to_cancel");
    writer.Write("Hello World, Part 1");
    // 主动取消传输
    writer.Cancel("network_abort");

    TEST_ASSERT(writer.is_closed());
    // 尝试在取消后写入，应当被忽略
    writer.Write("Ignored Part");

    // 报文结构应为 Header, Chunk, Trailer (reason = network_abort)
    TEST_ASSERT(delivered_packets.size() == 3);
    TEST_ASSERT(delivered_packets[2].has_stream_trailer());
    TEST_ASSERT(delivered_packets[2].stream_trailer().reason() == "network_abort");

    // 接收端 Reader
    livekit::TextStreamInfo info;
    info.stream_id = "stream_to_cancel";
    livekit::TextStreamReader reader(std::move(info));
    reader.OnChunkUpdate(delivered_packets[1].stream_chunk().content());
    reader.OnStreamClose("network_abort", {});

    TEST_ASSERT(reader.is_closed());
    TEST_ASSERT(reader.is_failed());
    TEST_ASSERT(reader.close_reason() == "network_abort");

    std::string part;
    TEST_ASSERT(reader.ReadNext(part));
    TEST_ASSERT(part == "Hello World, Part 1");
    // 下一次读取应返回 false
    TEST_ASSERT(!reader.ReadNext(part));

    std::cout << "[TestStreamCancellationAndInterruption] Passed!" << std::endl;
}

void TestConcurrentStreamsIsolation() {
    std::cout << "[TestConcurrentStreamsIsolation] Starting..." << std::endl;

    // 创建两个不同的 Reader
    livekit::TextStreamInfo info_a;
    info_a.stream_id = "stream_A";
    livekit::TextStreamReader reader_a(info_a);

    livekit::TextStreamInfo info_b;
    info_b.stream_id = "stream_B";
    livekit::TextStreamReader reader_b(info_b);

    // 交错投递 Chunks
    reader_a.OnChunkUpdate("A1-");
    reader_b.OnChunkUpdate("B1-");
    reader_a.OnChunkUpdate("A2");
    reader_b.OnChunkUpdate("B2");

    reader_a.OnStreamClose("", {});
    reader_b.OnStreamClose("", {});

    TEST_ASSERT(reader_a.ReadAll() == "A1-A2");
    TEST_ASSERT(reader_b.ReadAll() == "B1-B2");

    std::cout << "[TestConcurrentStreamsIsolation] Passed!" << std::endl;
}

} // namespace

int main() {
    std::cout << "Running test_data_stream_messaging..." << std::endl;
    TestTextStreamSlicingAndReading();
    TestUtf8AwareTextSlicing();
    TestByteStreamBinaryTransfer();
    TestStreamCancellationAndInterruption();
    TestConcurrentStreamsIsolation();
    std::cout << "All test_data_stream_messaging tests PASSED!" << std::endl;
    return 0;
}
