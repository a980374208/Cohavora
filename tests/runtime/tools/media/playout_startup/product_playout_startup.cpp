// Manual, service-free witness for playback before audio callback registration.
// Default mode requires playout to remain closed. --force-playout opens the
// selected output stream; neither mode records or persists audio samples.
#include <winsock2.h>

#include "src/rtc/webrtc_manager.h"
#include "api/audio/audio_device.h"

#include <iostream>
#include <string>

namespace {

struct PlayoutState {
    bool available = false;
    bool playing = false;
    bool initialized = false;
    int device_count = -1;
};

class ProbePeerObserver final : public webrtc::PeerConnectionObserver {
public:
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidate*) override {}
};

PlayoutState ReadPlayoutState(livekit::WebRTCManager& manager) {
    PlayoutState state;
    if (auto* worker = manager.worker_thread()) {
        worker->BlockingCall([manager_ptr = &manager, state_ptr = &state] {
            // The ADM reference is acquired and released on its owner thread;
            // none survives the subsequent WebRTCManager::Deinitialize call.
            const auto adm = manager_ptr->adm();
            if (!adm) return;
            state_ptr->available = true;
            state_ptr->playing = adm->Playing();
            state_ptr->initialized = adm->PlayoutIsInitialized();
            state_ptr->device_count = adm->PlayoutDevices();
        });
    }
    return state;
}

void Emit(const char* stage, const PlayoutState& state, const char* status) {
    std::cout << "{\"stage\":\"" << stage << "\",\"status\":\"" << status
              << "\",\"playing\":" << (state.playing ? "true" : "false")
              << ",\"initialized\":" << (state.initialized ? "true" : "false")
              << ",\"device_count\":" << state.device_count << "}" << std::endl;
}

bool MatchesIdle(const PlayoutState& state) {
    return state.available && state.device_count > 0 &&
           !state.playing && !state.initialized;
}

bool MatchesPlaying(const PlayoutState& state) {
    return state.available && state.device_count > 0 &&
           state.playing && state.initialized;
}

} // namespace

int main(int argc, char** argv) {
    bool force_playout = false;
    std::string device_id;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--force-playout") {
            force_playout = true;
        } else if (argument == "--device-id" && i + 1 < argc) {
            device_id = argv[++i];
        } else if (argument == "--help") {
            std::cout << "Usage: product_playout_startup [--device-id <endpoint>] [--force-playout]\n";
            return 0;
        } else {
            std::cout << "{\"stage\":\"arguments\",\"status\":\"FAIL\",\"reason\":\"invalid_arguments\"}\n";
            return 2;
        }
    }

    auto& manager = livekit::WebRTCManager::Instance();
    ProbePeerObserver observer;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer;
    const auto finish = [&manager, &peer](int code, const char* reason) {
        if (peer) {
            peer->Close();
            peer = nullptr;
        }
        manager.Deinitialize();
        const char* status = code == 0 ? "PASS" : (code == 3 ? "INCONCLUSIVE" : "FAIL");
        std::cout << "{\"stage\":\"complete\",\"status\":\"" << status
                  << "\",\"reason\":\"" << reason << "\"}" << std::endl;
        return code;
    };

    try {
        if (!manager.Initialize()) {
            Emit("after_initialize", ReadPlayoutState(manager), "FAIL");
            return finish(1, "manager_initialization_failed");
        }
        auto state = ReadPlayoutState(manager);
        if (state.available && state.device_count == 0) {
            Emit("after_initialize", state, "INCONCLUSIVE");
            return finish(3, "no_playout_device");
        }
        const bool idle_after_initialize = MatchesIdle(state);
        Emit("after_initialize", state, idle_after_initialize ? "PASS" : "FAIL");
        if (!idle_after_initialize) return finish(1, "unexpected_idle_playout_state");

        // Factory creation alone has not initialized VoiceEngine or registered
        // its AudioTransport. Playout must reject this unbound callback state.
        const bool unbound_started = manager.EnsurePlayout();
        state = ReadPlayoutState(manager);
        const bool rejected_unbound_playout = !unbound_started && MatchesIdle(state);
        Emit("after_unbound_ensure", state, rejected_unbound_playout ? "PASS" : "FAIL");
        if (!rejected_unbound_playout) return finish(1, "unbound_playout_was_not_rejected");

        // A real PeerConnection initializes the media engine and registers the
        // production transport. No SDP, ICE server, offer or media track is used.
        auto* signaling = manager.signaling_thread();
        if (!signaling) return finish(1, "signaling_thread_unavailable");
        signaling->BlockingCall([manager_ptr = &manager, observer_ptr = &observer,
                                  peer_ptr = &peer] {
            const auto factory = manager_ptr->factory();
            if (!factory) return;
            webrtc::PeerConnectionInterface::RTCConfiguration configuration;
            configuration.servers.clear();
            auto created = factory->CreatePeerConnectionOrError(
                configuration, webrtc::PeerConnectionDependencies(observer_ptr));
            if (created.ok()) *peer_ptr = created.MoveValue();
        });
        state = ReadPlayoutState(manager);
        const bool idle_after_peer = peer && MatchesIdle(state);
        Emit("after_peer_creation", state, idle_after_peer ? "PASS" : "FAIL");
        if (!idle_after_peer) return finish(1, "peer_creation_changed_idle_playout_state");

        const bool selected = manager.SetPlayoutDeviceById(device_id);
        state = ReadPlayoutState(manager);
        const bool idle_after_selection = selected && MatchesIdle(state);
        Emit(device_id.empty() ? "after_default_selection" : "after_explicit_selection",
             state, idle_after_selection ? "PASS" : "FAIL");
        if (!idle_after_selection) return finish(1, "selection_changed_idle_playout_state");

        if (force_playout) {
            const bool started = manager.EnsurePlayout();
            state = ReadPlayoutState(manager);
            const bool playing_after_ensure = started && MatchesPlaying(state);
            Emit("after_force_playout", state, playing_after_ensure ? "PASS" : "FAIL");
            if (!playing_after_ensure) return finish(1, "explicit_playout_failed");

            const bool retained = manager.EnsurePlayout();
            state = ReadPlayoutState(manager);
            const bool still_playing = retained && MatchesPlaying(state);
            Emit("after_repeat_ensure", state, still_playing ? "PASS" : "FAIL");
            if (!still_playing) return finish(1, "repeat_ensure_failed");
        }
        return finish(0, force_playout ? "explicit_playout_state_verified" : "idle_playout_state_verified");
    } catch (...) {
        return finish(1, "probe_exception");
    }
}
