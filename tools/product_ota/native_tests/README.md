# Native OTA fault injection

Run on Linux with a C++11 compiler and OpenSSL development headers/library:

```sh
python tools/product_ota/native_tests/run.py
```

The harness includes the actual
`targets/ESP32/_common/ProductFirmwareUpdate.cpp` translation unit. It invokes
the real public start/poll/release APIs, worker, installation transaction,
confirmation, supervisor and soft-reboot paths. It does not copy their
decisions into a test implementation.

SDK boundaries use deterministic fakes:

- Fixed layout 2 partitions backed by a 16 MiB memory image. Writes and erases
  against the active managed partition fail an assertion.
- Real SHA256 through an OpenSSL adapter, actual device CRC and actual package
  parser. CRC/SHA failures are tested before inactive writes.
- Injectable metadata, begin/write/end, managed erase/write, readback,
  activation, confirmation and rollback errors.
- A single-step scheduler and nonrecursive lifecycle lock model. Reset raises
  a test exception; the test records whether reset happened while the commit
  lock was held.
- Owned OTA handles and internal-RAM buffers are tracked. Begin can return an
  error after assigning a handle; End consumes its handle even on error.
- Package file descriptors are tracked through the real VFS-style
  `open`/`read`/`lseek`/`fstat`/`close` path. A 1.7 MiB package transaction must use
  block reads bounded at 4096 bytes, including both complete validation passes
  and payload programming. This prevents the size-optimized newlib `_IONBF`
  `fread` regression that refilled a one-byte stdio buffer for every byte.
  Interrupted operations and short reads preserve exact payload bytes;
  open/stat/seek/read errors, premature EOF, EOF-check errors and close failures
  prevent activation. A close error, including `EINTR`, is never retried against
  a descriptor another task could have reused.
- Actual `ProductFirmwareErrorMap.h` and actual CLR HRESULT constants test all
  product error codes, representative positive SDK errors and unknown errors.

The harness compiles four executables: required/optional package signatures
crossed with `NF_PRODUCT_OTA_ALLOW_VS_DEPLOY=0`/`1`. Each runs the actual engine
with its compiled policy, rather than changing a fake policy at runtime.

All four configurations test initial startup and CLR soft-reboot validation
with a changed managed byte, a shorter managed image, an appended assembly,
and an unerased byte at the end of the active deployment partition. Strict
builds reject these changes; the explicit VS development mode accepts them.
Both modes still reject unsupported partition geometry and corrupted native
or managed OTA payloads. Tests also verify the development warning,
`vs_deploy` boot diagnostic, unchanged 60-second confirmation deadline, and
mandatory hardware restart on an unconfirmed trial's CLR soft reboot.

The optional-signature executables cover cleanup
after each installation phase, corrupted/truncated files, wrong target/CRC,
native and managed readback failure, non-erased inactive managed tails,
cancellation at the final close-to-activation boundary,
concurrent start rejection, abandoned operation cleanup, activation API error
followed by mandatory reset, successful activation, confirmation identity and
deadline guards, stale supervisor work after confirmation, failed rollback
followed by reset, trial soft reboot, the first A-to-B update without a prior B
metadata record, and failure-closed handling of missing running-slot metadata.
It also exercises the real free-space and boot-status helpers: null inputs,
full filesystem, inconsistent used/total values, SDK information errors,
trial/reset diagnostics, unavailable metadata and bounded output truncation.
An OTA install from a modified active managed image is accepted only with
the VS development flag, and neither mode changes that active image.

The required-signature executables also verify that a structurally correct unsigned file and a
signed envelope with invalid crypto are rejected before any inactive write.
ECDSA SDK functions deliberately fail closed in this harness. These tests
exercise signature policy and error flow; they do **not** establish mbedTLS
cryptographic correctness. Host package tests exercise real P-256 signing and
verification separately. A real signed-package device test remains required.

This does not simulate ESP-IDF image validation, real flash timing, power
failure, dual-core scheduling, hardware watchdogs, filesystem durability or
the ESP bootloader. Those require the physical-device acceptance matrix.
The standalone native parser tests and complete firmware compile remain
separate checks.
