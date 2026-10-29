# Seadragon ESP32-S3 paired OTA releases

This implements the integration guide's `PRDOTA01` format version 1,
`layoutId=2`, target `PR-S3-N16R8-01`. Each release contains a native runtime and
its complete matching managed deployment. The running native slot determines
the managed partition: `ota_0 → deploy_a`, `ota_1 → deploy_b`. Both use `config`.

**Hardware acceptance status: NOT RUN.** Passing the host checks does not
establish that power-loss recovery, rollback, flash writes, watchdog behavior or
the peripheral startup checks work on the physical module. Do not release to
field devices until the acceptance record below is completed.

See the [implementation validation record](VALIDATION.md) for the completed
builds, automated checks and exact development artifact hashes.

## Build order and keys

Use the ESP32-S3-WROOM-1U-N16R8: 16 MiB Quad flash and 8 MiB Octal PSRAM. The
`PRODUCT_S3_OTA` preset inherits the existing `ESP32_S3_OCTAL` setup, retains USB
CDC wire protocol, and uses a separate build directory and SDK defaults.
The OCTAL name describes PSRAM; octal flash must remain disabled.

Both Octal PSRAM defaults enable `CONFIG_SPIRAM_ECC_ENABLE=y`. For this N16R8
module, Espressif specifies a maximum ambient temperature of 85 degrees C with
PSRAM ECC enabled, instead of 65 degrees C without it (see the
[module datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)).
ECC reserves 1/16 of the 8 MiB PSRAM, leaving 7.5 MiB before runtime allocations;
the managed heap automatically uses the available capacity. The rest of the
board must also be rated for its operating temperature.

An existing ESP-IDF `sdkconfig` retains saved settings over new defaults. When
upgrading an existing build, enable `CONFIG_SPIRAM_ECC_ENABLE=y` in that saved
configuration. The product build uses `<build-directory>/sdkconfig`, so a fresh
product build directory also applies the new defaults. Stock `ESP32_S3_OCTAL`
builds use the repository-root `sdkconfig`, even with a new build directory.
Product configuration rejects a saved configuration with ECC disabled.
Rebuild the native runtime and its signed OTA package to apply this change;
deploying only managed code from Visual Studio cannot enable ECC.

`Product.Firmware` 1.2.0.0 exposes
`float DeviceTelemetry.GetInternalTemperatureCelsius()` for the ESP32-S3's
internal temperature sensor. This reports silicon temperature in degrees C,
not the module's ambient temperature rating. The driver automatically switches
its measurement range; the native helper serializes reads and throws a managed
exception on a sensor error. SeadragonS3 sends each successful reading in the
existing HardwareStatus Float32 field. A failed reading is logged and that
status response sends `0` for temperature while preserving its other fields;
the next request retries the sensor normally.

`int DeviceTelemetry.GetPowerStatus()` returns `1` for OK or `2` for a pending
voltage dropout. At hardware boot, after NVS initialization and before CLR
startup, the runtime latches `ESP_RST_BROWNOUT` in the `product_power` NVS
namespace. Normal resets preserve pending events. Reading does not consume
the event. `void DeviceTelemetry.AcknowledgePowerStatus()` clears it only after
the managed client successfully transmits the report; storage errors throw
and are retried. Native initialization survives managed soft restarts, so an
acknowledged brownout is not relatched by Visual Studio deployment.

