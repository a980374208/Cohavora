#include "../probes/microphone_signal_metrics.h"
#include <iostream>
#include <random>

using namespace microphone_witness;
int main() {
    unsigned cases = 0;
    for (const unsigned rate : {16000, 44100, 48000}) {
        for (const double frequency : {0.0, 440.0, 997.0}) {
            SignalBlock block(rate);
            for (unsigned n = 0; n < rate / 10; ++n)
                block.add(.025 * std::sin(2 * std::acos(-1.0) * frequency * n / rate));
            if (block.matchesTone() != (frequency == 997.0)) return 1;
            if (frequency == 997.0 && std::abs(block.rms() - .025 / std::sqrt(2.0)) > .0002) return 2;
            ++cases;
        }
    }
    SignalBlock dc(48000), noise(48000), quiet(48000), clipped(48000);
    std::mt19937 generator(314159);
    std::normal_distribution<double> distribution(0, .03);
    for (unsigned n = 0; n < 4800; ++n) {
        dc.add(.1); noise.add(distribution(generator));
        const auto wave = std::sin(2 * std::acos(-1.0) * kToneHz * n / 48000);
        quiet.add(.001 * wave); clipped.add(wave);
    }
    if (dc.nonSilent() || noise.matchesTone() || quiet.matchesTone() || clipped.matchesTone()) return 3;
    std::cout << cases + 4 << " synthetic signal cases PASS\n";
    return 0;
}
