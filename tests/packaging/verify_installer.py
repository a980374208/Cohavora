"""Local validation only: probe InitializeSetup always aborts before installation."""
from pathlib import Path
import os
import shutil
import subprocess

import argparse
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--iscc', type=Path, required=True)
parser.add_argument('--build-dir', type=Path, required=True, help='Release app directory containing renderers/')
parser.add_argument('--output', type=Path, required=True, help='New output directory')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
iscc = args.iscc.resolve(strict=True)
build_dir = args.build_dir.resolve(strict=True)
for backend in ('dx11', 'opengl'):
    (build_dir / 'renderers' / f'cohavora-render-{backend}.dll').resolve(strict=True)
scratch = args.output.resolve()
scratch.mkdir(parents=True, exist_ok=False)
script = root / "packaging/windows/CohavoraInstaller.iss"
code = script.read_text(encoding="utf-8-sig").split("[Code]\n", 1)[1]
code = code.replace(" uninstallonly", "").replace("{app}", "{param:ProbeDir}")
probe = scratch / "rm-probe.iss"
probe.write_text(
    '#define MyAppExeName "Cohavora.exe"\n'
    "[Setup]\nAppName=Cohavora packaging probe\nAppVersion=1.0\n"
    "DefaultDirName={tmp}\\CohavoraPackagingProbe\nPrivilegesRequired=lowest\n"
    "CreateUninstallRegKey=no\nUninstallable=no\nCompression=none\n"
    f"OutputDir={scratch}\nOutputBaseFilename=rm-probe\n"
    "[CustomMessages]\nAppRunningUninstallPrompt=Busy\nAppCheckFailed=Failed\n"
    "[Code]\n" + code + "\n"
    "function InitializeSetup: Boolean;\n"
    "var Allowed, Busy: Boolean; State: String;\n"
    "begin\n"
    "  Allowed := CanRemoveApplication(Busy);\n"
    "  if Allowed then State := 'ALLOW'\n"
    "  else if Busy then State := 'BUSY' else State := 'ERROR';\n"
    "  SaveStringToFile(ExpandConstant('{param:ResultFile}'), State, False);\n"
    "  Result := False;\nend;\n",
    encoding="utf-8-sig",
)
compiled = subprocess.run([str(iscc), str(probe)], capture_output=True)
(scratch / "probe-compile.log").write_bytes(compiled.stdout + compiled.stderr)
assert compiled.returncode == 0, "Probe compile failed; see probe-compile.log"

fixture = scratch / "fixture app"
unrelated = scratch / "other app"
for directory in (fixture, unrelated):
    directory.mkdir(exist_ok=True)
    shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/cmd.exe", directory / "Cohavora.exe")
(fixture / "renderers").mkdir(exist_ok=True)
for backend in ("dx11", "opengl"):
    dll = f"cohavora-render-{backend}.dll"
    shutil.copy2(build_dir / "renderers" / dll, fixture / "renderers" / dll)

def check(name, expected, directory=fixture):
    marker = scratch / f"{name}.result"
    marker.unlink(missing_ok=True)
    subprocess.run(
        [str(scratch / "rm-probe.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
         f"/ProbeDir={directory}", f"/ResultFile={marker}", f"/LOG={scratch / (name + '.log')}"],
        timeout=30,
    )
    actual = marker.read_text(encoding="utf-8-sig") if marker.exists() else "NO_RESULT"
    assert actual == expected, f"{name}: expected {expected}, got {actual}; see {name}.log"
    print(f"PASS {name}: {actual}", flush=True)

check("idle", "ALLOW")
check("missing-files", "ALLOW", scratch / "absent app")
for name, directory, expected in (("same-installation", fixture, "BUSY"),
                                  ("other-installation", unrelated, "ALLOW")):
    process = subprocess.Popen(
        [str(directory / "Cohavora.exe"), "/D", "/Q"], stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW,
    )
    try:
        assert process.poll() is None, "Fixture process exited unexpectedly"
        check(name, expected)
    finally:
        process.communicate(b"exit\r\n", timeout=10)

# Verify preflight failures without compressing or launching a real installer.
for name, args, expected in (
    ("missing-exe", [f"/DAppBuildDir={scratch / 'absent app'}"], "Cohavora.exe is missing"),
    ("missing-renderer", [f"/DAppBuildDir={unrelated}"], "Missing renderers"),
    ("missing-language", [f"/DChineseMessagesFile={scratch / 'absent.isl'}"], "ChineseSimplified.isl is missing"),
):
    result = subprocess.run([str(iscc), "/O-", f"/DAppBuildDir={build_dir}", *args, str(script)], capture_output=True)
    combined = result.stdout + result.stderr
    (scratch / f"{name}.log").write_bytes(combined)
    assert result.returncode != 0 and expected.encode() in combined, f"{name} did not fail as expected"
    print(f"PASS {name}: rejected", flush=True)