This reports resets identified by the ESP32 brownout detector; an abrupt full
power loss that is subsequently reported as `ESP_RST_POWERON` cannot be
distinguished from an ordinary power-on by reset cause alone. See Espressif's
[reset reason API](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/system/misc_system_api.html#reset-reason).

The added telemetry methods change the interop contract to checksum
`0xAE9CBE59`. Install its matching native/managed pair through OTA before
deploying applications using the new contract from Visual Studio. Older OTA
slots retain their own matching application and runtime for rollback.

This implementation rejects encrypted partitions, including flash-encrypted
application partitions. Flash encryption needs a separate provisioning and
aligned-write implementation before it can be enabled. Package signing remains
required in production; ESP secure-boot image signing is a separate mechanism.

At implementation time the source baseline was
`8a962819c18815db3f7e4c55be697b8e1d65cdd5`; its ESP-IDF source image selects
`v5.5.5`, and the prebuilt development container is
`ghcr.io/nanoframework/dev-container-esp32:v3.1`. Record the actual commits,
resolved container digest and toolchain versions used for every release.
Tags alone do not record the resolved SDK or container identity.

Production requires a release-signing P-256 private key held outside either
source repository and a matching PEM SubjectPublicKeyInfo public key. Supply
the public key to CMake as `NF_PRODUCT_PUBLIC_KEY`; the generator embeds its
uncompressed SEC1 point in native read-only data. Supply the private key only
to the release packaging command. The package never supplies its own trust
anchor. No production key is included or generated by these tools. Unit tests
generate ephemeral keys in memory.

Arrange key creation and access in the release-signing environment. If an
existing approved unencrypted PEM private key is used, its public PEM can be
exported with:

```sh
openssl pkey -in /secure/release-private.pem -pubout -out /artifacts/release-public.pem
```

The CLI currently reads an unencrypted PEM private key; restrict that file to
the signing environment. Do not put it into a container image, source control,
firmware, CI logs or release artifacts. Key rotation needs a separately
reviewed transition plan; this version trusts exactly one pinned key.

Install host dependencies, including in the firmware build environment:

```sh
python -m pip install -r tools/product_ota/requirements.txt
```

1. Build `SeadragonS3` including `Product.Firmware` and all required managed
   dependencies into the standard `application.bin`. Confirm the managed/native
   API versions and run its verification project. Do not prepend a product
   header to this binary.
2. Generate the descriptor from that exact binary. The following command can
   inspect the generated output independently; the product CMake configuration
   also invokes it automatically:

   ```sh
   python tools/product_ota/product_ota.py descriptor \
     --managed /artifacts/application.bin --release-id 2026.10.20-01 \
     --public-key /artifacts/release-public.pem \
     --output /artifacts/ProductReleaseDescriptor.cpp
   ```

3. Configure and build the matching native image in the ESP32 development
   container. Use a fresh product build directory when introducing the layout.
   Keep the final managed file immutable during this build:

   ```sh
   cmake --preset PRODUCT_S3_OTA \
     -DCMAKE_BUILD_TYPE=MinSizeRel \
     -DNF_PRODUCT_MANAGED_IMAGE=/artifacts/application.bin \
     -DNF_PRODUCT_RELEASE_ID=2026.10.20-01 \
     -DNF_PRODUCT_PUBLIC_KEY=/artifacts/release-public.pem
   cmake --build --preset PRODUCT_S3_OTA
   ```

   CMake regenerates the descriptor when its inputs change and rejects a
   managed image larger than `0x2E0000`. Its native post-build check rejects
   `nanoCLR.bin` larger than `0x1A0000`. If using ESP native secure-boot signing,
   sign the native image first and recheck the size of the final signed image.
   Do not substitute a different native image after packaging.
4. Package the final binaries. The release ID must match the descriptor used
   for the native build. Packaging and verification check the embedded descriptor's
   target, layout, release, managed length and managed hash against the package.
   Rebuild the companion runtime when these inputs change:

   ```sh
   python tools/product_ota/product_ota.py check-runtime \
     --runtime /artifacts/nanoCLR.bin
   python tools/product_ota/product_ota.py package \
     --runtime /artifacts/nanoCLR.bin --managed /artifacts/application.bin \
     --release-id 2026.10.20-01 --private-key /secure/release-private.pem \
     --provenance /artifacts/provenance.json --output /artifacts/update.bin
   python tools/product_ota/product_ota.py verify \
     --package /artifacts/update.bin --public-key /artifacts/release-public.pem \
     --expected-crc HEX_CRC_FROM_MANIFEST
   ```

   Replace `HEX_CRC_FROM_MANIFEST` with `package.crc32` from
   `update.bin.manifest.json`; the CLI accepts eight hex digits or a `0x`
   prefix. The manifest records complete-file length/CRC32/SHA256, both
   payload lengths/hashes, target, layout, release ID, signature status and
   supplied provenance. CRC covers the header, both payloads and the signature.
   It uses CRC-32/ISO-HDLC; `123456789` gives `CBF43926`.
5. Archive `update.bin`, its manifest, both exact payloads, generated descriptor
   and descriptor JSON, the public key, source commits, managed package lock
   data, build configuration, bootloader and binary partition-table hashes.
   Do not archive the private key with a release.

Every managed-only release must rebuild its native companion. The descriptor
contains the managed image hash and length; the outer package contains hashes
of both final payloads. There is no circular hash dependency.

The native installer reads the staged package through bounded 4096-byte VFS
reads. Unbuffered `fread` is unsuitable for the size-optimized ESP32 C library:
it can refill a one-byte stdio buffer for every byte, making installation take
minutes. Direct reads retain the fixed memory bound, both validation passes,
payload readback, and all signature/hash checks. The update that first installs
this improvement still runs the previously installed runtime's installer;
subsequent updates use the improved path.

`provenance.json` is an object. Record at least `fork_commit`, `idf_tag`,
`idf_commit`, `build_container`, `compiler_version`, `managed_packages`,
`bootloader_sha256` and `partition_table_sha256`. Also record the managed
repository commit, source dirty status, exact preset/SDK settings, native API
contract, debugger state for hardware testing, and test log locations.
Packaging works with incomplete provenance for development but marks
`provenance_complete=false`; require a complete, reviewed record for release.
The tools never invent missing identities.

An explicitly identified development image may use a Kconfig overlay with
`# CONFIG_NF_PRODUCT_OTA_REQUIRE_SIGNATURE is not set`. Pass the overlay using
`NF_USER_KCONFIG_OVERLAY` and use `--allow-unsigned-development` when generating,
packaging or verifying unsigned artifacts. Production verification defaults to
requiring a signature, and a signed package always needs a trusted verification
key even when the development option is present. The tool never silently
truncates identifiers or falls back to unsigned output after a signing error.

## Visual Studio deployment during development

The product build normally checks the managed application's exact hash, length
and erased partition tail before every CLR startup, including soft restarts.
Ordinary Visual Studio deployment changes those bytes and therefore fails the
release pairing check. For development, explicitly opt in when configuring:

```sh
cmake --preset PRODUCT_S3_OTA -B build/product_s3_vs_debug \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DNF_USER_KCONFIG_OVERLAY="" \
  -DNF_PRODUCT_OTA_ALLOW_VS_DEPLOY=ON \
  -DNF_PRODUCT_MANAGED_IMAGE=/artifacts/application.bin \
  -DNF_PRODUCT_RELEASE_ID=vs-dev \
  -DNF_PRODUCT_PUBLIC_KEY=/artifacts/release-public.pem
cmake --build build/product_s3_vs_debug --parallel 8
```

`NF_PRODUCT_OTA_ALLOW_VS_DEPLOY` is a CMake option, defaults to `OFF`, and requires
the managed debugger with `NF_BUILD_RTM` disabled. It is independent of CMake's
`Debug` build type: `MinSizeRel` keeps the runtime within its flash slot while
retaining the managed debugger. No Kconfig signature override is needed.

Provision the development runtime and initial managed image using the product
layout below. The initial `application.bin` remains a build input; after this
firmware is installed, Visual Studio may replace the active managed deployment
over USB without rebuilding the native runtime for each C# edit. The deployed
application must still use compatible native APIs and `Product.Firmware` interop
metadata. Changing native APIs still requires a native rebuild.

The option skips only the running managed image's hash and erased-tail checks.
The partition layout and descriptor structure, incoming OTA package signatures,
payload hashes, inactive-slot readback and rollback rules remain enforced.
The build emits a development warning and boot status reports `vs_deploy=1`.
After Visual Studio deployment, the descriptor's release ID describes the
native build, not necessarily the current managed application's contents.

Use an already confirmed slot for interactive debugging. An unconfirmed OTA
trial still has a 60-second deadline and its watchdog; pausing it or deploying
with a soft restart can cause rollback. This flag does not enable Secure Boot.

For an OTA test, stop debugging, close Visual Studio, and reset the board once
so the existing application runs independently. Keep Visual Studio closed until
the new application has confirmed its boot and registered its new build. An
attached debugger can request StopOnBoot after the OTA restart: the CLR prints
`Ready.` but waits before running `Main`, so the application cannot confirm and
the native deadline rolls it back. A passive serial terminal can capture output
during this test without sending debugger commands. Reattach Visual Studio
after confirmation for normal deployment and debugging.

For a strict release build, use a separate build directory and omit the flag
or pass `-DNF_PRODUCT_OTA_ALLOW_VS_DEPLOY=OFF`, then rebuild and install a matching
native/managed pair. Reconfiguring the host alone does not change firmware
already on a board. Development builds must not be distributed as production
releases.

## Format and compatibility

The 128-byte header uses explicit unsigned little-endian fields: magic at 0,
format/header lengths at 8/10, flags at 12, total/runtime/managed lengths at
16/20/24, layout ID at 28, zero-padded target/release identifiers at 32/48,
and native/managed SHA256 hashes at 64/96. Identifiers contain 1–16 printable
non-space ASCII bytes. Bit 0 of flags indicates a signature; other bits are
rejected. Empty payloads, overflowed length sums, wrong target/layout,
noncanonical identifier padding and trailing bytes are rejected.

Native bytes start at offset 128, followed by the raw managed bytes and the
optional 64-byte signature. The production signature is ECDSA P-256 using
SHA256 over ASCII `PRDOTA-SIGN-v1` without a terminator followed by the exact
header. Its trailer is big-endian unsigned `r[32] || s[32]`, not ASN.1 DER.
Signature verification authenticates both payload hashes and compatibility
fields. Payload hashes are then independently checked.

The existing SeaDragon delivery protocol carries the complete `update.bin`
as sequential pages. Its start packet supplies the complete-file CRC32 and
page count. Use model `Seadragon-S3`, retain its existing version/build fields,
and do not send the Linux install ZIP as an ESP32 update. These legacy OTA
frames are not protected by the normal command HMAC; production package
signatures provide firmware authenticity.

The managed receiver retains installation outcomes through reboot. It reports
the confirmed version/build and installation timestamp in Register only after
local boot confirmation and matching the expected native slot and descriptor.
A repeated request for a confirmed package receives Completed/Ok without
reinstalling. Failed or rolled-back attempts have a persistent retry budget;
the server records outcomes and reconciles registration before retrying.
Deploy the matching SeadragonS3 and MeterMacsCore changes together. The existing
wire messages are unchanged, including compatibility with Linux clients.

For the cellular binary-receive recovery fix, rebuild the native runtime as well
as the managed application. The ESP32 UART receive event task and managed reads
now serialize access to their shared ring buffer, and close waits for the receive
worker to stop before freeing its memory. This fixes a software source of lost or
duplicated UART bytes that can break a counted modem response. A Visual Studio
managed-only deployment cannot replace that native UART implementation. Keep
`NF_PRODUCT_OTA_ALLOW_VS_DEPLOY=ON` in the rebuilt development runtime to retain
USB deployment; Secure Boot configuration is unchanged.

## Provisioning and layout migration

This layout moves the shared filesystem. A device with a stock or earlier
OTA table needs a controlled conversion: export and verify device identity,
keys and configuration; verify the physical chip, flash and PSRAM; provision
the new fixed table and matching A pair; explicitly initialize or restore
shared storage; and verify configuration and local hardware before service.
Never deploy the partition table, bootloader or filesystem image through the
routine OTA installer.

With partition-table offset `0x8000`, the initial addresses are:

| Artifact / region | Offset | Size / rule |
| --- | ---: | --- |
| ESP32-S3 rollback-enabled bootloader | `0x000000` | Use actual build output; S3 does not use ESP32's `0x1000` bootloader address |
| Product partition table | `0x008000` | Decode and compare against the CSV |
| NVS | `0x009000` | `0x6000`, preserve/reprovision deliberately |
| PHY data | `0x00F000` | `0x1000` |
| Initial `nanoCLR.bin` / `ota_0` | `0x010000` | `0x1A0000` maximum |
| `ota_1` | `0x1B0000` | `0x1A0000`, initially empty |
| Initial `application.bin` / `deploy_a` | `0x350000` | `0x2E0000` maximum |
| `deploy_b` | `0x630000` | `0x2E0000`, initially empty |
| OTA selection data | `0x910000` | `0x2000`, **initially erased** |
| Shared LittleFS `config` | `0x912000` | `0x6EE000`, explicitly initialized or restored |

Inspect generated flashing arguments and decode the **actual binary** partition
table with the pinned SDK's `gen_esp32part.py`. Verify the product CSV with:

```sh
python tools/product_ota/product_ota.py check-layout \
  --csv targets/ESP32/_IDF/esp32s3/partitions_product_ota_16mb.csv
```

A stock binary with `16mb` in its filename is not the product table. Keep the
production bootloader/table unchanged during OTA acceptance tests. In the pinned
ESP-IDF v5.5.5 source, commit
`b774170ff46c393eeb5e495ea37936038d3f4f4f`, the no-factory bootloader selects A
from erased metadata and writes its sequence-1 VALID record before loading the
application (`bootloader_utility.c`, `set_actual_ota_seq`). Manufacturing must
verify this behavior and exercise the managed application. B's metadata record
may remain absent for the first A→B update; the installer skips retiring a
nonexistent B record. A runtime `ESP_ERR_NOT_FOUND` for the running state is an
error, not permission to fabricate or silently confirm metadata. An empty B
does not prevent the first A→B trial with healthy A as its fallback.

Shared storage never autoformats after a mount error. Fresh blank storage must
be initialized through an explicit provisioning/recovery process. For that
process, supply an existing, reviewed directory containing the intended initial
files or verified configuration export:

```sh
cmake --preset PRODUCT_S3_OTA \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DNF_PRODUCT_MANAGED_IMAGE=/artifacts/application.bin \
  -DNF_PRODUCT_RELEASE_ID=2026.10.20-01 \
  -DNF_PRODUCT_PUBLIC_KEY=/artifacts/release-public.pem \
  -DNF_PRODUCT_CONFIG_DIRECTORY=/provisioning/reviewed-config
cmake --build --preset PRODUCT_S3_OTA --target littlefs_config_bin
```

This optional input invokes the installed component's
`littlefs_create_partition_image(config ...)`, producing
`build/product_s3_ota/config.bin`. The component takes partition size and offset
from the selected table and uses block size 4096. The product wrapper excludes
the terminating NUL from the configured filename buffer length and caps the
image's filename limit at the runtime's `LFS_NAME_MAX` (255 for this SDK).
Passing the buffer length of 256 directly creates an image the firmware cannot
mount, preventing USB debugger startup. The component installs its own
image-building dependencies. An explicitly chosen
empty directory creates a blank filesystem; never treat it as a configuration
backup. Inspect the seed and image before writing.

The image is deliberately **not** added to the ordinary firmware flash target.
During authorized initial provisioning or recovery only, flash it at
`0x912000` using verified build flashing arguments, or invoke the component's
dedicated `config-flash` target after checking the connected device/port:

```sh
cmake --build --preset PRODUCT_S3_OTA --target config-flash
```

That target replaces shared persistent data. It is not an OTA deployment step.
For routine release builds omit `NF_PRODUCT_CONFIG_DIRECTORY`; for an existing
build cache clear it with `-DNF_PRODUCT_CONFIG_DIRECTORY=`.

Preserve
old-readable configuration during rollback support; executable rollback does
not undo shared file writes. The maximum signed package is `0x4800C0` bytes;
keep only one complete staging package and maintain operating free-space
reserve. The managed application owns download retries and quarantine/cleanup
of rejected releases.

## Automated checks

```sh
python -m pip install -r tools/product_ota/test-requirements.txt kconfiglib
python -m unittest discover -s tools/product_ota -p 'test_*.py' -v
python scripts/validate_defconfigs.py --target PRODUCT_S3_OTA --verbose
python tools/product_ota/product_ota.py check-layout \
  --csv targets/ESP32/_IDF/esp32s3/partitions_product_ota_16mb.csv
c++ -std=c++11 -Wall -Wextra -Werror -Itargets/ESP32/_include \
  tools/product_ota/test_native_parser.cpp -o /tmp/product_ota_parser_tests
/tmp/product_ota_parser_tests
python tools/product_ota/native_tests/run.py
```

The host tests cover exact format/identifiers, CRC vector and streaming,
production signature policy, wrong keys, header/payload/signature corruption,
empty/short/trailing files, size limits, 32-bit addition overflow, descriptor
generation, partition map, manifest provenance, and CLI failure behavior.
They use temporary synthetic payloads, not releasable firmware.
The C++ host test compiles the same header parser and CRC implementation used
by the device. CI runs these checks; its job does not flash a board or replace
the required full firmware build and hardware acceptance record.

Six CMake configuration tests exercise the real product module from a child
directory, checking the actual engine source's root-scope compiler definition,
default and explicit VS deployment flags, switching an existing build back to
strict mode, and rejection of incompatible debugger/RTM settings.

Six filesystem image tests check the filename limit and CMake scope, reproduce
the incompatible 256-character image, and verify that the corrected image
mounts with the runtime's 255-character limit and preserves provisioning files.

The [native fault harness](native_tests/README.md) also compiles the actual
installer against deterministic SDK boundaries, covering 96 scenarios across
the four combinations of required/optional package signatures and strict/VS
managed deployment. It requires OpenSSL development
headers and a C++ compiler. Its ECDSA boundary deliberately fails closed;
successful device-side signature verification remains a hardware test.

## Required hardware acceptance record

All items below are **NOT RUN**. Run in both directions using distinctive
release IDs, the production bootloader, the exact N16R8 module and no attached
debugger. Save slot/deployment identities, payload hashes, reset reasons and
configuration/device-ID checks. Inject faults at several positions in each
phase. A rollback passes only when the previous **managed application** runs.

- First update with initially empty B: fail A→B trial and return to A, then
  confirm A→B, then fail B→A and return to B.
- Power loss during download, inactive metadata invalidation, runtime
  erase/write, managed erase/write, activation write and unconfirmed trial.
- Successful trial remains selected through repeated hardware power cycles.
- Broken managed entry point, wrong native contract, yielding managed
  deadlock and hard CPU/scheduler starvation recover through supervisor or
  reset-producing watchdog.
- Smaller managed image cannot load old trailing assemblies; altered managed
  bytes fail pairing validation before CLR startup.
- Shared filesystem mount failure preserves files; full filesystem preserves
  operating reserve; incompatible configuration migration permits fallback.
- Concurrent installs are rejected; late confirmation during installation
  cannot confirm a different pair; queued deadline after successful
  confirmation does not reject a VALID image.
- Cancellation/activation race is resolved under the lifecycle lock; every
  final selection attempt causes a full restart even if the SDK returns an
  error. CLR soft reboot quiesces the worker and cannot extend a trial.
- Unsigned, wrong-target, corrupt and wrong-key packages never activate.
- A rejected release is quarantined and does not install repeatedly after
  rollback; retries require the explicit managed policy.

Record actual native/managed/package sizes, filesystem reserve, selected
60-second trial deadline, SDK/container/compiler identities, production
bootloader/table hashes, local startup health checks and any unsupported
recovery scenarios with those results.
