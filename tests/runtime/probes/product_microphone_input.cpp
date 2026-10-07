// Independent shared WASAPI microphone witness and bounded synthetic source.
// PCM stays in memory. It never changes endpoint selection, mute or volume.
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "microphone_signal_metrics.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace microphone_witness;

struct Handle { HANDLE value{}; ~Handle() { if (value) CloseHandle(value); } };
struct MixFormat { WAVEFORMATEX* value{}; ~MixFormat() { CoTaskMemFree(value); } };
struct ClientStop { ComPtr<IAudioClient> client; ~ClientStop() { if (client) client->Stop(); } };
static void Check(HRESULT hr) { if (FAILED(hr)) throw hr; }

static std::string Fingerprint(IMMDevice* device) {
    LPWSTR id = nullptr;
    Check(device->GetId(&id));
    const auto size = static_cast<ULONG>(std::wcslen(id) * sizeof(wchar_t));
    unsigned char hash[32]{};
    const auto status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(id), size, hash, sizeof(hash));
    CoTaskMemFree(id);
    if (status < 0) throw E_FAIL;
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (auto byte : hash) result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

struct Format {
    bool floating = false;
    unsigned bytes = 0, channels = 0, rate = 0, validBits = 0;
    explicit Format(const WAVEFORMATEX& f) {
        auto tag = f.wFormatTag;
        validBits = f.wBitsPerSample;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            if (f.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) throw E_INVALIDARG;
            const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(f);
            validBits = ext.Samples.wValidBitsPerSample;
            if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
            else throw AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        bytes = f.wBitsPerSample / 8;
        channels = f.nChannels;
        rate = f.nSamplesPerSec;
        if (!channels || channels > 16 || rate < 8000 || rate > 192000 ||
            f.nBlockAlign != channels * bytes || !validBits || validBits > f.wBitsPerSample ||
            (floating ? bytes != 4 || validBits != 32 : tag != WAVE_FORMAT_PCM || (bytes != 2 && bytes != 3 && bytes != 4)))
            throw AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    double read(const BYTE* data) const {
        if (floating) {
            float value{}; std::memcpy(&value, data, sizeof(value));
            if (!std::isfinite(value)) throw E_INVALIDARG;
            return value;
        }
        if (bytes == 2) { std::int16_t value{}; std::memcpy(&value, data, 2); return value / 32768.0; }
        if (bytes == 3) {
            auto value = static_cast<std::int32_t>(data[0] | data[1] << 8 | data[2] << 16);
            if (value & 0x800000) value |= static_cast<std::int32_t>(0xff000000);
            return value / 8388608.0;
        }
        std::int32_t value{}; std::memcpy(&value, data, 4); return value / 2147483648.0;
    }
    void write(BYTE* data, double value) const {
        if (floating) { const auto v = static_cast<float>(value); std::memcpy(data, &v, 4); }
        else if (bytes == 2) { const auto v = static_cast<std::int16_t>(value * 32767); std::memcpy(data, &v, 2); }
        else {
            const auto scale = std::ldexp(1.0, validBits - 1) - 1;
            const auto v = static_cast<std::int32_t>(value * scale) * (std::int64_t{1} << (bytes * 8 - validBits));
            for (unsigned i = 0; i < bytes; ++i) data[i] = static_cast<BYTE>((static_cast<std::uint64_t>(v) >> (i * 8)) & 255);
        }
    }
};

int wmain(int argc, wchar_t** argv) {
    // capture|tone, run_id, new JSONL path, seconds [explicit endpoint id].
    if (argc < 5 || argc > 6) return 2;
    const bool tone = std::wstring(argv[1]) == L"tone";
    if (!tone && std::wstring(argv[1]) != L"capture") return 2;
    const std::wstring wideRun(argv[2]);
    if (wideRun.size() != 32 || !std::all_of(wideRun.begin(), wideRun.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    })) return 2;
    std::string run;
    for (const auto c : wideRun) run.push_back(static_cast<char>(c));
    wchar_t* end = nullptr;
    const auto seconds = std::wcstoul(argv[4], &end, 10);
    const std::filesystem::path destination(argv[3]);
    if (*end || seconds < 1 || seconds > 30000 || run.size() != 32 ||
        !std::all_of(run.begin(), run.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        std::filesystem::exists(destination)) return 2;
    std::ofstream out(destination, std::ios::binary);
    if (!out) return 2;
    std::uint64_t sequence = 0;
    auto emit = [&](const char* event, const std::string& fields) {
        const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out << "{\"schema\":1,\"collector\":\"wasapi_microphone_input\",\"run_id\":\"" << run
            << "\",\"sequence\":" << ++sequence << ",\"utc_ms\":" << utc
            << ",\"event\":\"" << event << "\"" << fields << "}\n";
        out.flush();
        if (!out) throw E_FAIL;
    };
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result = 1;
    try {
        Check(initialized);
        ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
        ComPtr<IMMDevice> device;
        auto role = std::string("eConsole");
        if (argc == 6) { Check(enumerator->GetDevice(argv[5], &device)); role = "explicit"; }
        else {
            auto hr = enumerator->GetDefaultAudioEndpoint(tone ? eRender : eCapture, eConsole, &device);
            if (FAILED(hr) && !tone) { hr = enumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &device); role = "eMultimedia"; }
            Check(hr);
        }
        const auto endpoint = Fingerprint(device.Get());
        ComPtr<IAudioEndpointVolume> volume;
        Check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume));
        BOOL muted{}; float level{};
        Check(volume->GetMute(&muted)); Check(volume->GetMasterVolumeLevelScalar(&level));
        ComPtr<IAudioClient> client;
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
        ClientStop stop{client};
        MixFormat mix;
        Check(client->GetMixFormat(&mix.value));
        const Format format(*mix.value);
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, tone ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            tone ? 1000000 : 200000, 0, mix.value, nullptr));
        emit("collector.started", ",\"mode\":\"" + std::string(tone ? "tone" : "capture") +
            "\",\"endpoint_sha256\":\"" + endpoint + "\",\"role\":\"" + role +
            "\",\"configuration\":\"" WITNESS_CONFIGURATION "\",\"sample_rate\":" + std::to_string(format.rate) +
            ",\"channels\":" + std::to_string(format.channels) + ",\"sample_bits\":" + std::to_string(format.bytes * 8) +
            ",\"valid_bits\":" + std::to_string(format.validBits) + ",\"float\":" + (format.floating ? "true" : "false") +
            ",\"endpoint_muted\":" + (muted ? "true" : "false") + ",\"endpoint_volume\":" + std::to_string(level) +
            ",\"pcm_persisted\":false,\"tone_hz\":997,\"tone_peak\":0.025,\"minimum_rms\":0.005,\"minimum_tone_fraction\":0.30,\"maximum_peak\":0.95");
        const auto began = Clock::now();
        auto window = began, lastPacket = began;
        std::uint64_t totalFrames = 0;
        if (tone) {
            ComPtr<IAudioRenderClient> render;
            Check(client->GetService(IID_PPV_ARGS(&render)));
            UINT32 bufferFrames{}; Check(client->GetBufferSize(&bufferFrames));
            const auto target = static_cast<std::uint64_t>(seconds) * format.rate;
            bool started = false;
            while (totalFrames < target) {
                UINT32 padding{}; Check(client->GetCurrentPadding(&padding));
                const auto available = static_cast<UINT32>((std::min)(static_cast<std::uint64_t>(bufferFrames - padding), target - totalFrames));
                if (available) {
                    BYTE* data{}; Check(render->GetBuffer(available, &data));
                    for (UINT32 f = 0; f < available; ++f) {
                        const auto n = totalFrames + f;
                        const auto ramp = (std::min)({1.0, n / (format.rate * 0.05), (target - n) / (format.rate * 0.05)});
                        const auto v = 0.025 * ramp * std::sin(2 * std::acos(-1.0) * kToneHz * n / format.rate);
                        for (unsigned c = 0; c < format.channels; ++c) format.write(data + (f * format.channels + c) * format.bytes, v);
                    }
                    Check(render->ReleaseBuffer(available, 0)); totalFrames += available;
                }
                if (!started) { Check(client->Start()); started = true; }
                if (Clock::now() - window >= std::chrono::seconds(1)) {
                    Check(volume->GetMute(&muted)); Check(volume->GetMasterVolumeLevelScalar(&level));
                    emit("tone.sample", ",\"submitted_frames\":" + std::to_string(totalFrames) + ",\"endpoint_muted\":" + (muted ? "true" : "false") + ",\"endpoint_volume\":" + std::to_string(level));
                    window = Clock::now();
                }
                if (Clock::now() - began > std::chrono::seconds(seconds + 5)) throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                Sleep(5);
            }
            UINT32 padding{};
            do { Sleep(5); Check(client->GetCurrentPadding(&padding)); } while (padding && Clock::now() - began < std::chrono::seconds(seconds + 5));
            if (padding) throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            Handle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
            if (!ready.value) throw HRESULT_FROM_WIN32(GetLastError());
            Check(client->SetEventHandle(ready.value));
            ComPtr<IAudioCaptureClient> capture;
            Check(client->GetService(IID_PPV_ARGS(&capture)));
            Check(client->Start());
            SignalBlock block(format.rate);
            std::uint64_t frames = 0, blocks = 0, nonSilent = 0, matching = 0, discontinuities = 0, timestampErrors = 0;
            UINT64 previousEnd = 0;
            bool positionSeen = false;
            std::uint64_t missingFrames = 0;
            double squares = 0, peak = 0, maxGap = 0, minBlockRms = 1, minTone = 1;
            auto sample = [&] {
                const auto now = Clock::now();
                const auto elapsed = std::chrono::duration<double>(now - window).count();
                maxGap = (std::max)(maxGap, std::chrono::duration<double, std::milli>(now - lastPacket).count());
                Check(volume->GetMute(&muted)); Check(volume->GetMasterVolumeLevelScalar(&level));
                bool sameDefault = true;
                if (argc != 6) { ComPtr<IMMDevice> current; const auto hr = enumerator->GetDefaultAudioEndpoint(eCapture, role == "eConsole" ? eConsole : eMultimedia, &current); sameDefault = SUCCEEDED(hr) && Fingerprint(current.Get()) == endpoint; }
                emit("microphone.sample", ",\"frames\":" + std::to_string(totalFrames) + ",\"window_frames\":" + std::to_string(frames) +
                    ",\"window_seconds\":" + std::to_string(elapsed) + ",\"rms\":" + std::to_string(frames ? std::sqrt(squares / (frames * format.channels)) : 0) +
                    ",\"peak\":" + std::to_string(peak) + ",\"blocks_100ms\":" + std::to_string(blocks) +
                    ",\"non_silent_blocks\":" + std::to_string(nonSilent) + ",\"tone_matched_blocks\":" + std::to_string(matching) +
                    ",\"minimum_block_rms\":" + std::to_string(blocks ? minBlockRms : 0) + ",\"minimum_tone_fraction\":" + std::to_string(blocks ? minTone : 0) +
                    ",\"max_packet_gap_ms\":" + std::to_string(maxGap) + ",\"discontinuities\":" + std::to_string(discontinuities) +
                    ",\"timestamp_errors\":" + std::to_string(timestampErrors) + ",\"missing_device_frames\":" + std::to_string(missingFrames) +
                    ",\"endpoint_muted\":" + (muted ? "true" : "false") + ",\"endpoint_volume\":" + std::to_string(level) + ",\"default_endpoint_unchanged\":" + (sameDefault ? "true" : "false"));
                window = now; frames = blocks = nonSilent = matching = 0; squares = peak = maxGap = 0; minBlockRms = minTone = 1;
            };
            while (Clock::now() - began < std::chrono::seconds(seconds)) {
                const auto waited = WaitForSingleObject(ready.value, 100);
                if (waited == WAIT_FAILED) throw HRESULT_FROM_WIN32(GetLastError());
                UINT32 packet{}; Check(capture->GetNextPacketSize(&packet));
                while (packet) {
                    BYTE* data{}; UINT32 count{}; DWORD flags{}; UINT64 devicePosition{}, qpc{};
                    Check(capture->GetBuffer(&data, &count, &flags, &devicePosition, &qpc));
                    const auto now = Clock::now();
                    maxGap = (std::max)(maxGap, std::chrono::duration<double, std::milli>(now - lastPacket).count()); lastPacket = now;
                    discontinuities += !!(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
                    timestampErrors += !!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
                    if (positionSeen && devicePosition > previousEnd) missingFrames += devicePosition - previousEnd;
                    if (positionSeen && devicePosition < previousEnd) throw E_FAIL;
                    previousEnd = devicePosition + count; positionSeen = true;
                    for (UINT32 f = 0; f < count; ++f) {
                        double mono = 0;
                        for (unsigned c = 0; c < format.channels; ++c) {
                            const auto v = flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : format.read(data + (f * format.channels + c) * format.bytes);
                            mono += v / format.channels; squares += v * v; peak = (std::max)(peak, std::abs(v));
                        }
                        block.add(mono);
                        if (block.frames >= format.rate / 10) {
                            ++blocks; nonSilent += block.nonSilent(); matching += block.matchesTone();
                            minBlockRms = (std::min)(minBlockRms, block.rms()); minTone = (std::min)(minTone, block.toneFraction());
                            block = SignalBlock(format.rate);
                        }
                    }
                    totalFrames += count; frames += count;
                    Check(capture->ReleaseBuffer(count)); Check(capture->GetNextPacketSize(&packet));
                }
                if (Clock::now() - window >= std::chrono::seconds(1)) sample();
            }
            sample();
        }
        Check(client->Stop());
        emit("collector.stopped", ",\"status\":\"COMPLETE\",\"frames\":" + std::to_string(totalFrames));
        result = 0;
    } catch (HRESULT error) {
        try { emit("collector.error", ",\"hresult\":" + std::to_string(static_cast<std::uint32_t>(error))); } catch (...) {}
    } catch (...) {
        try { emit("collector.error", ",\"reason\":\"unexpected_exception\""); } catch (...) {}
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
