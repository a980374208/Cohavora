#pragma once

#include "src/core/meeting_coordinator.h"
#include <QtCore/QJsonObject>
#include <array>
#include <mutex>
#include <set>

// Dedicated public control messages coordinate concurrent byte and text batches
// per key epoch. No retries of missing byte payloads, and no content is saved.
class E2eeProductByteStream final {
    struct State {
        std::mutex mutex;
        int readyEpoch = 0;
        std::vector<std::shared_ptr<livekit::ByteStreamReader>> readers;
        std::vector<std::shared_ptr<livekit::TextStreamReader>> textReaders;
    };
    struct Listener final : livekit::RoomListener {
        explicit Listener(std::shared_ptr<State> state) : state(std::move(state)) {}
        void OnDataReceived(const std::vector<uint8_t>& bytes,
            std::shared_ptr<livekit::RemoteParticipant> participant, const std::string& topic) override {
            if (!participant || participant->identity() != "receiver" || topic != "e2ee.bytes.control") return;
            const std::string message(bytes.begin(), bytes.end());
            std::lock_guard lock(state->mutex);
            if (message == "bytes-ready-4") state->readyEpoch = 4;
            if (message == "bytes-ready-6") state->readyEpoch = 6;
        }
        void OnByteStreamOpened(std::shared_ptr<livekit::ByteStreamReader> reader,
            std::shared_ptr<livekit::Participant>) override {
            if (reader->info().topic != "e2ee.bytes") return;
            std::lock_guard lock(state->mutex);
            if (state->readers.size() < 8) state->readers.push_back(std::move(reader));
            else reader->OnStreamError("fixture reader bound exceeded");
        }
        std::shared_ptr<State> state;
        void OnTextStreamOpened(std::shared_ptr<livekit::TextStreamReader> reader,
            std::shared_ptr<livekit::Participant>) override {
            if (reader->info().topic != "e2ee.concurrent-text") return;
            std::lock_guard lock(state->mutex);
            if (state->textReaders.size() < 8) state->textReaders.push_back(std::move(reader));
            else reader->OnStreamError("fixture text reader bound exceeded");
        }
    };
public:
    using Emit = std::function<void(const char*, QJsonObject)>;
    E2eeProductByteStream(OpenMeeting::MeetingCoordinator& owner, Emit callback)
        : owner_(owner), emit_(std::move(callback)) {}
    static std::string textBlock(int epoch, int index) {
        const auto unit = "e2ee-text-" + std::to_string(epoch) + "-" + std::to_string(index)
            + ":\xe4\xb8\xad\xf0\x9f\x99\x82;";
        std::string block;
        for (int i = 0; i < 1024; ++i) block += unit;
        return block;
    }
    void tick() {
        if (qEnvironmentVariable("E2EE_PRODUCT_BYTE_STREAMS") != "1") return;
        auto room = owner_.room();
        if (!room) return;
        if (room_.lock() != room) {
            room_ = room;
            state_ = std::make_shared<State>();
            room->AddListener(std::make_shared<Listener>(state_));
        }
        int readyEpoch = 0;
        {
            std::lock_guard lock(state_->mutex);
            readyEpoch = state_->readyEpoch;
            for (auto it = state_->readers.begin(); it != state_->readers.end();) {
                if (!(*it)->is_closed()) { ++it; continue; }
                const auto reader = *it;
                it = state_->readers.erase(it);
                const auto name = QString::fromStdString(reader->info().name).split('-');
                const int epoch = name.size() == 3 ? name[1].toInt() : 0;
                const int index = name.size() == 3 ? name[2].toInt() : -1;
                bool valid = !reader->is_failed() && (epoch == 4 || epoch == 6) && (index == 0 || index == 1);
                if (valid) {
                    const auto bytes = reader->ReadAll();
                    valid = bytes.size() == 65536;
                    for (size_t i = 0; valid && i < bytes.size(); ++i)
                        valid = bytes[i] == static_cast<uint8_t>(i + epoch + index);
                }
                emit_("product_byte_received", {{"epoch", epoch}, {"index", index}, {"valid", valid}});
            }
            for (auto it = state_->textReaders.begin(); it != state_->textReaders.end();) {
                if (!(*it)->is_closed()) { ++it; continue; }
                const auto reader = *it;
                it = state_->textReaders.erase(it);
                const auto& attrs = reader->info().attributes;
                const int epoch = attrs.contains("epoch") ? QString::fromStdString(attrs.at("epoch")).toInt() : 0;
                const int index = attrs.contains("index") ? QString::fromStdString(attrs.at("index")).toInt() : -1;
                bool valid = !reader->is_failed() && (epoch == 4 || epoch == 6) && (index == 0 || index == 1);
                if (valid) {
                    const auto block = textBlock(epoch, index);
                    valid = reader->ReadAll() == block + block;
                }
                emit_("product_text_received", {{"epoch", epoch}, {"index", index}, {"valid", valid}});
            }
        }
        const auto manager = room->e2ee_manager();
        if (!manager || owner_.state() != OpenMeeting::MeetingState::InMeeting) return;
        const auto epoch = static_cast<int>(manager->ReadMediaStatus(0).install_epoch);
        if ((epoch != 4 && epoch != 6) || readyEpoch != epoch || sent_.contains(epoch)) return;
        sent_.insert(epoch);
        try {
            std::array<std::shared_ptr<livekit::ByteStreamWriter>, 2> writers;
            for (int index = 0; index < 2; ++index)
                writers[index] = room->CreateByteStreamWriter("bytes-" + std::to_string(epoch) + "-" + std::to_string(index),
                    "e2ee.bytes", {}, "", 65536);
            for (size_t offset = 0; offset < 65536; offset += 8192) {
                for (int index = 0; index < 2; ++index) {
                    std::vector<uint8_t> bytes(8192);
                    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(offset + i + epoch + index);
                    writers[index]->Write(bytes);
                }
            }
            for (auto& writer : writers) writer->Close();
            emit_("product_byte_sent", {{"epoch", epoch}, {"streams", 2}});
            std::array<std::shared_ptr<livekit::TextStreamWriter>, 2> textWriters;
            for (int index = 0; index < 2; ++index) {
                textWriters[index] = room->CreateTextStreamWriter("e2ee.concurrent-text",
                    {{"epoch", std::to_string(epoch)}, {"index", std::to_string(index)}},
                    "", textBlock(epoch, index).size() * 2);
            }
            for (int chunk = 0; chunk < 2; ++chunk)
                for (int index = 0; index < 2; ++index) textWriters[index]->Write(textBlock(epoch, index));
            for (auto& writer : textWriters) writer->Close();
            emit_("product_text_sent", {{"epoch", epoch}, {"streams", 2}});
            const auto ready = "native-bytes-ready-" + std::to_string(epoch);
            if (!room->PublishData({ready.begin(), ready.end()}, true, {}, "e2ee.bytes.control"))
                emit_("product_byte_failed", {{"epoch", epoch}});
        } catch (...) { emit_("product_byte_failed", {{"epoch", epoch}}); }
    }
private:
    OpenMeeting::MeetingCoordinator& owner_;
    Emit emit_;
    std::weak_ptr<livekit::Room> room_;
    std::shared_ptr<State> state_;
    std::set<int> sent_;
};
