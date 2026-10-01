"""Derive an isolated, pinned WebRTC crypto repair without editing the SDK.

Only generated build inputs are written. No key material is used by this tool.
The source retains its upstream license. Every rewrite checks its match count.
"""
from pathlib import Path
import argparse
import hashlib
import json


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError("pinned source pattern mismatch")
    return text.replace(old, new)


def prepare(source: Path, header: Path, output: Path):
    cc = source.read_text(encoding="utf-8")
    hh = header.read_text(encoding="utf-8")
    if hashlib.sha256(hh.encode()).hexdigest() != "28e71383afa22427486f9682a298bff5db17361882e4f8ae2e94e43897a6e078":
        raise ValueError("unexpected backend header revision")
    if hashlib.sha256(cc.encode()).hexdigest() != "18217ca3c1edf2615e5dd7a0f027735c1b3cd24fc349f6c4de1a1c0283eeca6d":
        raise ValueError("unexpected backend revision")
    hh = replace(hh, '#include <unordered_map>', '#include <atomic>\n#include <unordered_map>')
    hh = replace(hh, '  ~FrameCryptorTransformer();',
        '  ~FrameCryptorTransformer();\n'
        '  uint64_t DroppedReceiverFrames() const { return dropped_receiver_frames_.load(); }')
    hh = replace(hh, '  FrameCryptionState last_dec_error_ = FrameCryptionState::kNew;',
        '  FrameCryptionState last_dec_error_ = FrameCryptionState::kNew;\n'
        '  std::atomic<unsigned> queued_receiver_frames_{0};\n'
        '  std::atomic<uint64_t> dropped_receiver_frames_{0};')
    cc = replace(cc, '      thread_->PostTask([frame = std::move(frame), this]() mutable {\n'
                     '        decryptFrame(std::move(frame));\n      });',
        '      // Bound expensive speculative decrypt work: one running frame and\n'
        '      // one pending frame. Never accumulate seconds of obsolete ciphertext.\n'
        '      if (queued_receiver_frames_.fetch_add(1) >= 2) {\n'
        '        queued_receiver_frames_.fetch_sub(1);\n'
        '        dropped_receiver_frames_.fetch_add(1);\n'
        '        return;\n'
        '      }\n'
        '      thread_->PostTask([frame = std::move(frame), this]() mutable {\n'
        '        decryptFrame(std::move(frame));\n'
        '        queued_receiver_frames_.fetch_sub(1);\n'
        '      });')
    hh = replace(hh, '        key_ring_size(copy.key_ring_size),',
        '        key_ring_size(copy.key_ring_size),\n        discard_frame_when_cryptor_not_ready(copy.discard_frame_when_cryptor_not_ready),')
    hh = replace(hh, '  webrtc::scoped_refptr<ParticipantKeyHandler> Clone() {',
        '  webrtc::scoped_refptr<ParticipantKeyHandler> Clone() {\n    webrtc::MutexLock lock(&mutex_);')
    hh = replace(hh, '    return crypto_key_ring_[key_index != -1 ? key_index : current_key_index_];',
        '    const int index = key_index == -1 ? current_key_index_ : key_index;\n'
        '    if (index < 0 || static_cast<size_t>(index) >= crypto_key_ring_.size()) return nullptr;\n'
        '    return crypto_key_ring_[index];')
    marker = '  bool DecryptionFailure() {'
    helpers = '''  // KeySet objects are immutable snapshots. Pointer identity prevents ABA
  // even when an application explicitly reinstalls the same material.
  bool IsCurrentKey(const webrtc::scoped_refptr<KeySet>& expected, int index) {
    webrtc::MutexLock lock(&mutex_);
    if (index == -1) index = current_key_index_;
    return index >= 0 && static_cast<size_t>(index) < crypto_key_ring_.size() &&
           expected && crypto_key_ring_[index] == expected;
  }

  bool CommitRatchetedKey(const webrtc::scoped_refptr<KeySet>& expected,
                         const webrtc::scoped_refptr<KeySet>& candidate, int index) {
    webrtc::MutexLock lock(&mutex_);
    if (index == -1) index = current_key_index_;
    if (index < 0 || static_cast<size_t>(index) >= crypto_key_ring_.size() ||
        !expected || !candidate || crypto_key_ring_[index] != expected) return false;
    crypto_key_ring_[index] = candidate;
    current_key_index_ = index;
    has_valid_key_ = true;
    decryption_failure_count_ = 0;
    return true;
  }

  bool DecryptionFailureForKey(const webrtc::scoped_refptr<KeySet>& expected, int index) {
    webrtc::MutexLock lock(&mutex_);
    if (index == -1) index = current_key_index_;
    if (index < 0 || static_cast<size_t>(index) >= crypto_key_ring_.size() ||
        !expected || crypto_key_ring_[index] != expected ||
        key_provider_->options().failure_tolerance < 0) return false;
    if (++decryption_failure_count_ > key_provider_->options().failure_tolerance) {
      has_valid_key_ = false;
      return true;
    }
    return false;
  }

'''
    hh = replace(hh, marker, helpers + marker)
    hh = replace(hh, '    SetKeyFromMaterial(new_material,\n                       key_index != -1 ? key_index : current_key_index_);\n    SetHasValidKey();\n    return new_material;',
        '    auto candidate = DeriveKeys(new_material, key_provider_->options().ratchet_salt, 128);\n'
        '    return CommitRatchetedKey(key_set, candidate, key_index) ? new_material : std::vector<uint8_t>{};')
    hh = replace(hh, '    SetKeyFromMaterial(password, key_index);\n    SetHasValidKey();',
        '    SetKeyFromMaterial(password, key_index);')
    hh = replace(hh, '        DeriveKeys(password, key_provider_->options().ratchet_salt, 128);\n  }',
        '        DeriveKeys(password, key_provider_->options().ratchet_salt, 128);\n'
        '    has_valid_key_ = true;\n    decryption_failure_count_ = 0;\n  }')
    hh = replace(hh, '    auto new_key = it->second->RatchetKey(key_index);',
        '    auto new_key = it->second->RatchetKey(key_index);\n    if (new_key.empty()) return {};')
    # These upstream diagnostics contain secrets even at informational level.
    for prefix in ['  RTC_LOG(LS_INFO) << "secret "', '  RTC_LOG(LS_INFO) << "raw_key "']:
        begin = cc.index(prefix)
        end = cc.index(';', begin) + 1
        cc = cc[:begin] + '  // Secret-bearing upstream diagnostic removed.' + cc[end:]
    cc = replace(cc, '  auto initialKeyMaterial = key_set->material;\n', '', 2)
    begin_marker = '      /* Since the key it is first send and only afterwards actually used for'
    for _ in range(2):
        begin = cc.index(begin_marker)
        end = cc.index('        key_handler->SetKeyFromMaterial(initialKeyMaterial, key_index);\n      }', begin)
        end += len('        key_handler->SetKeyFromMaterial(initialKeyMaterial, key_index);\n      }')
        cc = cc[:begin] + '      // Speculative ratchets never mutated the ring: no rollback is needed.' + cc[end:]
    commit = '          key_handler->SetKeyFromMaterial(new_material, key_index);\n          key_handler->SetHasValidKey();'
    # Media returns void; DataPacket returns an RTCErrorOr. Keep them explicit.
    if cc.count(commit) == 2:
        cc = cc.replace(commit,
            '          if (!key_handler->CommitRatchetedKey(key_set, ratcheted_key_set, key_index)) return;\n'
            '          key_set = ratcheted_key_set;', 1)
        cc = replace(cc, commit,
            '          if (!key_handler->CommitRatchetedKey(key_set, ratcheted_key_set, key_index))\n'
            '            return RTCError(RTCErrorType::INVALID_STATE, "key generation changed");\n'
            '          key_set = ratcheted_key_set;')
    else:
        raise ValueError("unexpected ratchet commit count")
    cc = replace(cc, '    if (key_handler->DecryptionFailure()) {',
        '    if (key_handler->DecryptionFailureForKey(key_set, key_index)) {')
    cc = replace(cc, '  webrtc::Buffer payload(buffer.data(), buffer.size());',
        '  if (!key_handler->IsCurrentKey(key_set, key_index)) return;\n\n'
        '  webrtc::Buffer payload(buffer.data(), buffer.size());')
    cc = replace(cc, '  if (decryption_success) {\n    return buffer;',
        '  if (decryption_success && key_handler->IsCurrentKey(key_set, key_index)) {\n    return buffer;')
    # IV bytes are already explicit in the upstream wire format. Only their
    # allocation lifetime changes: one bounded process sequence for media/data.
    cc = replace(cc, '#include "api/crypto/frame_crypto_transformer.h"',
        '#include "api/crypto/frame_crypto_transformer.h"\n'
        '#include "api/crypto/cohavora_iv_sequence.h"')
    helper = """namespace {
bool SeedCohavoraIv(cohavora_e2ee::IvSequence::Iv& bytes) {
  return RAND_bytes(bytes.data(), bytes.size()) == 1;
}
webrtc::Buffer NextCohavoraIv() {
  static cohavora_e2ee::IvSequence sequence(SeedCohavoraIv);
  cohavora_e2ee::IvSequence::Iv bytes;
  if (!sequence.Next(bytes)) return webrtc::Buffer();
  return webrtc::Buffer(bytes.data(), bytes.size());
}
}  // namespace

"""
    cc = replace(cc, 'enum class EncryptOrDecrypt', helper + 'enum class EncryptOrDecrypt')
    for signature in ['webrtc::Buffer FrameCryptorTransformer::makeIv(uint32_t ssrc, uint32_t timestamp)',
                      'webrtc::Buffer DataPacketCryptor::makeIv(uint32_t timestamp)']:
        start = cc.index(signature + ' {')
        end = cc.index('\n}', start) + 2
        unused = '  (void)ssrc;\n' if 'ssrc' in signature else '  (void)send_count_;  // Retain upstream ABI layout.\n'
        cc = cc[:start] + signature + ' {\n' + unused + '  (void)timestamp;\n  return NextCohavoraIv();\n}' + cc[end:]
    cc = replace(cc, '  webrtc::Buffer iv = makeIv(frame->GetSsrc(), frame->GetTimestamp());',
        '  webrtc::Buffer iv = makeIv(frame->GetSsrc(), frame->GetTimestamp());\n'
        '  if (iv.size() != getIvSize()) {\n'
        '    if (last_enc_error_ != FrameCryptionState::kEncryptionFailed) {\n'
        '      last_enc_error_ = FrameCryptionState::kEncryptionFailed;\n'
        '      onFrameCryptionStateChanged(last_enc_error_);\n'
        '    }\n    return;\n  }')
    cc = replace(cc, '  auto iv = makeIv(timestamp);  // for data packets, ssrc is always 0',
        '  auto iv = makeIv(timestamp);\n'
        '  if (iv.size() != 12)\n'
        '    return RTCError(RTCErrorType::INTERNAL_ERROR, "IV allocation unavailable");')
    iv_source = Path(__file__).resolve().parents[2] / 'probes/e2ee_backend_patch/cohavora_iv_sequence.h'
    iv_header = output / 'include/api/crypto/cohavora_iv_sequence.h'
    iv_header.parent.mkdir(parents=True, exist_ok=True)
    iv_header.write_bytes(iv_source.read_bytes())
    include = output / 'include/api/crypto/frame_crypto_transformer.h'
    include.parent.mkdir(parents=True, exist_ok=True)
    include.write_text(hh, encoding='utf-8', newline='\n')
    (output / 'frame_crypto_transformer.cc').write_text(cc, encoding='utf-8', newline='\n')
    manifest = {'upstream_commit': 'aaeeee8077eb0a4cad1c9494e9c6433c751ef663',
                'repair': 'conditional ratchet commit; bounded receiver queue; no secret logging; process-wide bounded IV sequence',
                'iv_header_sha256': hashlib.sha256(iv_source.read_bytes()).hexdigest(),
                'source_sha256': hashlib.sha256(cc.encode()).hexdigest(),
                'header_sha256': hashlib.sha256(hh.encode()).hexdigest()}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(manifest))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--header', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.header, args.output)
