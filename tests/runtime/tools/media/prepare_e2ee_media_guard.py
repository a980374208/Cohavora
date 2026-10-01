"""Prepare pinned native channel guards without editing the supplied SDK.

Required encryption accepts the application's authenticated transformer path as
well as WebRTC's FrameEncryptor/FrameDecryptor path. A missing hook drops media,
including the interval before OnTrack. No public class layout changes.
"""
import argparse
import hashlib
import json
from pathlib import Path


INPUTS = {
    'audio/channel_receive.cc': '8e1e99c81cf26e586233f87d8b13a4cf71029cfdfb80e5f634f71366b9e9f2ab',
    'audio/channel_send.cc': 'e2246d6eebcc6de221e0b5e332b6bfac108f7390de992d74903e2e2b499e7de8',
    'video/rtp_video_stream_receiver2.cc': 'da43a9e58029a8b36d7d2e097756b0714151ca3cf5f174e70676954c67eafed1',
    'modules/rtp_rtcp/source/rtp_sender_video.cc': '4f9570a2cf3ba318781cc5a470dbdf82804a3d321aa99ae5aa838c0c58f30fed',
}


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('pinned media guard pattern mismatch')
    return text.replace(old, new)


def prepare(source, sdk, output):
    manifest = {'upstream_commit': 'aaeeee8077eb0a4cad1c9494e9c6433c751ef663',
                'scope': 'required crypto before native callback; transformer or frame cryptor',
                'inputs': INPUTS, 'outputs': {}}
    generated = {}
    for name, expected in INPUTS.items():
        text = (source / name).read_text(encoding='utf-8')
        if hashlib.sha256(text.encode()).hexdigest() != expected:
            raise ValueError('unexpected media source revision: ' + name)
        header = str(Path(name).with_suffix('.h'))
        if (source / header).read_text() != (sdk / header).read_text():
            raise ValueError('media header mismatch: ' + header)
        if name.startswith('audio/'):
            text = replace(text, '} else if (crypto_options_.sframe.require_frame_encryption) {',
                '} else if (crypto_options_.sframe.require_frame_encryption &&\n'
                '             !frame_transformer_delegate_) {')
        elif name.startswith('video/'):
            # No unbounded queue waiting for a decryptor that the transformer
            # design will never install. Until its hook arrives, drop frames.
            text = replace(text, 'if (config_.crypto_options.sframe.require_frame_encryption) {',
                'if (config_.crypto_options.sframe.require_frame_encryption && frame_decryptor) {')
            text = replace(text,
                '  } else {\n    OnCompleteFrames(reference_finder_->ManageFrame(std::move(frame)));\n  }',
                '  } else if (!config_.crypto_options.sframe.require_frame_encryption) {\n'
                '    OnCompleteFrames(reference_finder_->ManageFrame(std::move(frame)));\n  }')
            # VideoReceiveStream uses this method to permit keyframe retries.
            # A transformer can recover after key installation; its presence
            # is only a retry capability, never an authentication verdict.
            text = replace(text, '  return frames_decryptable_;',
                '  return frames_decryptable_ || frame_transformer_delegate_ != nullptr;')
        else:
            text = replace(text, '} else if (require_frame_encryption_) {',
                '} else if (require_frame_encryption_ && !frame_transformer_delegate_) {')
            text = replace(text,
                '           "one is required since require_frame_encryptor is set";\n  }',
                '           "one is required since require_frame_encryptor is set";\n'
                '    return false;\n  }')
        generated[name] = text
    output.mkdir(parents=True, exist_ok=True)
    for name, text in generated.items():
        path = output / Path(name).name
        path.write_text(text, encoding='utf-8', newline='\n')
        manifest['outputs'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    (output / 'media-guard-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--sdk-include', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.sdk_include, args.output)
