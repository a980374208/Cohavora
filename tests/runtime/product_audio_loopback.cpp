// Independent Windows process-loopback witness. No PCM is persisted. This
// observes the selected product's render stream, not microphone or other apps.
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};

class Activation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase> {
public:
    Handle done{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> unknown;
        HRESULT activated = E_FAIL;
        result = operation->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(result)) result = activated;
        if (SUCCEEDED(result)) result = unknown.As(&client);
        SetEvent(done.value);
        return S_OK;
    }
};

static void Check(HRESULT result) { if (FAILED(result)) throw result; }

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2; // pid, run_id, new output, maximum seconds
    const auto pid = std::wcstoul(argv[1], nullptr, 10);
    const std::wstring wide_run(argv[2]);
    const std::string run(wide_run.begin(), wide_run.end());
    const auto seconds = std::wcstoul(argv[4], nullptr, 10);
    const std::filesystem::path destination(argv[3]);
    if (!pid || seconds < 1 || seconds > 30000 || run.size() != 32 ||
        !std::all_of(run.begin(), run.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        std::filesystem::exists(destination)) return 2;
    std::ofstream out(destination, std::ios::binary);
    if (!out) return 2;
    std::uint64_t sequence = 0;
    auto emit = [&](const char* event, const std::string& fields) {
        const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out << "{\"schema\":1,\"collector\":\"wasapi_process_loopback\",\"run_id\":\""
            << run << "\",\"sequence\":" << ++sequence << ",\"utc_ms\":" << utc
            << ",\"pid\":" << pid << ",\"event\":\"" << event << "\"" << fields << "}\n";
        out.flush();
    };
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result = 1;
    try {
        Check(initialized);
        Handle process{OpenProcess(SYNCHRONIZE, FALSE, pid)};
        if (!process.value) Check(HRESULT_FROM_WIN32(GetLastError()));
        AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = pid;
        parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT blob{};
        blob.vt = VT_BLOB;
        blob.blob.cbSize = sizeof(parameters);
        blob.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
        auto completion = Microsoft::WRL::Make<Activation>();
        if (!completion || !completion->done.value) Check(E_OUTOFMEMORY);
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        Check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient), &blob, completion.Get(), &operation));
        if (WaitForSingleObject(completion->done.value, 10000) != WAIT_OBJECT_0)
            Check(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        Check(completion->result);
        const auto client = completion->client;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            200000, 0, &format, nullptr));
        Handle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        if (!ready.value) Check(HRESULT_FROM_WIN32(GetLastError()));
        Check(client->SetEventHandle(ready.value));
        ComPtr<IAudioCaptureClient> capture;
        Check(client->GetService(IID_PPV_ARGS(&capture)));
        Check(client->Start());
        emit("collector.started", ",\"sample_rate\":48000,\"channels\":2,\"pcm_persisted\":false");
        const auto began = Clock::now();
        auto window = began, last_packet = began;
        std::uint64_t total_frames = 0, frames = 0, signal_frames = 0;
        std::uint64_t discontinuities = 0, timestamp_errors = 0;
        double sum_squares = 0, peak = 0, max_gap_ms = 0;
        bool process_exited = false;
        auto sample = [&] {
            const auto now = Clock::now();
            const auto elapsed = std::chrono::duration<double>(now - window).count();
            max_gap_ms = (std::max)(max_gap_ms,
                std::chrono::duration<double, std::milli>(now - last_packet).count());
            const auto rms = frames ? std::sqrt(sum_squares / (frames * 2)) : 0;
            emit("audio.sample", ",\"frames\":" + std::to_string(total_frames) +
                ",\"window_frames\":" + std::to_string(frames) +
                ",\"window_signal_frames\":" + std::to_string(signal_frames) +
                ",\"window_seconds\":" + std::to_string(elapsed) +
                ",\"max_packet_gap_ms\":" + std::to_string(max_gap_ms) +
                ",\"rms\":" + std::to_string(rms) + ",\"peak\":" + std::to_string(peak) +
                ",\"discontinuities\":" + std::to_string(discontinuities) +
                ",\"timestamp_errors\":" + std::to_string(timestamp_errors));
            window = now; frames = signal_frames = 0;
            sum_squares = peak = max_gap_ms = 0;
        };
        while (Clock::now() - began < std::chrono::seconds(seconds)) {
            if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) {
                process_exited = true;
                break;
            }
            WaitForSingleObject(ready.value, 100);
            UINT32 packet = 0;
            Check(capture->GetNextPacketSize(&packet));
            while (packet) {
                BYTE* data = nullptr;
                UINT32 count = 0;
                DWORD flags = 0;
                Check(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr));
                const auto now = Clock::now();
                max_gap_ms = (std::max)(max_gap_ms,
                    std::chrono::duration<double, std::milli>(now - last_packet).count());
                last_packet = now;
                discontinuities += !!(flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY);
                timestamp_errors += !!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    const auto* samples = reinterpret_cast<const std::int16_t*>(data);
                    for (UINT32 f = 0; f < count; ++f) {
                        double frame_peak = 0;
                        for (unsigned channel = 0; channel < 2; ++channel) {
                            const auto value = samples[f * 2 + channel] / 32768.0;
                            sum_squares += value * value;
                            frame_peak = (std::max)(frame_peak, std::abs(value));
                        }
                        peak = (std::max)(peak, frame_peak);
                        signal_frames += frame_peak > .001;
                    }
                }
                total_frames += count; frames += count;
                Check(capture->ReleaseBuffer(count));
                Check(capture->GetNextPacketSize(&packet));
            }
            if (Clock::now() - window >= std::chrono::seconds(1)) sample();
            if (!out) Check(E_FAIL);
        }
        Check(client->Stop());
        sample();
        emit("collector.stopped", std::string(",\"status\":\"") +
            (process_exited ? "COMPLETE" : "TIMEOUT") + "\"");
        result = process_exited ? 0 : 1;
    } catch (HRESULT error) {
        emit("collector.error", ",\"hresult\":" + std::to_string(static_cast<std::uint32_t>(error)));
    } catch (...) {
        emit("collector.error", ",\"reason\":\"unexpected_exception\"");
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
