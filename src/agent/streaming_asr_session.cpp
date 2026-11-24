#include "src/agent/streaming_asr_session.h"

#include "src/agent/streaming_asr_adapters.h"

#include <algorithm>
#include <cmath>

namespace OpenMeeting {

std::vector<int16_t> resamplePcm16Mono(
		const int16_t *samples,
		size_t sampleCount,
		int inputSampleRate,
		int targetSampleRate) {
	if (!samples || sampleCount == 0 || inputSampleRate <= 0 || targetSampleRate <= 0) {
		return {};
	}
	if (inputSampleRate == targetSampleRate) {
		return std::vector<int16_t>(samples, samples + sampleCount);
	}
	const double ratio = static_cast<double>(inputSampleRate) / static_cast<double>(targetSampleRate);
	const size_t outFrames = std::max<size_t>(
		1, static_cast<size_t>(std::llround(static_cast<double>(sampleCount) / ratio)));
	std::vector<int16_t> out(outFrames);
	for (size_t i = 0; i < outFrames; ++i) {
		const double srcPos = static_cast<double>(i) * ratio;
		const size_t idx0 = std::min<size_t>(static_cast<size_t>(srcPos), sampleCount - 1);
		const size_t idx1 = std::min<size_t>(idx0 + 1, sampleCount - 1);
		const double frac = srcPos - static_cast<double>(idx0);
		const double val = static_cast<double>(samples[idx0]) * (1.0 - frac) +
			static_cast<double>(samples[idx1]) * frac;
		out[i] = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(val)), -32768, 32767));
	}
	return out;
}

StreamingAsrIoRuntime &StreamingAsrIoRuntime::instance() {
	static StreamingAsrIoRuntime runtime;
	return runtime;
}

StreamingAsrIoRuntime::StreamingAsrIoRuntime()
	: _sslCtx(asio::ssl::context::tls_client) {
	_sslCtx.set_default_verify_paths();
	_sslCtx.set_verify_mode(asio::ssl::verify_peer);
}

StreamingAsrIoRuntime::~StreamingAsrIoRuntime() {
	_workGuard.reset();
	_ioCtx.stop();
	if (_worker.joinable()) {
		_worker.join();
	}
}

void StreamingAsrIoRuntime::ensureStarted() {
	std::call_once(_startOnce, [this] {
		_workGuard.emplace(asio::make_work_guard(_ioCtx));
		_worker = std::thread([this] {
			_ioCtx.run();
		});
	});
}

std::unique_ptr<IStreamingAsrSession> createStreamingAsrSession(const StreamingAsrConfig &config) {
	std::shared_ptr<BaseWsStreamingAsrSession> inner;
	switch (config.protocol) {
	case AgentSpeechProtocolMode::OpenAiRealtimeWs:
	case AgentSpeechProtocolMode::DashScopeQwenRealtimeWs:
		inner = std::make_shared<OpenAiRealtimeAsrSession>(config);
		break;
	case AgentSpeechProtocolMode::DashScopeInferenceWs:
		inner = std::make_shared<DashScopeInferenceAsrSession>(config);
		break;
	case AgentSpeechProtocolMode::DeepgramWs:
		inner = std::make_shared<DeepgramAsrSession>(config);
		break;
	case AgentSpeechProtocolMode::AssemblyAiV3Ws:
		inner = std::make_shared<AssemblyAiV3AsrSession>(config);
		break;
	case AgentSpeechProtocolMode::VolcengineBigModelWs:
		inner = std::make_shared<VolcengineSaucAsrSession>(config);
		break;
	case AgentSpeechProtocolMode::ElevenLabsScribeWs:
		inner = std::make_shared<ElevenLabsScribeAsrSession>(config);
		break;
	case AgentSpeechProtocolMode::XaiSttWs:
		inner = std::make_shared<XaiSttAsrSession>(config);
		break;
	default:
		return nullptr;
	}
	return std::make_unique<StreamingAsrSessionHandle>(std::move(inner));
}

} // namespace OpenMeeting
