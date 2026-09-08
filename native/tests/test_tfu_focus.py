#!/usr/bin/env python3
"""Check real foreground activation of TFU's Cocoa/Carbon game window.

Briefly presents an isolated test app in fullscreen and windowed modes.
No desktop input is injected; activation is the game's normal ShowWindow.
Cmd+Tab and switching Spaces should additionally be checked interactively.
LP32_FOCUS_REAL_SWITCH=1 opts into real desktop Cmd+Tab input and verifies
WindowServer ordering and Cocoa key/main identity; it requires authorization
to switch apps. LP32_FOCUS_RAPID=1 checks short returns. CHECKPOINT, SETTINGS,
AUDIO and REAL_CONTROLLER options cover the normal gameplay configuration.
"""
import datetime
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import tempfile
import time
import uuid

NATIVE = Path(__file__).resolve().parents[1]
OUTPUT = NATIVE / "build/test-tfu-focus"


def main():
    OUTPUT.mkdir(exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix="activation-", dir=OUTPUT))
    identifier = "local.tfu.focus-test." + uuid.uuid4().hex
    domain = identifier + ".preferences"
    contents = case / "TFU Focus Test.app/Contents"
    (contents / "MacOS").mkdir(parents=True)
    loader = contents / "MacOS/TFUFocusTest"
    shutil.copy2(os.environ.get("LP32_TEST_LOADER", NATIVE / "build/game_loader"), loader)
    (contents / "Info.plist").write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier=identifier, CFBundleName="TFU Focus Test",
        CFBundleExecutable="TFUFocusTest", CFBundlePackageType="APPL",
        NSHighResolutionCapable=False, NSPrincipalClass="NSApplication")))
    subprocess.run(["codesign", "--force", "--sign", "-", str(contents.parent)], check=True)
    original = subprocess.check_output(["defaults", "export", "com.aspyr.SWTFU", "-"])
    prefs = plistlib.loads(original)
    assert prefs.get("AspyrEulaAccepted"), "Requires an already accepted game license"
    prefs.update(RegistrationRequestDate=datetime.datetime(3999, 9, 4),
                 PrefsDialogAlways=bool(os.environ.get("LP32_FOCUS_SETTINGS")),
                 GameDisplayDepth=32)
    checkpoint = bool(os.environ.get("LP32_FOCUS_CHECKPOINT"))
    assert not checkpoint or os.environ.get("LP32_FOCUS_REAL_SWITCH"), "Checkpoint mode requires the app-switch test"
    if checkpoint:
        source = Path.home() / "Documents/Aspyr/Star Wars The Force Unleashed/SWTFU.BIN"
        original_save = source.read_bytes()
        target = case / "user-data/Documents/Aspyr/Star Wars The Force Unleashed/SWTFU.BIN"
        target.parent.mkdir(parents=True)
        target.write_bytes(original_save)
    try:
        for fullscreen in (False, True):
            mode = "fullscreen" if fullscreen else "windowed"
            if os.environ.get("LP32_FOCUS_MODE") and os.environ["LP32_FOCUS_MODE"] != mode:
                continue
            report = case / f"{mode}.json"
            prefs["GameDisplayMode"] = int(fullscreen)
            subprocess.run(["defaults", "import", domain, "-"], input=plistlib.dumps(prefs), check=True)
            env = dict(os.environ, LP32_MUTE_AUDIO="1", LP32_TFU_PREFERENCES_ID=domain,
                       LP32_TFU_USER_DATA_ROOT=str(case / "user-data"), LP32_TRACE_CARBON_EVENTS="1",
                       LP32_CARBON_WINDOW_REPORT=str(report), LP32_DIAGNOSTIC_LOG=str(case / f"{mode}-diagnostic.log"))
            env["LP32_CARBON_FOCUS_HISTORY"] = str(case / f"{mode}-history.jsonl")
            if os.environ.get("LP32_FOCUS_AUDIO"):
                env.pop("LP32_MUTE_AUDIO", None)
            if checkpoint:
                pad = case / "pad"
                pad.write_text("0 0 0 0 0 0 0 -1\n")
                env.update(LP32_TFU_TEST_FOREGROUND_INPUT="1", LP32_TFU_TEST_PAD_FILE=str(pad),
                           LP32_AGL_CAPTURE_FRAME=str(case / f"{mode}.ppm"), LP32_AGL_CAPTURE_EVERY="120",
                           LP32_TRACE_MOVIES="1")
            for key in ("LP32_BACKGROUND_TEST", "LP32_HEADLESS", "LP32_BUILD_UI_ONLY"):
                env.pop(key, None)
            with (case / f"{mode}.log").open("w") as log:
                process = subprocess.Popen(["arch", "-x86_64", str(loader),
                    str(NATIVE / "build/TFU-Compat.app/Contents/SharedSupport/TFU.image")],
                    cwd=NATIVE, env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + (90 if prefs["PrefsDialogAlways"] else 15)
                    while time.monotonic() < deadline:
                        assert process.poll() is None, f"Exited {process.returncode}; see {case}"
                        windows = json.loads(report.read_text()) if report.exists() else []
                        visible = [w for w in windows if w["visible"] and not w["backdrop"]]
                        if len(visible) == 1 and all(visible[0][k] for k in ("active", "key", "main", "foreground", "onActiveSpace")):
                            assert visible[0]["fullscreen"] == fullscreen
                            assert visible[0]["collectionBehavior"] & 2, "Game window cannot follow activation across Spaces"
                            assert not any(w["key"] or w["visible"] for w in windows if w["backdrop"])
                            if fullscreen and os.environ.get("LP32_FOCUS_TRACE_SIGNAL"):
                                # Ordinary launches must handle the diagnostic signal;
                                # previously only an env-enabled trace installed it.
                                trace_deadline = time.monotonic() + 5
                                trace_log = case / f"{mode}.log"
                                while "enabled SIGQUIT-triggered GL tracing for context presentation" not in trace_log.read_text():
                                    assert process.poll() is None and time.monotonic() < trace_deadline
                                    time.sleep(.05)
                                process.send_signal(signal.SIGQUIT)
                                while "GL render trace armed" not in trace_log.read_text():
                                    assert process.poll() is None and time.monotonic() < trace_deadline
                                    time.sleep(.05)
                            print(f"PASS {mode}: application active, game window key/main; artifacts: {case}", flush=True)
                            if os.environ.get("LP32_FOCUS_REAL_SWITCH"):
                                delay = float(os.environ.get("LP32_FOCUS_SWITCH_DELAY", "0"))
                                until = time.monotonic() + delay
                                while time.monotonic() < until:
                                    assert process.poll() is None, f"Exited before delayed app switch; see {case}"
                                    time.sleep(.2)
                                if checkpoint:
                                    movie_deadline = time.monotonic() + 60
                                    while "movie stop name=Aspyr.mov" not in (case / f"{mode}.log").read_text():
                                        assert process.poll() is None and time.monotonic() < movie_deadline
                                        time.sleep(.2)
                                    time.sleep(3)
                                    for _ in range(2):
                                        pad.write_text("0 0 0 0 0 0 4096 -1\n")
                                        time.sleep(.2)
                                        pad.write_text("0 0 0 0 0 0 0 -1\n")
                                        time.sleep(4)
                                    time.sleep(15)
                                    from test_tfu_continue import has_health_bar
                                    assert has_health_bar(case / f"{mode}.ppm"), "Checkpoint did not load"
                                    if os.environ.get("LP32_FOCUS_REAL_CONTROLLER"):
                                        pad.unlink()
                                        time.sleep(1)
                                probe = NATIVE / "build/tfu_cmd_tab_probe"
                                assert probe.exists(), "Build tfu_cmd_tab_probe before running real switch tests"
                                with (case / f"{mode}-cmd-tab.jsonl").open("w") as trace:
                                    subprocess.run([str(probe), str(process.pid), str(visible[0]["windowNumber"]), str(report)],
                                                   stdout=trace, check=True)
                                count = 6 if os.environ.get("LP32_FOCUS_RAPID") else 2
                                print(f"PASS {mode}: {count} real Cmd+Tab returns restore front order and Cocoa key/main identity", flush=True)
                            observe = float(os.environ.get("LP32_FOCUS_OBSERVE_SECONDS", "0"))
                            if observe:
                                print(f"App-switch probe pid={process.pid} bundle={contents.parent}; observing {observe}s", flush=True)
                                until = time.monotonic() + observe
                                while time.monotonic() < until:
                                    assert process.poll() is None, f"Exited during app-switch observation; see {case}"
                                    time.sleep(.2)
                            break
                        time.sleep(.1)
                    else:
                        raise AssertionError(f"Game window did not receive focus: {windows}; see {case}")
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    finally:
        subprocess.run(["defaults", "delete", domain], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert plistlib.loads(subprocess.check_output(["defaults", "export", "com.aspyr.SWTFU", "-"])) == plistlib.loads(original)
        if checkpoint:
            assert source.read_bytes() == original_save, "User save changed during isolated test"


if __name__ == "__main__":
    main()
