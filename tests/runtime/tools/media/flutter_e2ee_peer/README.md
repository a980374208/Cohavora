# E2EE Flutter interoperability fixture

This is a task-owned Windows test client, not the product and not the official Flutter example. The product is the Native C++/Qt application. The external SDK checkout and pub cache remain read-only.

Use `invoke_e2ee_interop.py` for protocol tests and `invoke_e2ee_product.py` for the real Qt/Coordinator path. Credentials and random test keys enter child environments; never place them in command arguments, evidence, screenshots, or source files. Runs use independent test rooms and retain previous failures.

## Pinned derivatives

The current Release runtime is selected by `build-e2ee-backend/flutter-repaired-current.json`. Its `backend-provenance.json` binds the backend, Dart AOT, executable, SDK transport patch and optional memory-sampling plugin. This fixture uses explicitly recorded task-owned derivatives; it does not establish that an unmodified upstream Flutter binary passes the same-slot replacement matrix.

The task-owned backend fixes stale key-generation writeback and bounded frame work, preserves the protocol envelope, and shares the v2 IV lifecycle contract with the native package. `prepare_e2ee_flutter_backend.py --stage-runtime` creates a new runtime directory and refuses to replace an existing one.

## Camera content check

`invoke_e2ee_product.py --camera native --camera-content --codec h264 --duration 65` checks native-to-Flutter content; `--camera flutter` reverses the camera publisher and `--codec vp8` selects the other supported codec. These commands require prior device/service authorization.

The optional task-owned plugin continuously samples a bounded 8 by 8 luma summary in memory. An owned worker waits for a new sample without blocking the Flutter platform thread. Summaries travel only over the encrypted test data channel and are never written to evidence. No camera screenshot, image file, or video recording is created. Evidence records only outcomes and mean error. The fixed thresholds are contrast at least 16, mean luma error at most 8, and a 5-second correspondence window; at least three successful samples are required before and after key replacement. Missing samples and errors remain failures. Spatial correspondence is not a perceptual-quality score or final display-pixel guarantee.

Performance runs do not enable this content sampler. Functional validation is distinct from the approved CPU/memory/FPS budget and ten-minute observation. Windows results do not cover mobile or Web clients, automatic key distribution, member revocation, or long-term stability.
