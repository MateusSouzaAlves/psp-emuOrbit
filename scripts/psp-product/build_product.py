"""Rebuild/stage the pinned PSP component for normal Gradle variants.

No proof receipt, prebuilt test .so, guest program or personal file is an input.
Source/cache locations are local configuration; generated payload stays in the
explicit output directory. This recipe does not qualify distribution or saves.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import uuid

from native_payload import allocated, sha, verify_worker_abi
from core_gl_compat import prepare as prepare_gl_compat, contract as gl_compat_contract
from stage_assets import stage as stage_assets
from stage_notices import stage as stage_notices, verify as verify_notices

ROOT = Path(__file__).resolve().parents[2]


def run(args):
    subprocess.run([str(value) for value in args], check=True)


def input_hashes():
    files = [ROOT / 'config/psp-core.proof.json', *sorted((ROOT / 'app/src/main/cpp/psp').glob('*')),
             *(ROOT / 'scripts/psp-product' / name for name in
               ('CMakeLists.txt', 'build_product.py', 'stage_assets.py', 'stage_notices.py', 'native_payload.py', 'core_gl_compat.py', 'core_gl_compat.cmake'))]
    return {path.relative_to(ROOT).as_posix(): sha(path) for path in files if path.is_file()}


def verified_existing(output, inputs, pin):
    receipt_path = output / 'product-build.json'
    if not receipt_path.exists():
        return None
    if receipt_path.is_symlink():
        raise ValueError('PSP generated receipt alias')
    receipt = json.loads(receipt_path.read_text('utf-8'))
    if receipt.get('scope') != 'COMPONENT_BUILD_NOT_PRODUCT_QUALIFICATION' or receipt.get('sources') != inputs or receipt.get('commit') != pin:
        raise ValueError('Use a fresh generated PSP payload directory for changed build inputs')
    if receipt.get('core_adaptations') != [gl_compat_contract()]:
        raise ValueError('PSP derived shader provenance missing or changed')
    for name, binary in receipt['binaries'].items():
        path = output / 'jniLibs/arm64-v8a' / name
        if path.is_symlink() or sha(path) != binary['packaged_sha256']:
            raise ValueError('PSP generated binary changed')
        allocated(path)
    assets = output / 'assets/psp'
    if sha(assets / 'manifest.sha256') != receipt['assets']['manifest_sha256']:
        raise ValueError('PSP generated asset manifest changed')
    for line in (assets / 'manifest.sha256').read_text('utf-8').splitlines():
        digest, name = line.split('  ', 1)
        path = assets / name
        if path.is_symlink() or assets.resolve() not in path.resolve().parents or sha(path) != digest:
            raise ValueError('PSP generated asset changed')
    notices = output / 'assets/licenses/psp'
    def read_notice(name):
        path = notices / name
        if path.is_symlink() or notices.resolve() not in path.resolve().parents:
            raise ValueError('PSP generated notice alias')
        return path.read_bytes()
    verify_notices(read_notice, receipt['notices'],
                   (path.relative_to(notices).as_posix() for path in notices.rglob('*') if path.is_file()))
    return receipt


def require_empty_output(output):
    if output.is_symlink() or getattr(output, 'is_junction', lambda: False)():
        raise ValueError('PSP generated output alias')
    if output.exists() and (not output.is_dir() or next(output.iterdir(), None) is not None):
        raise ValueError('Unpublished PSP payload contains data; choose a fresh generated output directory')


def publish_generation(staging, output):
    if staging.parent != output.parent or not staging.name.startswith(output.name+'.building-'):
        raise ValueError('PSP publication must stay in its generated parent')
    require_empty_output(output)
    if output.exists():
        # Gradle creates an empty declared output directory before Exec. Remove
        # that empty directory only, non-recursively, then publish atomically.
        # rmdir refuses a concurrent writer instead of deleting its contents.
        output.rmdir()
    os.rename(staging,output)


def build(args):
    source, cache, output, sdk = (value.resolve() for value in (args.source, args.cache, args.output, args.sdk))
    if cache == source or cache in source.parents or source in cache.parents or cache == ROOT or cache == Path(cache.anchor):
        raise ValueError('Use a dedicated native cache outside the upstream source')
    if output in (ROOT, source, cache, Path(output.anchor)) or output in source.parents or source in output.parents:
        raise ValueError('Use a dedicated generated PSP payload directory')
    pin = json.loads((ROOT / 'config/psp-core.proof.json').read_text('utf-8'))
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != pin['commit'] or subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--untracked-files=no']):
        raise ValueError('Use the pristine pinned PPSSPP source and matching submodules')
    verify_worker_abi(source)
    ndk = sdk / 'ndk' / pin['ndk']
    properties = (ndk / 'source.properties').read_text('utf-8')
    if not any(line.strip() == 'Pkg.Revision = ' + pin['ndk'] for line in properties.splitlines()):
        raise ValueError('PSP NDK revision mismatch')
    inputs = input_hashes()
    previous = verified_existing(output, inputs, revision)
    if previous:
        print('PSP_COMPONENT_BUILD_REVERIFIED_NOT_PRODUCT_QUALIFIED')
        return
    # An incomplete prior build is not merged into a candidate. Generated output
    # is append-only by input digest; no recursive delete of caller paths.
    require_empty_output(output)
    final_output=output
    cmake = sdk / 'cmake/3.22.1/bin/cmake.exe'
    common = ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(sdk / 'cmake/3.22.1/bin/ninja.exe'),
              '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build/cmake/android.toolchain.cmake'),
              '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-26', '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release']
    # A clone/build directory must not change __FILE__ strings and hence the
    # delivered core digest used by quick-state compatibility. Canonical paths
    # and the immutable commit epoch are build inputs, never user save IDs.
    mappings=' '.join('-ffile-prefix-map="'+path.as_posix()+'='+label+'"'
                      for path,label in ((source,'ppsspp'),(cache,'psp-build'),(ROOT,'emuorbit')))
    derived=cache/'gl-compat/GPU/GLES/ShaderManagerGLES.cpp'
    compatibility=prepare_gl_compat(source,derived)
    # The existing cache -> psp-build prefix map also covers this derived unit.
    # Do not change global flags and unnecessarily rebuild every upstream file.
    common += ['-DCMAKE_C_FLAGS='+mappings, '-DCMAKE_CXX_FLAGS='+mappings]
    epoch=subprocess.check_output(['git','-C',str(source),'show','-s','--format=%ct',revision],text=True).strip()
    if not epoch.isdigit():raise ValueError('Pinned PPSSPP commit epoch unavailable')
    os.environ['SOURCE_DATE_EPOCH']=epoch
    run([cmake, '-S', source, '-B', cache / 'core', *common, '-DLIBRETRO=ON', '-DHEADLESS=OFF',
         '-DUNITTEST=OFF', '-DATLAS_TOOL=OFF', '-DUSING_GLES2=ON', '-DUSING_EGL=ON', '-DUSE_DISCORD=OFF',
         '-DUSE_MINIUPNPC=OFF', '-DUSE_CCACHE=OFF', '-DUSE_SYSTEM_LIBPNG=OFF',
         '-DCMAKE_PROJECT_PPSSPP_INCLUDE='+str(ROOT/'scripts/psp-product/core_gl_compat.cmake'),
         '-DEMUORBIT_PSP_GL_COMPAT_SOURCE='+str(derived),
         '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384'])
    run([cmake, '--build', cache / 'core', '--target', 'ppsspp_libretro', '-j', args.jobs])
    run([cmake, '-S', ROOT / 'scripts/psp-product', '-B', cache / 'adapter', *common, '-DPPSSPP_SOURCE=' + str(source)])
    run([cmake, '--build', cache / 'adapter', '-j', args.jobs])
    # Verify source/adapter inputs again after the build before publishing.
    if inputs != input_hashes() or subprocess.check_output(['git', '-C', str(source), 'diff', 'HEAD', '--']):
        raise ValueError('PSP build inputs changed during compilation')
    # Publish a whole generation only after both libraries, assets and receipt
    # are complete. A failed staging attempt cannot poison the next retry or
    # become an apparently valid generated payload.
    output=final_output.with_name(final_output.name+'.building-'+uuid.uuid4().hex)
    output.mkdir(parents=True, exist_ok=False)
    libraries = output / 'jniLibs/arm64-v8a'
    libraries.mkdir(parents=True)
    strip = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe'
    binaries = {}
    for original, name in ((cache / 'core/ppsspp_libretro_android.so', 'libemuorbit_psp_core.so'),
                           (cache / 'adapter/libemuorbit_psp_session.so', 'libemuorbit_psp_session.so')):
        packaged = libraries / name
        run([strip, '--strip-debug', '-o', packaged, original])
        if allocated(original) != allocated(packaged):
            raise ValueError('PSP strip changed allocated runtime sections')
        binaries[name] = {'original_sha256': sha(original), 'packaged_sha256': sha(packaged), 'bytes': packaged.stat().st_size}
    assets = stage_assets(source, output / 'assets/psp')
    notices = stage_notices(source, output / 'assets/licenses/psp')
    receipt = {'schema': 1, 'scope': 'COMPONENT_BUILD_NOT_PRODUCT_QUALIFICATION', 'commit': revision,
               'abi': pin['abi'], 'api': pin['api'], 'ndk': pin['ndk'], 'sources': inputs,
               'binaries': binaries, 'assets': assets, 'notices': notices, 'core_adaptations': [compatibility], 'distribution_qualified': False}
    temporary = output / ('product-build-' + uuid.uuid4().hex + '.tmp')
    temporary.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, output / 'product-build.json')
    publish_generation(output,final_output)
    print('PSP_COMPONENT_BUILD_PASS_NOT_PRODUCT_QUALIFIED')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('source', 'cache', 'output', 'sdk'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--jobs', type=int, choices=range(1, 9), default=2)
    build(parser.parse_args())
