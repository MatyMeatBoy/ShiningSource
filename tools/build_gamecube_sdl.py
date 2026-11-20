#!/usr/bin/env python3
"""Prepare the optional SDL3 + libusb runtime for the original GameCube USB adapter on Windows."""

import argparse
import hashlib
import os
import shutil
import subprocess
from pathlib import Path

import build


LIBUSB_VERSION = '1.0.30'
LIBUSB_BIN = f'libusb-{LIBUSB_VERSION}.7z'
LIBUSB_SOURCE = f'libusb-{LIBUSB_VERSION}.tar.bz2'
LIBUSB_RELEASE = f'https://github.com/libusb/libusb/releases/download/v{LIBUSB_VERSION}'


def runtime_files(sdl_sdk):
    """The explicit artifact allowlist. Only an opted-in build requires libusb."""
    override = os.environ.get('SHININGSOURCE_GAMECUBE_SDL')
    if not override:
        return {'SDL3.dll': sdl_sdk/'bin/SDL3.dll'}
    directory = Path(override)
    files = {
        'SDL3.dll': directory/'SDL3.dll',
        'libusb-1.0.dll': directory/'libusb-1.0.dll',
        'libusb-LICENSE.txt': directory/'libusb-LICENSE.txt',
        f'third_party/{LIBUSB_SOURCE}': directory/LIBUSB_SOURCE,
    }
    for path in files.values():
        if not path.is_file():
            raise SystemExit(f'Missing GameCube SDL artifact {path}. Run tools/build_gamecube_sdl.py first.')
    return files


def copy_runtime(sdl_sdk, output):
    for name, path in runtime_files(sdl_sdk).items():
        if name.endswith('.dll'):
            shutil.copy2(path, output/name)


def get_release_file(output, name, sha256):
    path = output/name
    if not path.exists():
        pending = path.with_suffix(path.suffix+'.download')
        subprocess.run(['curl.exe', '-fLsS', f'{LIBUSB_RELEASE}/{name}', '-o', str(pending)], check=True)
        if hashlib.sha256(pending.read_bytes()).hexdigest() != sha256:
            raise SystemExit(f'libusb download checksum differs for {pending}')
        pending.replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != sha256:
        raise SystemExit(f'libusb checksum differs for {path}')
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdl-source', type=Path, required=True, help='SDL 3.4.18 source tree')
    parser.add_argument('--cmake', default='cmake.exe', help='path to CMake if it is not on PATH')
    parser.add_argument('--ninja', default='ninja.exe', help='path to Ninja if it is not on PATH')
    parser.add_argument('--output', type=Path, default=build.ROOT/'build/gamecube-sdl')
    parser.add_argument('-j', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    source = args.sdl_source.resolve()
    version = source/'include/SDL3/SDL_version.h'
    text = version.read_text(encoding='utf-8').splitlines()
    defines = {line.split()[1]: line.split()[2] for line in text if line.startswith('#define SDL_') and len(line.split()) >= 3}
    if tuple(defines.get(f'SDL_{name}_VERSION') for name in ('MAJOR', 'MINOR', 'MICRO')) != ('3', '4', '18'):
        raise SystemExit('The port links against the SDL 3.4.18 SDK; provide the same SDL source version.')

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = get_release_file(output, LIBUSB_BIN, '7fb1dfec805b97983763d7d0ae244320da12add1003d4249c96cc4d586398c79')
    archive = get_release_file(output, LIBUSB_SOURCE, 'fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf')
    usb = output/'usb'
    usb.mkdir(exist_ok=True)
    subprocess.run(['tar.exe', '-xf', str(binary), '-C', str(usb), 'MinGW64', 'include'], check=True)
    license_bytes = subprocess.check_output(['tar.exe', '-xOf', str(archive), f'libusb-{LIBUSB_VERSION}/COPYING'])
    (output/'libusb-LICENSE.txt').write_bytes(license_bytes)

    clang = Path(build.clang()).resolve()
    env = dict(os.environ, PATH=str(clang.parent)+os.pathsep+os.environ.get('PATH', ''))
    sdl = output/'sdl'
    # The libusb import library leaves libusb replaceable as a separate DLL.
    subprocess.run([args.cmake, '-S', str(source), '-B', str(sdl), '-G', 'Ninja',
                    '-DCMAKE_MAKE_PROGRAM='+args.ninja, '-DCMAKE_C_COMPILER='+str(clang),
                    '-DCMAKE_BUILD_TYPE=Release', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF',
                    '-DSDL_HIDAPI_LIBUSB=ON', '-DSDL_HIDAPI_LIBUSB_SHARED=OFF',
                    '-DLibUSB_INCLUDE_PATH='+str(usb/'include'),
                    '-DLibUSB_LIBRARY='+str(usb/'MinGW64/static/libusb-1.0.dll.a')], check=True, env=env)
    subprocess.run([args.cmake, '--build', str(sdl), '--config', 'Release', '--parallel', str(args.j)], check=True, env=env)
    shutil.copy2(sdl/'SDL3.dll', output/'SDL3.dll')
    shutil.copy2(usb/'MinGW64/dll/libusb-1.0.dll', output/'libusb-1.0.dll')
    print(f'Set SHININGSOURCE_GAMECUBE_SDL={output} for Windows builds and packaging.')


if __name__ == '__main__':
    main()
