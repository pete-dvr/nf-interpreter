# Implementation validation — 20 October 2026

The implementation compiled and passed the host checks below. The original
20 October checks did not flash a board. The 21 October startup correction
below records the subsequent physical USB debugger check. Full hardware
acceptance, successful device-side signature verification,
power-loss recovery, watchdog timing and both rollback directions remain
**NOT RUN**. This is a development validation record, not release approval.

## Source and tools

- Native baseline: `8a962819c18815db3f7e4c55be697b8e1d65cdd5`, with local implementation changes.
- Managed baseline: `47b9f4810098c08be1e543a8f516601125fed9cb`, with local changes.
- ESP-IDF: `v5.5.5`, commit `b774170ff46c393eeb5e495ea37936038d3f4f4f`.
- Container: `ghcr.io/nanoframework/dev-container-esp32@sha256:2a8c73c2c4606e497ffa25914b1b9a1cb57fd653dacfd23a9ad3c4d4332e274b`.
- Compiler: `xtensa-esp-elf-gcc (crosstool-NG esp-14.2.0_20260121) 14.2.0`; CMake 3.31.6.
- Native preset: `PRODUCT_S3_OTA`, `MinSizeRel`, release ID `ota-dev-test`.
- Managed contract: `Product.Firmware` 1.0.0.0, checksum `0xDCFAD239`.

The isolated validation build used an explicit overlay disabling signature
requirements. The checked-in product configuration requires signatures. No
production signing key was generated or supplied, and the resulting package
is **unsigned and for development only**.

## Results

| Check | Result |
| --- | --- |
| Complete ESP32-S3 native build and link | Passed |
| Managed release build script, including regenerated interop metadata | Passed |
| Exact generated interop registration/header comparison | Passed |
| Package/signing host unit tests | 20 passed |
| Actual native engine with deterministic SDK faults | 26 development + 3 production-policy scenarios passed |
| Actual native error-to-CLR HRESULT mapping | Passed |
| Native parser/CRC host tests | Passed |
| Product Kconfig and fixed partition layout | Passed |
| Managed OTA verification | 126 assertions passed |
| Existing Fabric regression verification | 460 assertions passed |
| Linked ELF descriptor versus final managed image | Exact 72-byte match |
| Binary partition tables from SDK and product generator | Identical |
| Explicit empty LittleFS provisioning image | Generated at exact `0x6EE000` size |
| Final package length, hashes and full-file CRC | Passed |

The native fault harness uses real SHA256 and production transaction code, but
its ECDSA boundary fails closed. Its production cases test policy and error
handling. Real P-256 signing/verification is covered by the separate host
package tests; neither replaces a signed-package test on the module.

The native image is `0x155610` bytes, below the `0x1A0000` slot limit by
`0x4A9F0` bytes. The managed image is 301,236 bytes, below `0x2E0000`.
The native trial deadline is 60 seconds, with a 10-second task watchdog.
Staging reserves at least 1 MiB of actual free filesystem space.

## Artifacts

Local native outputs are in `build/product_s3_ota_validation/`.
The package, copied managed image, provenance JSON and verification logs are
in `build/ota-validation/`. These generated directories are ignored by Git.
The managed release script also produced
`SeadragonS3/bin/Release/application.bin` and its package-version manifest in
the managed repository.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `nanoCLR.bin` | 1398288 | `68b72dbfde7233a9e8b78654251ed61720cdbd65a05b8488b3629d706685035c` |
| `application.bin` | 301236 | `cdadd2fe116aaf51ec2a9728554156e6d404da5f7c0c20b797bc6c20a934959b` |
| `bootloader.bin` | 19504 | `0f264f0bc0ecb2004bab036640caf353feea36bc02876e4cbfaa30b39e862009` |
| Binary partition table | 3072 | `44cfc9b98db3115ae0239ac995a6f7099a908e4fda2c50e34ecbcacb68296501` |
| Empty `config.bin` | 7266304 | `8f4cea5cdefb6fbcd88ee7c3cf2cea1fb3dc113c58885379f716effcdb46248a` |
| `update-development.bin` | 1699652 | `408dad79ef8f0797d657148c240b7852bca37fe69a1a891f558c46d7f8b37135` |

The development package's complete-file CRC32 is `B8449A2C`. The historical
filesystem image above is incompatible with the runtime filename limit and
must be regenerated using the 21 October correction below. The empty
filesystem image is only a provisioning test artifact; it contains no device
identity, keys or configuration backup.

Follow the [build and provisioning instructions](README.md) to create a
production-signed release and complete the hardware acceptance record.

## Visual Studio development option — 21 October 2026

`NF_PRODUCT_OTA_ALLOW_VS_DEPLOY=ON` permits replacing the running managed
application through the debugger without rebuilding its native companion.
It is a default-off CMake option, requires the managed debugger and a non-RTM
build, and skips only the running managed hash and erased-tail checks. Package
signatures, incoming payload checks, inactive-slot readback, partition geometry,
trial deadlines and rollback remain enforced. Boot status includes `vs_deploy`.

