# EmuOrbit PSP component

<!-- doc-status: reference; reviewed: 2026-10-21 -->

Pinned PPSSPP source, the EmuOrbit native adapter, its minimal JNI interface,
and the Android ARM64 build recipe. This is a component-source repository,
not the EmuOrbit application or an APK download.

## Source and licenses

- PPSSPP **v1.20.4**, commit
  `fa50bb1976065c4f8b1b47af227d367fe9771555`, is recorded in the `core` Git
  submodule. Its own submodules retain their upstream pins.
- The EmuOrbit adapter, minimal JNI interface and build recipes provided here
  are licensed under **GPL-2.0-or-later**. Preserve the original per-file notices.
- [LICENSE.TXT](LICENSE.TXT) is the original PPSSPP license/PSPSDK notice,
  retained byte-for-byte. Dependencies and assets have their own terms; the
  build stages their original notices separately from runtime assets.
- `source-package.json` records the reviewed integration-file SHA-256 values,
  upstream/submodule pins and the single derived GLES change. No unmodified
  upstream binary is claimed for a core containing that adaptation.

Publishing this component does not license or certify the private application
frontend or establish compliance of a combined application distribution.
No games, BIOS, saves, credentials, signing material or binaries are included.

## Obtain the pinned sources

```sh
git clone https://github.com/MateusSouzaAlves/psp-emuOrbit.git
cd psp-emuOrbit
git submodule update --init --recursive core
git -C core rev-parse HEAD
```

The last command must return the exact PPSSPP commit above. The build refuses
a modified source checkout or a different pin. The upstream checkout remains
unchanged: one derived translation unit is generated outside it for the reviewed
GLES clip-distance condition. See `scripts/psp-product/core_gl_compat.py`.

## Build the component

Install Git, Python 3, Android SDK CMake **3.22.1** and NDK **29.0.14206865**.
From this repository root, provide absolute local paths:

```sh
python -B scripts/psp-product/build_product.py --source <absolute-core-path> --cache <absolute-native-cache-path> --output <absolute-fresh-component-output-path> --sdk <absolute-android-sdk-path> --jobs 2
```

Use dedicated cache/output folders outside `core`; do not reuse a populated
output folder for changed inputs. The recipe does not download a toolchain or
use the private Android frontend. The minimal Java interface is source for the
host integration, not a standalone Android application.

Outputs include `jniLibs/arm64-v8a`, runtime assets, original notices with
hash manifests, and `product-build.json` with source identities and derived
core provenance. `original_sha256` and `packaged_sha256` distinguish build
and stripped delivery binaries. Generated outputs remain local.

Source availability, byte correspondence, save compatibility, external-service
transport and approval for distribution are separate checks. This repository
does not promise universal game/device compatibility or qualify the full app.
