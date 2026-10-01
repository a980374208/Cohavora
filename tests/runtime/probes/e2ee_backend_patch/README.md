# E2EE backend repair validation

This independent project tests the pinned WebRTC crypto repair without editing
`deps/webrtc`, the Flutter reference checkout, or the official release DLL.
It does not establish a product or Flutter runtime verdict.

Pinned WebRTC revision: `aaeeee8077eb0a4cad1c9494e9c6433c751ef663`.
The preparation tool validates normalized source/header SHA-256 before deriving
build inputs. The generated manifest records the resulting fingerprints.

From the repository root:

```powershell
python tests/runtime/tools/media/prepare_e2ee_backend.py --source build-e2ee-backend/upstream/native_frame_crypto_transformer.cc --header build-e2ee-backend/upstream/native_frame_crypto_transformer.h --output build-e2ee-backend/patched
cmake -S tests/runtime/probes/e2ee_backend_patch -B build-e2ee-backend/cmake -G "Visual Studio 18 2026" -A x64
cmake --build build-e2ee-backend/cmake --config RelWithDebInfo --target test_e2ee_key_epoch
ctest --test-dir build-e2ee-backend/cmake -C RelWithDebInfo --output-on-failure
```

The test forces key installation between snapshot capture and ratchet completion
using promises. Both actual encoded-audio and DataPacket decrypt paths reject
stale success/failure commits. It also covers repeated same-slot replacement,
the last candidate in the default ratchet window, and wrong-key recovery.

Native product probes can opt into the generated repair with
`-DCOHAVORA_E2EE_PATCH_DIR=<absolute generated directory>`. This is a validation
configuration; an unconfigured product build still consumes the packaged SDK.
BoringSSL prefix definitions and headers remain private to the replacement
object target.

Flutter requires a full matching backend build. The wrapper is pinned to
`libwebrtc.m144.7559.09` / `557808919a58fb93b0569d7e0d149312db637a91`.
Its custom-audio patch changes ABI, so its headers cannot be paired with the
existing unpatched native archive. After syncing the pinned WebRTC dependencies
under `build-e2ee-backend/full/src`, the preparation script applies the official
audio/build patches and copies the crypto repair into that task-owned checkout:

```powershell
python tests/runtime/tools/media/prepare_e2ee_flutter_backend.py
```

Use the wrapper's official GN configuration, Release with `symbol_level=1`.
The exact successful args are retained in `full/src/out-e2ee-release/args.gn`.
On a shallow dependency clone, generate `build/util/LASTCHANGE` with
`python build/util/lastchange.py --filter . -o build/util/LASTCHANGE` so that its
real pinned commit time is used rather than the zero-time fallback.

After building `libwebrtc`, stage a separate runtime directory:

```powershell
python tests/runtime/tools/media/prepare_e2ee_flutter_backend.py --stage-runtime
python tests/runtime/tools/media/invoke_e2ee_interop.py --mode on --duration 80 --video-source window --codec h264 --interactive-desktop --focus-source --key-action replace --key-rounds 3 --repaired-flutter
```

Staging never replaces the official runtime directory. Each run verifies the
staged EXE/DLL against its provenance and identifies the backend as a derivative.
Every requested key epoch must independently show media and data progress;
counter resets across reconnect generations do not count as recovery.
