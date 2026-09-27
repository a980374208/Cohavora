#include "src/telemetry/diagnostic_pipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr int kProducers = 8;
constexpr int kEventsPerProducer = 750;
constexpr int kTotalEvents = kProducers * kEventsPerProducer;

struct Sample {
    double wall_ms = 0;
    double p99_us = 0;
    double maximum_us = 0;
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
};

Sample Run(bool enabled) {
    livekit::diagnostic::DiagnosticPipeline pipeline;
    std::array<std::vector<double>, kProducers> latencies;
    std::atomic<int> ready{0};
    std::atomic<bool> begin{false};
    std::atomic<std::uint64_t> baseline_sink{0};
    std::vector<std::thread> threads;
    for (int producer = 0; producer != kProducers; ++producer) {
        latencies[producer].reserve(kEventsPerProducer);
        threads.emplace_back([&, producer] {
            ready.fetch_add(1, std::memory_order_release);
            while (!begin.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int index = 0; index != kEventsPerProducer; ++index) {
                auto event = livekit::diagnostic::Event::Received(
                    livekit::diagnostic::ChatKind::Text, 17);
                if (!enabled) {
                    baseline_sink.fetch_add(event.bytes,
                        std::memory_order_relaxed);
                    continue;
                }
                const auto start = std::chrono::steady_clock::now();
                pipeline.TryEmit(event);
                const auto end = std::chrono::steady_clock::now();
                latencies[producer].push_back(
                    std::chrono::duration<double, std::micro>(end - start)
                        .count());
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != kProducers)
        std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    begin.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    const auto end = std::chrono::steady_clock::now();
    Sample sample;
    sample.wall_ms = std::chrono::duration<double, std::milli>(end - start)
        .count();
    if (!enabled) return sample;
    std::vector<double> measured;
    measured.reserve(kTotalEvents);
    for (const auto& producer : latencies)
        measured.insert(measured.end(), producer.begin(), producer.end());
    std::sort(measured.begin(), measured.end());
    sample.p99_us = measured[static_cast<std::size_t>(
        std::ceil(0.99 * measured.size())) - 1];
    sample.maximum_us = measured.back();
    const auto status = pipeline.GetStatus();
    sample.accepted = status.accepted;
    sample.dropped = status.dropped_ordinary + status.dropped_critical;
    return sample;
}

} // namespace

int main() {
    bool passed = true;
    Run(false);
    Run(true);
    std::cout << "synthetic=true producers=" << kProducers
              << " events_per_pair=" << kTotalEvents << '\n';
    for (int pair = 0; pair != 3; ++pair) {
        Sample baseline, enabled;
        if (pair % 2 == 0) {
            baseline = Run(false);
            enabled = Run(true);
        } else {
            enabled = Run(true);
            baseline = Run(false);
        }
        std::cout << "pair=" << pair + 1
                  << " baseline_ms=" << baseline.wall_ms
                  << " enabled_ms=" << enabled.wall_ms
                  << " tryemit_p99_us=" << enabled.p99_us
                  << " tryemit_max_us=" << enabled.maximum_us
                  << " accepted=" << enabled.accepted
                  << " dropped=" << enabled.dropped << '\n';
        passed &= enabled.accepted == kTotalEvents &&
            enabled.dropped == 0 && enabled.p99_us <= 100.0;
    }
    return passed ? 0 : 1;
}
