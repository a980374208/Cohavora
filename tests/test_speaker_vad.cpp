#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>
#include "participant.h"
#include "room.h"
#include "audio_vad.h"
#include "livekit_models.pb.h"

using namespace livekit;

class TestRoomListener : public RoomListener {
public:
    void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<Participant>>& speakers) override {
        last_speakers = speakers;
        event_count++;
    }

    std::vector<std::shared_ptr<Participant>> last_speakers;
    int event_count{0};
};

void TestActiveSpeakerUpdateSignal() {
    std::cout << "[TEST 1] Testing ActiveSpeakerUpdate Protobuf Parsing & Sorting..." << std::endl;

    asio::io_context io_ctx;
    auto room = Room::Create(io_ctx.get_executor());
    auto listener = std::make_shared<TestRoomListener>();
    room->AddListener(listener);

    // Create 3 remote participants
    auto p1 = std::make_shared<RemoteParticipant>("PA_111", "user_alice");
    auto p2 = std::make_shared<RemoteParticipant>("PA_222", "user_bob");
    auto p3 = std::make_shared<RemoteParticipant>("PA_333", "user_charlie");

    // Manually add to room for testing
    proto::ParticipantUpdate update;
    auto* info1 = update.add_participants();
    info1->set_sid("PA_111");
    info1->set_identity("user_alice");
    info1->set_state(proto::ParticipantInfo::JOINED);

    auto* info2 = update.add_participants();
    info2->set_sid("PA_222");
    info2->set_identity("user_bob");
    info2->set_state(proto::ParticipantInfo::JOINED);

    auto* info3 = update.add_participants();
    info3->set_sid("PA_333");
    info3->set_identity("user_charlie");
    info3->set_state(proto::ParticipantInfo::JOINED);

    room->UpdateParticipantsForTesting(update);

    // Create SpeakersChanged: Bob (level 0.9, active), Alice (level 0.4, active), Charlie (level 0.0, inactive)
    proto::SpeakersChanged speaker_update;
    auto* s1 = speaker_update.add_speakers();
    s1->set_sid("PA_111");
    s1->set_level(0.4f);
    s1->set_active(true);

    auto* s2 = speaker_update.add_speakers();
    s2->set_sid("PA_222");
    s2->set_level(0.9f);
    s2->set_active(true);

    auto* s3 = speaker_update.add_speakers();
    s3->set_sid("PA_333");
    s3->set_level(0.0f);
    s3->set_active(false);

    room->HandleActiveSpeakerUpdateForTesting(speaker_update);

    assert(listener->event_count == 1);
    assert(listener->last_speakers.size() == 2);
    // Bob should be first (0.9), Alice second (0.4)
    assert(listener->last_speakers[0]->sid() == "PA_222");
    assert(listener->last_speakers[0]->audio_level() == 0.9f);
    assert(listener->last_speakers[0]->is_speaking() == true);

    assert(listener->last_speakers[1]->sid() == "PA_111");
    assert(listener->last_speakers[1]->audio_level() == 0.4f);
    assert(listener->last_speakers[1]->is_speaking() == true);

    std::cout << "  [PASS] ActiveSpeakerUpdate parsed and active speakers sorted by volume." << std::endl;
}

void TestAudioVadCalculation() {
    std::cout << "[TEST 2] Testing AudioVad PCM RMS Energy & Speaking Detection..." << std::endl;

    AudioVad vad(-40.0f);

    // 1. Silent buffer (all zeros)
    std::vector<int16_t> silent_pcm(480, 0);
    float silent_rms = AudioVad::CalculateRmsPcm16(silent_pcm.data(), silent_pcm.size());
    float silent_level = AudioVad::RmsToAudioLevel(silent_rms);
    bool silent_speaking = vad.IsSpeaking(silent_rms);

    assert(silent_rms == 0.0f);
    assert(silent_level == 0.0f);
    assert(silent_speaking == false);
    std::cout << "  [PASS] Silent PCM correctly detected as silent." << std::endl;

    // 2. Loud sine wave buffer (amplitude 16384)
    std::vector<int16_t> loud_pcm(480);
    for (size_t i = 0; i < loud_pcm.size(); ++i) {
        loud_pcm[i] = static_cast<int16_t>(16384.0 * std::sin(2.0 * 3.14159 * 440.0 * i / 48000.0));
    }
    float loud_rms = AudioVad::CalculateRmsPcm16(loud_pcm.data(), loud_pcm.size());
    float loud_level = AudioVad::RmsToAudioLevel(loud_rms);
    bool loud_speaking = vad.IsSpeaking(loud_rms);

    assert(loud_rms > 0.3f);
    assert(loud_level > 0.8f);
    assert(loud_speaking == true);
    std::cout << "  [PASS] Loud PCM (Sine Wave) correctly detected as speaking (RMS=" << loud_rms << ", level=" << loud_level << ")." << std::endl;
}

int main() {
    std::cout << "[TEST] Starting Speaker VAD Unit Tests..." << std::endl;
    TestActiveSpeakerUpdateSignal();
    TestAudioVadCalculation();
    std::cout << "[SUCCESS] ALL Speaker VAD Unit Tests Passed!" << std::endl;
    return 0;
}
