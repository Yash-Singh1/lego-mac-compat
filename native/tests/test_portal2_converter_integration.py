#!/usr/bin/env python3
"""Exercise the shipped converter without opening any application windows."""
import argparse
import ast
import hashlib
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import tempfile
import time


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def inventory(path):
    # Preserve metadata as well as save bytes. Never follow game-data symlinks.
    result = {}
    for root, dirs, files in os.walk(path):
        for name in dirs + files:
            p = Path(root) / name
            st = p.lstat()
            rel = str(p.relative_to(path))
            save = digest(p) if p.is_file() and not p.is_symlink() and "SAVE" in p.parts else None
            result[rel] = (st.st_mode, st.st_size, st.st_mtime_ns, save)
    return result


def cancel_extraction(command, cache):
    print("Cancelling with the Apple archive mounted…", flush=True)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        for line in process.stderr:
            if line.startswith("Extracting compatibility files"):
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    if any(cache.glob("extract-*/installer/InstallMacOSX.pkg")):
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError("The test never observed the mounted installer")
                process.send_signal(signal.SIGTERM)
                break
        stdout, stderr = process.communicate(timeout=30)
        assert process.returncode == 1 and "cancelled" in stderr, (stdout, stderr, process.returncode)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    assert not list(cache.glob("extract-*")), "Cancelled extraction left staging files"
    mounts = plistlib.loads(run("/usr/bin/hdiutil", "info", "-plist", stdout=subprocess.PIPE).stdout)
    assert str(cache) not in str(mounts), "Cancelled extraction left an image mounted"
    print("PASS: cancellation unmounts the archive and cleans extraction files", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--installer", type=Path, required=True)
    args = parser.parse_args()
    native = Path(__file__).resolve().parents[1]
    converter_app = native / "build/Portal2-Converter.app"
    source = args.source.resolve()
    source_before = inventory(source)
    # Assert that the GUI and Makefile download the exact same runtime.
    constants = {}
    for node in ast.parse((native / "tools/prepare_portal2_runtime.py").read_text()).body:
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name):
            if node.targets[0].id in {"URL", "INSTALLER_SHA256", "LIBRARIES"}:
                constants[node.targets[0].id] = ast.literal_eval(node.value)
    swift = (native / "converter/Conversion.swift").read_text()
    assert constants["URL"] in swift and constants["INSTALLER_SHA256"] in swift
    for name, (member, sha) in constants["LIBRARIES"].items():
        assert all(value in swift for value in (name, member, sha))

    with tempfile.TemporaryDirectory(prefix="converter-test-", dir=native / "build") as folder:
        temp = Path(folder)
        # Move the converter outside its build location to catch repository-path
        # dependencies, and use spaces/quotes/metacharacters in every test path.
        moved = temp / "Converter ' standalone $.app"
        run("/usr/bin/ditto", converter_app, moved)
        executable = moved / "Contents/MacOS/Portal2Converter"
        cache = temp / "cache ' $"
        cache.mkdir()
        run("/bin/cp", "-c", args.installer.resolve(), cache / "InstallMacOSX-Lion.dmg")
        destination = temp / "output ' $"
        destination.mkdir()
        command = [str(executable), "--convert", str(source), str(destination), "--cache", str(cache)]

        cancel_extraction(command, cache)
        assert not list(destination.iterdir()), "Cancelled extraction created an output"

        print("Converting real Portal 2, including runtime extraction…", flush=True)
        result = run(*command, stdout=subprocess.PIPE, text=True)
        output = Path(result.stdout.strip())
        assert output == destination / "Portal2-Compat.app"
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", output)
        contents = output / "Contents"
        info = plistlib.loads((contents / "Info.plist").read_bytes())
        assert info["CFBundleExecutable"] == "Portal2Compat"
        assert digest(contents / "SharedSupport/Portal2.image") == digest(source / "Contents/MacOS/portal2_osx")
        # Signing changes the executable's signature to match the game bundle;
        # exercise its actual image/dependency loading below instead of comparing
        # a whole-file hash to the separately signed bundled loader.
        for name, (_, sha) in constants["LIBRARIES"].items():
            assert digest(contents / "SharedSupport/Portal2/compat-runtime" / name) == sha
        for root, _, files in os.walk(source / "Contents/MacOS"):
            for name in files:
                p = Path(root) / name
                copied = contents / "SharedSupport/Portal2" / p.relative_to(source / "Contents/MacOS")
                assert copied.stat().st_size == p.stat().st_size, f"Incorrect copy: {p}"
                if "SAVE" in p.parts:
                    assert digest(copied) == digest(p), f"Save mismatch: {p}"

        env = dict(os.environ, LP32_DYLD_SELFTEST="1")
        loader_check = run("/usr/bin/arch", "-x86_64", contents / "MacOS/Portal2Compat",
                           contents / "SharedSupport/Portal2.image", env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(loader_check.stdout[-2500:], flush=True)
        # Stand in for a user's newer checkpoint in a previous converted app.
        save = contents / "SharedSupport/Portal2/portal2/SAVE/converter-test-checkpoint"
        save.parent.mkdir(parents=True, exist_ok=True)
        save.write_text("Keep this progress intact.\n")
        previous_before = inventory(output)
        cached_before = {name: (cache / "runtime" / name).stat().st_mtime_ns for name in constants["LIBRARIES"]}

        print("Repeating with cached libraries and an existing output…", flush=True)
        repeated = run(*command, stdout=subprocess.PIPE, text=True)
        second = Path(repeated.stdout.strip())
        assert second.name == "Portal2-Compat 2.app"
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", second)
        assert inventory(output) == previous_before, "Previous converted copy changed"
        assert cached_before == {name: (cache / "runtime" / name).stat().st_mtime_ns for name in constants["LIBRARIES"]}

        print("Cancelling while game files are being copied…", flush=True)
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            for line in process.stderr:
                if line.startswith("Copying Portal 2"):
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        if any(destination.glob(".portal2-converting-*/Portal2-Compat.app/Contents/SharedSupport/Portal2/*")):
                            break
                        time.sleep(0.05)
                    process.send_signal(signal.SIGTERM)
                    break
            stdout, stderr = process.communicate(timeout=20)
            assert process.returncode == 1 and "cancelled" in stderr, (stdout, stderr, process.returncode)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        assert sorted(p.name for p in destination.iterdir()) == ["Portal2-Compat 2.app", "Portal2-Compat.app"]
        assert not list(cache.glob("extract-*")), "Extraction staging was not cleaned up"
        mounts = plistlib.loads(run("/usr/bin/hdiutil", "info", "-plist", stdout=subprocess.PIPE).stdout)
        assert str(temp) not in str(mounts), "An extraction image is still mounted"
        assert inventory(output) == previous_before, "Cancellation changed the previous output"
        assert inventory(source) == source_before, "Original app or saves changed"
        print("PASS: extraction, relocated app, real conversion, signature, loader, cache reuse, collision, cancellation and saves", flush=True)


if __name__ == "__main__":
    main()
