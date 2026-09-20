#include "livekit/livekit.h"
#include "livekit/data_stream.h"
#include "livekit/room_delegate.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr auto kTopic = "lk.l3.e2ee";
constexpr auto kTextHash = "065185be71990d5579218c42b67f4c347e2681daf7640b4be69f44e001ac50c4";
constexpr auto kByteHash = "58468b46afc797d551cce0824c779231753a28077ae07ec9261052280e033528";

std::string Sha256(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("hash initialization failed");
    unsigned char digest[32]{};
    const auto status = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("hash failed");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : digest) out << std::setw(2) << static_cast<int>(byte);
    return out.str();
}

class Receiver final : public livekit::RoomDelegate {
public:
    std::string run, sender;
    int expected = 2;

    ~Receiver() { Join(); }

    void Register(livekit::Room& room) {
        room.registerTextStreamHandler(kTopic, [this](auto reader, const auto& identity) {
            Start(reader, identity, "text", std::string{});
        });
        room.registerByteStreamHandler(kTopic, [this](auto reader, const auto& identity) {
            Start(reader, identity, "byte", std::vector<uint8_t>{});
        });
        room.registerTextStreamHandler(std::string(kTopic) + ".done",
            [this](auto reader, const auto& identity) {
                Start(reader, identity, "done", std::string{});
            });
    }

    void onUserPacketReceived(livekit::Room&, const livekit::UserDataPacketEvent& event) override {
        if (event.topic == kTopic || event.topic == std::string(kTopic) + ".done") {
            std::lock_guard lock(mutex_);
            ++unexpected_raw_;
            changed_.notify_all();
        }
    }

    bool Wait() {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 45s, [&] {
            return done_ && finished_ == opened_;
        })) return false;
        // DONE follows all main packets on the reliable channel. This extra
        // observation window also detects duplicate/late business deliveries.
        changed_.wait_for(lock, 2s, [&] { return invalid_ || unexpected_raw_; });
        return ValidLocked();
    }

    bool Report() {
        std::lock_guard lock(mutex_);
        const bool valid = ValidLocked();
        std::cout << "[OFFICIAL] streams=" << streams_.size() << " opened=" << opened_
                  << " finished=" << finished_ << " invalid=" << invalid_
                  << " unexpected_raw=" << unexpected_raw_ << " done=" << done_
                  << " status=" << (valid ? "PASS" : "FAIL") << std::endl;
        return valid;
    }

    void Join() {
        // Call after disconnect, when SDK callbacks have stopped. A malformed
        // stream that never closes is bounded by the parent process deadline.
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        workers_.clear();
    }

private:
    bool ValidLocked() const {
        return done_ && !invalid_ && !unexpected_raw_ && finished_ == opened_ &&
            opened_ == static_cast<size_t>(expected + 1) &&
            streams_.size() == static_cast<size_t>(expected);
    }

    template <typename Reader, typename Chunk>
    void Start(std::shared_ptr<Reader> reader, const std::string& identity,
               std::string kind, Chunk chunk) {
        const auto initial = reader->info();
        const auto attr = initial.attributes.find("l3_run_id");
        if (attr == initial.attributes.end() || attr->second != run) return;
        std::lock_guard lock(mutex_);
        ++opened_;
        workers_.emplace_back([this, reader, identity, kind, initial, chunk]() mutable {
            bool valid = false;
            size_t chunks = 0;
            std::string hash;
            try {
                std::vector<uint8_t> data;
                const size_t limit = kind == "text" ? 45000 : kind == "byte" ? 60000 : 256;
                while (reader->readNext(chunk)) {
                    ++chunks;
                    if (chunk.size() > limit - data.size()) throw std::runtime_error("bound exceeded");
                    data.insert(data.end(), chunk.begin(), chunk.end());
                }
                // info attributes are inspected after EOS (onStreamClose merges
                // trailer attributes under the same mutex as readNext).
                const auto final = reader->info();
                valid = identity == sender && initial.size == data.size() &&
                    final.attributes.contains("l3_result") &&
                    final.attributes.at("l3_result") == "complete";
                if (kind == "done") {
                    const std::string done = "e2ee-done-v1\n" + run;
                    valid = valid && data == std::vector<uint8_t>(done.begin(), done.end()) &&
                        initial.stream_id == run + "-done" && chunks == 1;
                } else {
                    const auto sequence = "native-" + kind;
                    const auto expected_hash = kind == "text" ? kTextHash : kByteHash;
                    std::vector<uint8_t> fixture(limit);
                    for (size_t i = 0; i < limit; ++i)
                        fixture[i] = kind == "text" ? static_cast<uint8_t>('a' + ((i + 17 * 11) % 26))
                            : static_cast<uint8_t>((i * 131 + 23 * 17 + (i >> 7)) & 255);
                    hash = Sha256(data);
                    valid = valid && data == fixture && hash == expected_hash &&
                        initial.stream_id == run + "-" + sequence &&
                        initial.topic == kTopic &&
                        initial.attributes.at("l3_case") == "e2ee-outbound" &&
                        initial.attributes.at("l3_kind") == kind &&
                        initial.attributes.at("l3_sequence") == sequence &&
                        initial.attributes.at("l3_bytes") == std::to_string(limit) &&
                        initial.attributes.at("l3_sha256") == expected_hash &&
                        initial.mime_type == (kind == "text" ? "text/plain" : "application/octet-stream") &&
                        chunks == (kind == "text" ? 3 : 4);
                    if constexpr (requires { initial.name; })
                        valid = valid && initial.name == sequence + ".bin";
                }
            } catch (...) { valid = false; }
            std::lock_guard lock(mutex_);
            ++finished_;
            if (!valid) ++invalid_;
            if (kind == "done") {
                if (done_) ++invalid_;
                done_ = valid;
            } else if (!streams_.insert(kind).second) ++invalid_;
            std::cout << "[OFFICIAL] kind=" << kind << " chunks=" << chunks
                      << " content_and_metadata=" << (valid ? "true" : "false")
                      << " sha256=" << hash << std::endl;
            changed_.notify_all();
        });
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::thread> workers_;
    std::set<std::string> streams_;
    size_t opened_ = 0, finished_ = 0, invalid_ = 0, unexpected_raw_ = 0;
    bool done_ = false;
};

