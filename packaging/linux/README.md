# Ubuntu packages

The initial package profile targets Ubuntu 24.04 amd64 with GNOME Wayland.
DEB is the installed application; tar.gz contains the same `/usr` layout and
can be unpacked and started with `./usr/bin/cohavora`. Keep the whole tree
together. The launcher locates its payload relative to itself.

Build with the `ubuntu-product` preset and the matching static QtWayland and
WebRTC SDKs described in the repository README. Then, from the repository root:

```sh
python3 packaging/linux/package.py --build-dir out/build/ubuntu-product \
    --qt-source-root /path/to/matching/qt5 \
    --qt-source-root /path/to/matching/qtwayland --parallel 2
```

The source roots must belong to the Qt SDK used by this build. They supply
license notices when those are absent from the SDK prefix. The entry builds the
application, records the recursive Git source-tree identity and build inputs,
then runs CPack. CPack refuses missing notices or stale provenance. These local
metadata files identify the inputs; they are not a source distribution or a
release signature. A published GPL build also needs its corresponding sources
and reproducible dependency/patch materials.

CPack produces local packages in `out/build/ubuntu-product/packages`. The DEB
records dependencies derived from the installed ELF files, plus PipeWire and
the desktop portal. It also produces a separate debug information package.
Install a reviewed DEB using the system package manager, for example
`sudo apt install ./cohavora_<version>_amd64.deb`; start `cohavora` as a normal
desktop user. Remove it with `sudo apt remove cohavora`. User settings and
keyring records require their own explicit removal and are not deleted by a
package removal script.

The application payload is in `/usr/lib/cohavora`, with its renderer modules in
`renderers/`; the launcher and desktop integration are in `/usr/bin` and
`/usr/share`. Static Qt does not make this a fully static executable. Runtime
desktop, GL, font, media and C++ libraries come from Ubuntu, and the installed
ELF files must not retain a development sysroot RPATH. Do not ship glibc,
compositor/portal services, or the development sysroot inside this package.

These build commands alone do not verify a clean-machine installation,
upgrade, signature, or removal. Signed update manifests, package hashes,
APT repository signatures and a complete install/upgrade test are separate
checks. A locally built archive is not a published or release-approved build.

Credential persistence also depends on the desktop Secret Service. The tested
Ubuntu 24.04 package `gnome-keyring 46.1-2ubuntu0.2` crashes when an application
cancels an unlock prompt. Unmodified upstream 51.1 fixes that crash, but its
password prompt can remain visible after this application closes. A local
cancellation-forwarding patch passed separate native and full-application
tests. The Ubuntu 46.1 two-patch candidate also passed private-userland package
install, upgrade, remove and purge diagnostics, including conffile and user
marker preservation. It has not replaced the host package; PAM authentication,
systemd startup and a complete distribution environment remain open release
conditions. For memory-only
login, leave "Remember login" unchecked. The application does not fall back to
plaintext credential storage.

The application package does not ship, activate, or replace a Secret Service
daemon. A distribution fix must preserve Ubuntu's service, PAM and package
integration and be validated separately. Do not copy the private test daemon
or its development library directory into the application payload.

For packaging changes, configure a positive integer Debian revision, for example
`cmake --preset ubuntu-product -DCPACK_DEBIAN_PACKAGE_RELEASE=2`, preserving the
same SDK/toolchain inputs used by the existing build. This produces version
`0.0.1-2` when the application version is `0.0.1`; it does not claim a new
application version. The default revision is `1`. Both DEB control metadata
and the packaged `build-info.json` use the configured revision.

Use `--output-directory /path/to/new-candidates` with `package.py` to keep an
earlier reviewed package intact. An explicitly requested output directory must
be new or empty. A revision change and local package creation are preparation
for upgrade validation, not a release approval or a signed update.

The latest [scoped packaging report](../../samples/acceptance/reports/product/linux-p6-packaging-updates.html)
also verifies the actual DEB in an official Ubuntu 24.04 userspace with its own
loader and runtime libraries: installation, full payload, real account login,
meeting creation, peer-observed continuous microphone audio, UI end/logout,
clean exit and removal. It shares the host kernel and desktop/media services;
independent OS installation, physical media quality and long-run stability
remain separate checks. Ubuntu Base's documentation exclusion was overridden
by explicit Cohavora-only dpkg path-include options for this test.
