#!/usr/bin/env python3
"""Compile and run the actual native telemetry helper against host SDK fakes."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    repository = tests.parents[2]
    with tempfile.TemporaryDirectory(prefix="product-telemetry-tests-") as directory:
        for name in ("test_temperature", "test_power_status"):
            binary = Path(directory) / name
            command = [args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-I", str(tests / "fakes"), "-I", str(repository / "targets/ESP32/_include"),
                       str(tests / (name + ".cpp")), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