int Run(const std::string& run, const std::string& mode,
        const std::string& sender, const std::string& state) {
    const char* url = std::getenv("LIVEKIT_URL");
    const char* token = std::getenv("LIVEKIT_TOKEN");
    if (!url || !token) return 2;
    Receiver receiver;
    receiver.run = run;
    receiver.sender = sender;
    receiver.expected = state == "good" ? 2 : 0;
    livekit::Room room;
    room.setDelegate(&receiver);
    receiver.Register(room);
    livekit::RoomOptions options;
    options.encryption = livekit::E2EEOptions{};
    std::vector<uint8_t> material(32, 0x42);
    if (state == "wrong") material.front() ^= 1;
    if (mode == "shared") options.encryption->key_provider_options.shared_key = material;
    if (!room.connect(url, token, options)) return 2;
    auto local = room.localParticipant().lock();
    auto manager = room.e2eeManager().lock();
    if (!local || !manager) return 2;
    auto keys = manager->keyProvider().lock();
    if (!keys) return 2;
    if (state != "missing") {
        for (const int slot : {3, 5}) {
            // Distinct material makes a wrong slot selection observable in L3.
            std::vector<uint8_t> stream_material(32, slot == 3 ? 0x42 : 0x44);
            if (state == "wrong") stream_material.front() ^= 1;
            if (mode == "shared") keys->setSharedKey(stream_material, slot);
            else keys->setKey(sender, stream_material, slot);
        }
    }
    const std::vector<uint8_t> control(32, 0x43);
    if (mode == "shared") keys->setSharedKey(control, 4);
    else {
        keys->setKey(sender, control, 4);
        keys->setKey(local->identity(), control, 4);
    }
    manager->setEnabled(true);
    if (!manager->enabled()) return 1;
    std::cout << "[SESSION] local_identity=" << local->identity() << std::endl;
    bool passed = receiver.Wait();
    if (passed) {
        const std::string ack = "l3-ack-v1\ne2ee-outbound\n" + run + "\n" +
            std::to_string(receiver.expected) + "\n0";
        local->publishData({ack.begin(), ack.end()}, true, {sender}, std::string(kTopic) + ".ack");
        std::cout << "[OFFICIAL] encrypted_ack_sent=true control_slot=4" << std::endl;
        // Allow the reliable ACK to leave before disconnecting the publisher.
        std::this_thread::sleep_for(2s);
    }
    room.disconnect();
    receiver.Join();
    passed = receiver.Report() && passed;
    return passed ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 5 || (std::string(argv[2]) != "shared" && std::string(argv[2]) != "participant") ||
        (std::string(argv[4]) != "good" && std::string(argv[4]) != "wrong" && std::string(argv[4]) != "missing")) {
        std::cerr << "Usage: receiver RUN shared|participant SENDER good|wrong|missing\n";
        return 2;
    }
    livekit::initialize(livekit::LogLevel::Off);
    int result = 1;
    try { result = Run(argv[1], argv[2], argv[3], argv[4]); }
    catch (...) { std::cerr << "[OFFICIAL] status=FAIL reason=sdk_exception\n"; }
    livekit::shutdown();
    return result;
}
