#!/usr/bin/env python3
"""Real depot conversion and Makefile regression, without opening game windows."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def inventory(root):
    result = {}
    for directory, dirs, files in os.walk(root):
        for name in dirs + files:
            path = Path(directory) / name
            st = path.lstat()
            result[str(path.relative_to(root))] = (st.st_mode, st.st_size, st.st_mtime_ns)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    native = Path(__file__).resolve().parents[1]
    source = args.source.resolve()
    layout = json.loads(run(native / 'build/portal2_source', '--describe', source,
                            stdout=subprocess.PIPE, text=True).stdout)
    roots = [Path(p) for p in layout['roots']]
    before = inventory(source)
    with tempfile.TemporaryDirectory(prefix='depot-test-', dir=native / 'build') as folder:
        temp = Path(folder)
        cache = temp / 'cache'
        (cache / 'runtime').mkdir(parents=True)
        run('/usr/bin/ditto', native / 'build/guest-runtime', cache / 'runtime')
        converter = native / 'build/Portal2-Converter.app/Contents/MacOS/Portal2Converter'
        result = run(converter, '--convert', source, temp, '--cache', cache,
                     stdout=subprocess.PIPE, text=True)
        app = Path(result.stdout.strip())
        game = app / 'Contents/SharedSupport/Portal2'
        assert digest(app / 'Contents/SharedSupport/Portal2.image') == digest(Path(layout['image']))
        expected = {}
        for root in roots:
            for directory, dirs, files in os.walk(root):
                dirs[:] = [name for name in dirs if name != '.DepotDownloader']
                for name in files:
                    path = Path(directory) / name
                    expected[path.relative_to(root)] = path
        for relative, original in expected.items():
            copied = game / relative
            assert copied.stat().st_size == original.stat().st_size, relative
            if copied.suffix in ('.dylib', '.icns') or relative.name in ('portal2_osx', 'gameinfo.txt'):
                assert digest(copied) == digest(original), relative
        assert not list(game.rglob('.DepotDownloader'))
        run('/usr/bin/codesign', '--verify', '--deep', '--strict', app)
        env = dict(os.environ, LP32_DYLD_SELFTEST='1')
        env.pop('LP32_GUEST_RUNTIME_DIR', None)
        run('/usr/bin/arch', '-x86_64', app / 'Contents/MacOS/Portal2Compat', env=env)
        print('PASS: real depot conversion, overlay, images, libraries, icon, signature and dependency loading', flush=True)
        # Rebuild this disposable copy with Make. Protect progress and options,
        # and prove that stale binaries from an older layout disappear.
        save = game / 'portal2/SAVE/depot-test-checkpoint'
        config = game / 'portal2/cfg/config.cfg'
        save.parent.mkdir(parents=True, exist_ok=True)
        save.write_text('keep this save')
        config.write_text('keep these settings')
        (game / 'bin/obsolete.dylib').write_text('obsolete binary')
        run('make', '-C', native, 'GAME=portal2', 'SOURCE_GAME=' + str(source), 'BUNDLE=' + str(app), 'bundle')
        assert save.read_text() == 'keep this save'
        assert config.read_text() == 'keep these settings'
        assert not (game / 'bin/obsolete.dylib').exists()
        run('/usr/bin/arch', '-x86_64', app / 'Contents/MacOS/Portal2Compat', env=env)
        assert inventory(source) == before, 'source changed'
        print('PASS: Makefile depot rebuild, stale library cleanup, save/settings preservation, source unchanged', flush=True)


if __name__ == '__main__':
    main()
