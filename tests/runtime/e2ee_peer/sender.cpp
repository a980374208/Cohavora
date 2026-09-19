#include "livekit/livekit.h"
#include "livekit/data_stream.h"
#include "livekit/room_delegate.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr auto kTopic = "lk.l3.e2ee";
constexpr auto kTextHash = "065185be71990d5579218c42b67f4c347e2681daf7640b4be69f44e001ac50c4";
constexpr auto kByteHash = "58468b46afc797d551cce0824c779231753a28077ae07ec9261052280e033528";

class Acknowledgements final : public livekit::RoomDelegate {
public:
    std::string run, target, observer;
    int expected_complete = 2;
    std::mutex mutex;
    std::condition_variable changed;
    std::set<std::string> identities;
    void onUserPacketReceived(livekit::Room&, const livekit::UserDataPacketEvent& event) override {
        if (event.topic != std::string(kTopic) + ".ack" || !event.participant) return;
        const auto identity = event.participant->identity();
        if (identity != target && identity != observer) return;
        const auto expected = "l3-ack-v1\ne2ee-interop\n" + run + "\n" +
            std::to_string(identity == target ? expected_complete : 0) + "\n0";
        if (event.data != std::vector<uint8_t>(expected.begin(), expected.end())) return;
        std::lock_guard lock(mutex);
        identities.insert(identity);
        changed.notify_all();
    }
    bool Wait() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(40), [&] {
            return identities.contains(target) &&
                (observer.empty() || identities.contains(observer));
        });
    }
};

std::map<std::string, std::string> Attributes(const std::string& run,
                                             const std::string& kind,
                                             size_t bytes, const char* hash) {
    return {{"l3_run_id", run}, {"l3_case", "e2ee-interop"}, {"l3_kind", kind},
            {"l3_sequence", "official-" + kind}, {"l3_bytes", std::to_string(bytes)},
            {"l3_sha256", hash}};
}

int Run(const std::string& run, const std::string& mode,
        const std::string& target, const std::string& observer, int expected_complete) {
    const char* url = std::getenv("LIVEKIT_URL");
    const char* token = std::getenv("LIVEKIT_TOKEN");
    if (!url || !token) return 2;
    Acknowledgements ack;
    ack.run = run;
    ack.target = target;
    ack.observer = observer;
    ack.expected_complete = expected_complete;
    livekit::Room room;
    room.setDelegate(&ack);
    livekit::RoomOptions options;
    options.encryption = livekit::E2EEOptions{};
    const std::vector<uint8_t> material(32, 0x42); // Public synthetic fixture only.
    if (mode == "shared") options.encryption->key_provider_options.shared_key = material;
    if (!room.connect(url, token, options)) return 2;
    auto local = room.localParticipant().lock();
    auto manager = room.e2eeManager().lock();
    if (!local || !manager) return 2;
    auto keys = manager->keyProvider().lock();
    if (!keys) return 2;
    if (mode == "shared") keys->setSharedKey(material, 3);
    else keys->setKey(local->identity(), material, 3);
    manager->setEnabled(true);
    if (!manager->enabled()) return 1;
    std::cout << "[OFFICIAL] connected=true encryption=GCM mode=" << mode
              << " key_index=3" << std::endl;

    std::string text(45000, 'a');
    for (size_t i = 0; i < text.size(); ++i) text[i] = 'a' + ((i + 17 * 11) % 26);
    std::vector<uint8_t> bytes(60000);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 131 + 23 * 17 + (i >> 7)) & 255);
    livekit::TextStreamWriter text_writer(*local, kTopic,
        Attributes(run, "text", text.size(), kTextHash), run + "-text",
        text.size(), "", {target});
    text_writer.write(text);
    text_writer.close();
    livekit::ByteStreamWriter byte_writer(*local, "interop.bin", kTopic,
        Attributes(run, "byte", bytes.size(), kByteHash), run + "-byte",
        bytes.size(), "application/octet-stream", {target});
    byte_writer.write(bytes);
    byte_writer.close();
    std::cout << "[OFFICIAL] encrypted_streams_sent=2 text_bytes=" << text.size()
              << " byte_bytes=" << bytes.size() << std::endl;

    // A separately keyed, authenticated control marker follows all stream
    // packets on the same reliable channel. Receivers always have slot 4, even
    // when slot 3 is deliberately absent/wrong. Frame enablement is independent
    // of the Rust backend's DataChannel encryption enablement.
    const std::vector<uint8_t> control_material(32, 0x43);
    if (mode == "shared") keys->setSharedKey(control_material, 4);
    else keys->setKey(local->identity(), control_material, 4);
    const std::string done = "e2ee-done-v1\n" + run;
    const auto destinations = observer.empty() ? std::vector<std::string>{target}
        : std::vector<std::string>{target, observer};
    local->publishData({done.begin(), done.end()}, true, destinations,
                       std::string(kTopic) + ".done");
    const bool passed = ack.Wait();
    std::cout << "[OFFICIAL] required_ack=" << (passed ? "true" : "false")
              << " observer=" << (observer.empty() ? "NOT_RUN" : "included")
              << " status=" << (passed ? "PASS" : "INCONCLUSIVE") << std::endl;
    room.disconnect();
    return passed ? 0 : 2;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 6 || (std::string(argv[2]) != "shared" && std::string(argv[2]) != "participant") ||
        (std::string(argv[5]) != "0" && std::string(argv[5]) != "2")) {
        std::cerr << "Usage: sender RUN shared|participant TARGET OBSERVER 0|2\n";
        return 2;
    }
    livekit::initialize(livekit::LogLevel::Off);
    int result = 1;
    try {
        result = Run(argv[1], argv[2], argv[3],
                     std::string(argv[4]) == "-" ? "" : argv[4], argv[5][0] - '0');
    } catch (...) {
        std::cerr << "[OFFICIAL] status=FAIL reason=sdk_exception\n";
    }
    livekit::shutdown();
    return result;
}
