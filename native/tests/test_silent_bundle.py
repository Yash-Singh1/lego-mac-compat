"""Check bundle-enforced muting before loading a guest or opening audio."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

loader = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="lp32-silent-") as temporary:
    app = Path(temporary) / "SilentTest.app"
    executable = app / "Contents/MacOS/TestLoader"
    executable.parent.mkdir(parents=True)
    shutil.copy2(loader, executable)
    info = {"CFBundleExecutable": "TestLoader", "CFBundleIdentifier": "local.lp32.silent-test",
            "CFBundlePackageType": "APPL"}
    for enabled in (False, True):
        info["LP32SilentTest"] = enabled
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
        subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for inherited in (None, "0"):
            env = {k: v for k, v in os.environ.items() if not k.startswith("LP32_")}
            env["LP32_SILENT_TEST_SELFTEST"] = "1"
            if inherited is not None:
                env["LP32_MUTE_AUDIO"] = inherited
                env["LP32_BACKGROUND_TEST"] = inherited
            result = subprocess.run(["arch", "-x86_64", str(executable)], env=env,
                                    capture_output=True, text=True, timeout=15)
            assert (result.returncode == 0) == enabled, result.stdout + result.stderr
            assert "before guest/audio initialization" in result.stderr
            assert ("silent-test startup: PASS" in result.stderr) == enabled
print("Silent bundle PASS (missing/zero environment overridden; ordinary bundles unchanged)")
