# Painted Wolf Python Engine

Painted Wolf Code uses this engine to run agent-written Python scripts on macOS (Apple Silicon) and Linux (x86-64). At build time, it adds work metering to a pinned CPython WebAssembly module and compiles it ahead of time into a native shared library. At runtime, that library evaluates ordinary Python scripts without requiring a system Python installation or a JIT.

## How it fits into Painted Wolf Code

The engine creates a fresh guest instance with its own memory and fuel budget for each invocation. When CPython needs a host operation, such as opening a file, it calls the host through a WASI import. Painted Wolf Code supplies the Python package image and decides which workspace view the script sees, whether an operation is allowed, which changes to publish, and what to record in the execution receipt. The engine exposes cancellation, work usage and terminal status through its [C interface](include/pw_py_engine.h). The [import inventory](imports.json) lists the calls the host implements.

Both platforms build from the same metered guest. The [source lock](lock.json) records the CPython and WABT versions, expected digests, metering algorithm and compiler flags. The [target adapters](scripts/targets) handle platform-specific linking, signing and symbol checks.

## Build and test

Run the following from the repository root on the target platform. Install Go 1.22 or newer, Python 3, CMake, a C/C++ toolchain, curl and tar with xz support. On macOS, the build uses the Xcode command-line tools and ad-hoc signs the library with hardened runtime enabled.

Download and verify the pinned CPython archive, then extract the interpreter and matching standard library into a disposable build directory.

```sh
set -eu
build_dir="$(mktemp -d)"
archive="$build_dir/cpython.tar.gz"
url="$(python3 -c 'import json; print(json.load(open("lock.json"))["cpython"]["source_url"])')"
curl -fL "$url" -o "$archive"
python3 - "$archive" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

expected = json.loads(Path("lock.json").read_text())["cpython"]["source_sha256"]
if hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest() != expected:
    raise SystemExit("CPython archive checksum mismatch")
PY
tar -xzf "$archive" -C "$build_dir"

bash scripts/build_target.sh \
  "$build_dir/bin/python-3.12.0.wasm" \
  "$build_dir/usr/local/lib" \
  "$build_dir/cache" \
  "$build_dir/work" \
  "$build_dir/out" \
  "$(git rev-parse HEAD)"
```

The builder verifies the guest digest, tests the metering transform, builds the pinned WABT translator, compiles the library and checks its exported symbols. It then loads the library and runs the [producer tests](tests/test_engine.c), including fuel exhaustion, cancellation during a CPU loop, memory limits and isolation between concurrent instances.

The output directory contains the target archive, artifact manifest, checksums and test log. The archive includes the shared library, C header, import inventory and license notices. Its manifest identifies the source commit, ABI, target, toolchain and guest digests so the consuming application can verify the pairing.
