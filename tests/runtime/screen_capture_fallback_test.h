#pragma once
#include "screen_capture_fallback.h"
#include "tests/support/test_check.h"
#include <array>
#include <stdexcept>

inline void TestScreenCaptureFallback() {
    using Capturer = webrtc::DesktopCapturer;
    using Result = Capturer::Result;
    struct State {
        int created = 0, destroyed = 0, captures = 0;
        bool selected = true, throws = false, in_capture = false;
        Capturer::SourceId source = 0;
        Result result = Result::SUCCESS;
    };
    struct Fake final : Capturer {
        State& state;
        Callback* callback = nullptr;
        explicit Fake(State& s) : state(s) { ++state.created; }
        ~Fake() override { TEST_CHECK(!state.in_capture); ++state.destroyed; }
        bool GetSourceList(SourceList*) override { return false; }
        bool SelectSource(SourceId id) override { state.source = id; return state.selected; }
        void Start(Callback* c) override { callback = c; }
        void CaptureFrame() override {
            ++state.captures;
            if (state.throws) throw std::runtime_error("injected capture error");
            state.in_capture = true;
            callback->OnCaptureResult(state.result, state.result == Result::SUCCESS
                ? std::make_unique<webrtc::BasicDesktopFrame>(webrtc::DesktopSize(8, 8)) : nullptr);
            state.in_capture = false;
        }
    };
    struct Sink final : Capturer::Callback {
        Result result = Result::ERROR_PERMANENT;
        void OnCaptureResult(Result r, std::unique_ptr<webrtc::DesktopFrame>) override { result = r; }
    };
    // Each case exercises the same chain used by production, without requiring
    // unsupported hardware or forcing a physical GPU failure.
    for (int scenario = 0; scenario < 9; ++scenario) {
        std::array<State, 3> states{};
        if (scenario == 1) states[0].result = Result::ERROR_TEMPORARY;
        if (scenario == 2) states[0].result = Result::ERROR_PERMANENT;
        if (scenario == 3) states[0].selected = false;
        if (scenario == 4) states[0].throws = true;
        if (scenario >= 5) states[0].result = Result::ERROR_PERMANENT;
        if (scenario == 6) states[1].result = Result::ERROR_PERMANENT;
        if (scenario == 7) states[1].result = Result::ERROR_TEMPORARY;
        if (scenario == 8) for (auto& state : states) state.result = Result::ERROR_PERMANENT;
        Sink sink;
        {
            std::vector<livekit::ScreenCaptureFallback::Backend> factories;
            for (int i = 0; i < 3; ++i) {
                factories.push_back({[&, i]() -> std::unique_ptr<Capturer> {
                    if (scenario == 5 && i < 2) return nullptr;
                    return std::make_unique<Fake>(states[i]);
                }, i > 0});
            }
            livekit::ScreenCaptureFallback chain(std::move(factories));
            TEST_CHECK(chain.SelectSource(17));
            chain.Start(&sink);
            chain.CaptureFrame();
            if (scenario == 8) {
                TEST_CHECK(sink.result == Result::ERROR_PERMANENT);
                for (const auto& state : states)
                    TEST_CHECK(state.captures == 1 && state.destroyed == 1);
                continue;
            }
            if (scenario == 7) {
                TEST_CHECK(sink.result == Result::ERROR_TEMPORARY && states[2].created == 0);
                states[1].result = Result::SUCCESS;
                chain.CaptureFrame();
            }
            TEST_CHECK(sink.result == Result::SUCCESS);
            const int winner = scenario == 0 ? 0 : (scenario == 5 || scenario == 6) ? 2 : 1;
            TEST_CHECK(states[winner].captures > 0 && states[winner].source == 17);
            const int first_captures = states[0].captures;
            chain.CaptureFrame();
            if (winner != 0) TEST_CHECK(states[0].captures == first_captures);
            if (winner < 2) TEST_CHECK(states[2].created == 0);
        }
        for (const auto& state : states) TEST_CHECK(state.created == state.destroyed);
    }
}