- Native engine: 96 scenarios passed across all four combinations of signature
  policy and VS deployment mode, compiled with warnings as errors. Includes
  changed, shorter and longer deployments, erased-tail policy, initial/soft
  startup, unchanged trial behavior, and rejected OTA corruption/signatures.
- CMake option: six tests passed against the real module, including child-to-root
  source definition propagation, default-off policy, debugger/RTM rejection, and
  switching an existing build from ON back to OFF.
- Existing package/signature tests: 20 passed. CI discovers both Python suites.

The full `PRODUCT_S3_OTA` / `MinSizeRel` build passed in
`build/product_s3_vs_debug`, with release ID `vs-dev`, the VS deployment option
enabled, USB CDC debugger transport and required OTA package signatures. The
generated compiler command explicitly defines the option as `1` on the actual
OTA engine source. Secure Boot and flash encryption remain disabled.

The separate `factory.bin` contains the complete initial layout with the exact
built bootloader, partition table, runtime, 301,236-byte managed application,
erased OTA metadata and explicitly empty LittleFS image. Every component was
compared with its bytes at the correct merged offset, and all gaps were verified
as erased (`0xFF`). The image replaces all flash contents, including settings.

| Development artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `build/product_s3_vs_debug/nanoCLR.bin` | 1398320 | `0b7bc6818ced904a76436f77bf2e014dcdbad148ea731b194fc244da0a38add7` |
| `build/product_s3_vs_debug/factory.bin` | 16777216 | `3cf54b22e7042b2676150cdf95ea32f867ef61973561b370b1bdc2afa4256fe7` |

The factory image hash above identifies the original image, superseded by the
filesystem correction below. Do not provision that original image.

Actual Visual Studio deployment over USB and full physical-device acceptance remain
**NOT RUN**. Use a confirmed/VALID slot for interactive debugging; an unconfirmed
OTA trial still restarts on a CLR soft reboot and retains its 60-second deadline.

## Physical startup and filesystem correction — 21 October 2026

The initial factory image stopped before starting the USB debugger receiver.
Its LittleFS superblock advertised a maximum filename length of 256, while the
compiled filesystem accepts 255. The SDK's image helper incorrectly used
`CONFIG_LITTLEFS_OBJ_NAME_LEN`, a buffer length including the terminating NUL,
as the on-disk filename limit. The new product CMake wrapper subtracts that NUL
and caps the result at the installed component's `LFS_NAME_MAX`, without
changing runtime SDK configuration.

On the connected ESP32-S3, the bootloader loaded `ota_0`; readback confirmed
sequence 1 / VALID OTA metadata, the original filesystem header, and an exact
runtime digest match. Replacing only the still-empty filesystem at `0x912000`
with the corrected image restored debugger communication. A direct USB wire
protocol Monitor Ping on COM5 returned a 40-byte acknowledged nanoCLR response
(source `0x00010000`). Native firmware and managed deployment were preserved.
Temporary startup instrumentation was removed without being flashed.

All 32 host tests passed, including six new filesystem regressions. They
reproduce rejection of the old image, exercise the real CMake wrapper's scope
and limits, and mount/extract the corrected image to verify seed contents.
This establishes physical startup and USB debugger response. Visual Studio
deployment, device-side signed OTA, rollback and power-loss acceptance still
require their separate hardware checks.

The subsequent read-only capability query used by discovery also succeeded:
command `0x00020008`, capability 7, returned the expected 200-byte metadata
payload, version `0.9.99.999`, target `PRODUCT_S3_OTA` and platform `ESP32`.
After Visual Studio was fully closed, nanoff's read-only device-details query
succeeded and enumerated the managed/native assemblies, including
`Product.Firmware` with checksum `0xDCFAD239`. Its automatic serial watcher also
completed successfully and found one valid device: `PRODUCT_S3_OTA @ COM5`.
The previous nanoff access failure occurred while Visual Studio held the
nanoFramework device lock. After Visual Studio was restarted, the user confirmed
that Device Explorer shows `PRODUCT_S3_OTA @ COM5`. USB discovery is therefore
confirmed in both nanoff and Visual Studio. An actual managed deployment still
requires its separate hardware check.

The corrected build completed, its factory merge was compared against every
component, and its filesystem mounted with the firmware's filename limit.
The regenerated native image exactly matches the pre-diagnostic build bytes.
Current corrected artifacts are:

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `build/product_s3_vs_debug/nanoCLR.bin` | 1398320 | `518c8a34490303fff1aa41d59a619fa5123abccc91051fbc3772111e48e70c4c` |
| `build/product_s3_vs_debug/config.bin` | 7266304 | `6527c5bfbba0cf22dfa4e8ec4358ff36dc3fe3fa82efb964b89d0e0c1ead319c` |
| `build/product_s3_vs_debug/factory.bin` | 16777216 | `2f5a0baf21be036cfca203eec6686f432764e83bb29169f9b4b81f91ae563239` |
