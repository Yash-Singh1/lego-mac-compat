#!/usr/bin/env python3
"""Runs a converted game with a given loader and OpenGL backend, sends
scripted keys, and saves frame captures.  Used to compare LP32GL with
Apple's OpenGL on the same build.

  drive_game.py --source build/MW2-Compat.app --loader build/game_loader \
      --backend mesa --out /tmp/mw2-mesa 50:125 53:36 ...

--source is an existing converted bundle.  It is cloned (copy-on-write) once
under --work (default build/lp32gl-bundles) and the clone gets the loader and
LP32GL, so the source bundle is never modified.  Steps are
'DELAY:KEY[:HOLD[:MODS]]' (KEY '-' only waits).  Captures come from the
loader's LP32_AGL_CAPTURE_FRAME hook every --capture-every swaps.
"""
import argparse
import os
import pathlib
import plistlib
import shutil
import signal
import subprocess
import sys
import time

NATIVE = pathlib.Path(__file__).resolve().parents[2]


def assemble(source: pathlib.Path, work: pathlib.Path, loader: pathlib.Path, lp32gl: pathlib.Path):
    """Clones the source bundle once (APFS copy-on-write: no data is
    duplicated, and games that resolve paths inside their bundle see a real
    one), then installs the loader and LP32GL into the clone."""
    info = plistlib.loads((source / 'Contents/Info.plist').read_bytes())
    executable = info['CFBundleExecutable']
    bundle = work / f'{source.stem}-LP32GL.app'
    if not bundle.exists():
        work.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cp', '-Rc', str(source), str(bundle)], check=True)
    target = bundle / 'Contents/MacOS' / executable
    if target.read_bytes() != loader.read_bytes():
        target.unlink()
        shutil.copy2(loader, target)
    frameworks = bundle / 'Contents/Frameworks/LP32GL'
    if lp32gl.is_dir():
        if frameworks.exists():
            shutil.rmtree(frameworks)
        shutil.copytree(lp32gl, frameworks, ignore=shutil.ignore_patterns('*.dSYM'))
    return bundle, executable


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=pathlib.Path)
    parser.add_argument('--bundle', type=pathlib.Path,
                        help='an already assembled clone to run in place (only its loader is updated)')
    parser.add_argument('--loader', default=NATIVE / 'build/game_loader', type=pathlib.Path)
    parser.add_argument('--lp32gl', default=NATIVE / 'build/lp32gl', type=pathlib.Path)
    parser.add_argument('--work', default=NATIVE / 'build/lp32gl-bundles', type=pathlib.Path)
    parser.add_argument('--backend', choices=['apple', 'mesa', 'metal'], default='mesa')
    parser.add_argument('--glmetal', default=NATIVE / 'glmetal/build/libGLMetal.dylib', type=pathlib.Path,
                        help='GLMetal for --backend metal (loaded in place, not copied)')
    parser.add_argument('--foreground', action='store_true', help='let the game activate and receive normal window events')
    parser.add_argument('--home', type=pathlib.Path, help='isolated home (default: a fresh one under --work)')
    parser.add_argument('--out', required=True, type=pathlib.Path)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--capture-every', default='30')
    parser.add_argument('--env', action='append', default=[], help='K=V added to the environment')
    parser.add_argument('--game-arg', action='append', default=[],
                        help='host process argument, also visible through NSProcessInfo.arguments; repeat as needed')
    parser.add_argument('steps', nargs='*')
    args = parser.parse_args()

    if args.bundle:
        bundle = args.bundle.resolve()
        executable = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())['CFBundleExecutable']
        target = bundle / 'Contents/MacOS' / executable
        loader = args.loader.resolve()
        if target.read_bytes() != loader.read_bytes():
            target.unlink()
            shutil.copy2(loader, target)
        args.source = bundle
    elif args.source:
        bundle, executable = assemble(args.source.resolve(), args.work.resolve(), args.loader.resolve(),
                                      args.lp32gl.resolve())
    else:
        parser.error('--source or --bundle is required')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    home = (args.home or (args.work / f'{args.source.stem}-home')).resolve()
    (home / 'Library/Application Support').mkdir(parents=True, exist_ok=True)
    fifo = out / 'keys'
    if fifo.exists():
        fifo.unlink()
    os.mkfifo(fifo)
    frame = out / 'frame.ppm'
    env = os.environ | {
        'LP32_MUTE_AUDIO': '1', 'LP32_NO_DIAGNOSTIC_LOG': '1',
        'LP32_TEST_HOME_DIR': str(home),
        'LP32_APPLICATION_SUPPORT_DIR': str(home / 'Library/Application Support'),
        'LP32_TEST_KEY_FIFO': str(fifo), 'LP32_AGL_CAPTURE_FRAME': str(frame),
        'LP32_AGL_CAPTURE_EVERY': args.capture_every, 'LP32_GL_BACKEND': args.backend,
    }
    if args.backend == 'metal':
        env['LP32_GLMETAL_PATH'] = str(args.glmetal.resolve())
    if not args.foreground:
        env['LP32_BACKGROUND_TEST'] = '1'
    else:
        env.pop('LP32_BACKGROUND_TEST', None)
    for item in args.env:
        key, value = item.split('=', 1)
        env[key] = value
    log_path = out / 'log.txt'
    with open(log_path, 'w') as log:
        process = subprocess.Popen([str(bundle / 'Contents/MacOS' / executable), *args.game_arg], env=env,
                                   stdout=log, stderr=log, start_new_session=True)
        game_group = process.pid

        def game_running():
            if process.poll() is None:
                return True
            try:
                os.killpg(game_group, 0)
                return True
            except ProcessLookupError:
                return False

        # Read-write so opening never waits for the game to open its end
        # (COD4 never reads the key FIFO).
        keys = os.fdopen(os.open(fifo, os.O_RDWR), 'w')
        deadline = time.time() + args.timeout

        def snap(name):
            if frame.exists():
                subprocess.run(['sips', '-s', 'format', 'png', str(frame), '--out', str(out / f'{name}.png')],
                               capture_output=True)

        for index, step in enumerate(args.steps, 1):
            delay, key, *rest = step.split(':')
            time.sleep(float(delay))
            print(f'step {index} {step} t={time.time():.1f}', flush=True)
            if not game_running():
                print('game exited', flush=True)
                break
            if key != '-':
                hold = rest[0] if rest else '0.1'
                mods = f' {rest[1]}' if len(rest) > 1 else ''
                keys.write(f'{key} {hold}{mods}\n')
                keys.flush()
            time.sleep(1.5)
            snap(f'step{index}')
        while game_running() and time.time() < deadline:
            time.sleep(0.5)
        snap('final')
        if game_running():
            try:
                os.killpg(game_group, signal.SIGTERM)
            except ProcessLookupError:
                pass
            stop = time.time() + 10
            while game_running() and time.time() < stop:
                time.sleep(0.1)
            if game_running():
                try:
                    os.killpg(game_group, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            process.wait()
            print('terminated')
        else:
            print('exit', process.returncode)
    print(log_path.read_text(errors='replace')[-3000:])


if __name__ == '__main__':
    sys.exit(main())
