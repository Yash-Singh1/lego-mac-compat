#!/usr/bin/env python3
"""Package a built GLMetal driver and its matching native shader compiler."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

DEFAULT_BUILD = Path(__file__).resolve().parents[1] / 'glmetal/build'
FILES = ('libGLMetal.dylib', 'glmetal-compiler')


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def validate(build_dir):
    build_dir = Path(build_dir)
    for name in FILES:
        path = build_dir / name
        if not path.is_file() or not os.access(path, os.X_OK):
            raise ValueError(f'Missing executable GLMetal artifact: {path}. Build GLMetal with make -C native/glmetal lib.')
    run('lipo', build_dir / FILES[0], '-verify_arch', 'x86_64', 'arm64')
    run('lipo', build_dir / FILES[1], '-verify_arch', 'arm64')


def install(build_dir, destination, manifest_path=None):
    validate(build_dir)
    build_dir, destination = Path(build_dir), Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    manifest = {'files': {}}
    for name in FILES:
        source, target = build_dir / name, destination / name
        source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        shutil.copy2(source, target)
        if hashlib.sha256(target.read_bytes()).hexdigest() != source_hash:
            raise ValueError(f'GLMetal changed while packaging {name}; retry the build.')
        target.chmod(0o755)
        run('codesign', '--force', '--sign', '-', target)
        run('codesign', '--verify', '--strict', '--all-architectures', target)
        manifest['files'][name] = {
            'source_sha256': source_hash,
            'bundled_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
        }
    manifest_path = Path(manifest_path) if manifest_path else destination / 'build-info.json'
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')


def configure(info):
    environment = dict(info.get('LSEnvironment', {}))
    environment['LP32_GL_BACKEND'] = 'metal'
    # A generated app must not inherit a developer's absolute driver path.
    environment.pop('LP32_GLMETAL_PATH', None)
    info['LSEnvironment'] = environment
    info['LP32GLMetal'] = True


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=DEFAULT_BUILD)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args()
    install(args.build, args.destination, args.manifest)
