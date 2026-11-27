#!/usr/bin/env bash
# Reproducible PY07 target builder for darwin-arm64 and linux-amd64.
# Usage: scripts/build_target.sh <python.wasm> <stdlib_dir> <cache_dir> <work_dir> <out_dir> <source_commit>
set -euo pipefail

PYTHON_WASM="$1"
STDLIB_DIR="$2"
CACHE_DIR="$3"
WORK_DIR="$4"
OUT_DIR="$5"
SOURCE_COMMIT="$6"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE_TAG="pw-cpython-wasi-engine-v0.1.0"
WABT_VERSION="1.0.42"
WABT_URL="https://github.com/WebAssembly/wabt/releases/download/1.0.42/wabt-1.0.42.tar.xz"
WABT_SHA256="a76cda3c174a43097863a07fc0b0c202f770f53e21806ea2636f167d1ffb1e30"
CPYTHON_WASM_SHA256="5ce0cbeb843e6e5abf2d50c7158002e8333c26a40fbe27a7a52e66ee48cf64a8"
METERED_WASM_SHA256="774a08a7863c1dee1b2f21bb80fb9964f0dd5dc27bdf2294bdd8a11934e893c6"

OS="$(uname -s)"
ARCH="$(uname -m)"
if [[ "$OS" == "Darwin" && "$ARCH" == "arm64" ]]; then
  PLATFORM="darwin-arm64"
  LIB_NAME="libpw_cpython_wasi.dylib"
  SHA256_CMD="shasum -a 256"
  JOBS="$(sysctl -n hw.ncpu)"
elif [[ "$OS" == "Linux" && "$ARCH" == "x86_64" ]]; then
  PLATFORM="linux-amd64"
  LIB_NAME="libpw_cpython_wasi.so"
  SHA256_CMD="sha256sum"
  JOBS="$(nproc)"
else
  echo "Unsupported platform: $OS/$ARCH" >&2
  exit 1
fi

mkdir -p "$CACHE_DIR" "$WORK_DIR" "$OUT_DIR"

# 1. Verify pinned python.wasm
GOT_WASM_SHA="$($SHA256_CMD "$PYTHON_WASM" | awk '{print $1}')"
if [[ "$GOT_WASM_SHA" != "$CPYTHON_WASM_SHA256" ]]; then
  echo "ERROR: python.wasm sha256 mismatch: got $GOT_WASM_SHA want $CPYTHON_WASM_SHA256" >&2
  exit 1
fi
echo "[1/7] Verified python.wasm ($GOT_WASM_SHA)"

# 2. Run authoritative InstrumentWasmFuel + byte-equivalence test
(cd "$REPO_ROOT/cmd/meter" && PW_PY07_CPYTHON_WASM="$PYTHON_WASM" go test -v ./...)
(cd "$REPO_ROOT/cmd/meter" && go run . "$PYTHON_WASM" "$WORK_DIR/metered.wasm" "$METERED_WASM_SHA256")
echo "[2/7] Metered python.wasm verified ($METERED_WASM_SHA256)"

# 3. Fetch, verify, and build pinned upstream WABT 1.0.42 (wasm2c only)
WABT_TAR="$CACHE_DIR/wabt-$WABT_VERSION.tar.xz"
if [[ ! -f "$WABT_TAR" ]]; then
  curl -fsSL -o "$WABT_TAR.tmp" "$WABT_URL"
  mv "$WABT_TAR.tmp" "$WABT_TAR"
fi
GOT_WABT_SHA="$($SHA256_CMD "$WABT_TAR" | awk '{print $1}')"
if [[ "$GOT_WABT_SHA" != "$WABT_SHA256" ]]; then
  echo "ERROR: wabt-$WABT_VERSION.tar.xz sha256 mismatch: got $GOT_WABT_SHA want $WABT_SHA256" >&2
  exit 1
fi

WABT_SRC="$CACHE_DIR/wabt-$WABT_VERSION"
WABT_BIN="$WABT_SRC/build/wasm2c"
if [[ ! -x "$WABT_BIN" ]]; then
  rm -rf "$WABT_SRC"
  tar -xf "$WABT_TAR" -C "$CACHE_DIR"
  cmake -S "$WABT_SRC" -B "$WABT_SRC/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    -DBUILD_LIBWASM=OFF \
    -DUSE_INTERNAL_SHA256=ON
  cmake --build "$WABT_SRC/build" --target wasm2c -j"$JOBS"
fi
"$WABT_BIN" --version
echo "[3/7] Verified WABT $WABT_VERSION ($WABT_BIN)"

# 4. Translate metered.wasm -> C with wasm2c --num-outputs=12 -n pw
GEN_DIR="$WORK_DIR/gen"
rm -rf "$GEN_DIR"
mkdir -p "$GEN_DIR"
"$WABT_BIN" --num-outputs=12 -n pw "$WORK_DIR/metered.wasm" -o "$GEN_DIR/guest.c"
cp "$WABT_SRC"/wasm2c/*.h \
   "$WABT_SRC"/wasm2c/*.inc \
   "$WABT_SRC/wasm2c/wasm-rt-impl.c" \
   "$WABT_SRC/wasm2c/wasm-rt-mem-impl.c" \
   "$GEN_DIR/"
cp "$REPO_ROOT/include/pw_py_engine.h" "$GEN_DIR/"

python3 "$REPO_ROOT/scripts/generate_bridge.py" \
  --guest-h "$GEN_DIR/guest.h" \
  --imports-json "$REPO_ROOT/imports.json" \
  --out-c "$GEN_DIR/pw_py_bridge.c"
echo "[4/7] Generated wasm2c C sources and verified 43-import bridge"

# 5. Compile objects with release optimization + explicit bounds/stack checks and link shared library
OBJ_DIR="$WORK_DIR/obj"
rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"

COMMON_CFLAGS=(
  -O1 -g0 -fPIC -fvisibility=hidden
  -DWASM_RT_MEMCHECK_BOUNDS_CHECK=1
  -DWASM_RT_STACK_DEPTH_COUNT=1
  -DWASM_RT_USE_SEGUE=0
  -I"$GEN_DIR"
)

printf '%s\n' "$GEN_DIR"/guest_*.c "$GEN_DIR"/wasm-rt-impl.c "$GEN_DIR"/wasm-rt-mem-impl.c | \
  xargs -P"$JOBS" -I{} sh -c '
    src="$1"; obj_dir="$2"; shift 2
    base="$(basename "$src" .c)"
    cc "$@" -c "$src" -o "$obj_dir/$base.o"
  ' _ {} "$OBJ_DIR" "${COMMON_CFLAGS[@]}"

cc "${COMMON_CFLAGS[@]}" \
  -DPW_PY_ENGINE_TAG="\"$ENGINE_TAG\"" \
  -DPW_PY_SOURCE_COMMIT="\"$SOURCE_COMMIT\"" \
  -DPW_PY_PLATFORM="\"$PLATFORM\"" \
  -DPW_PY_CPYTHON_WASM_SHA256="\"$CPYTHON_WASM_SHA256\"" \
  -DPW_PY_METERED_WASM_SHA256="\"$METERED_WASM_SHA256\"" \
  -DPW_PY_WABT_VERSION="\"$WABT_VERSION\"" \
  -c "$GEN_DIR/pw_py_bridge.c" -o "$OBJ_DIR/pw_py_bridge.o"

LIB_PATH="$WORK_DIR/$LIB_NAME"
if [[ "$PLATFORM" == "darwin-arm64" ]]; then
  cc -dynamiclib -o "$LIB_PATH" "$OBJ_DIR"/*.o \
    -Wl,-install_name,@rpath/"$LIB_NAME" \
    -Wl,-exported_symbols_list,"$REPO_ROOT/exports/darwin.exp" \
    -lm -lpthread
  codesign --force --sign - --options runtime --timestamp=none "$LIB_PATH"
  codesign --verify --verbose=2 "$LIB_PATH"
  echo "--- codesign entitlements check ---"
  codesign -d --entitlements - "$LIB_PATH" 2>&1 || true
else
  cc -shared -o "$LIB_PATH" "$OBJ_DIR"/*.o \
    -Wl,-soname,"$LIB_NAME" \
    -Wl,--version-script="$REPO_ROOT/exports/linux.map" \
    -lm -lpthread
fi
echo "[5/7] Built $LIB_PATH ($(wc -c < "$LIB_PATH") bytes)"

# 6. Audit exported symbols (must be ONLY the 7 pw_py_* entry points)
echo "[6/7] Auditing exported symbols:"
if [[ "$PLATFORM" == "darwin-arm64" ]]; then
  EXPORTED="$(nm -gU "$LIB_PATH" | awk '{print $3}' | sort)"
  echo "$EXPORTED"
  NON_PW="$(echo "$EXPORTED" | grep -v '^_pw_py_' || true)"
else
  EXPORTED="$(nm -D --defined-only "$LIB_PATH" | awk '$2 ~ /^[TDRB]$/ {print $3}' | sort)"
  echo "$EXPORTED"
  NON_PW="$(echo "$EXPORTED" | grep -v '^pw_py_' || true)"
fi
if [[ -n "$NON_PW" ]]; then
  echo "ERROR: Unexpected exported symbols found: $NON_PW" >&2
  exit 1
fi

# 7. Run independent C producer test suite via dlopen()
cc -O2 -Wall -Wextra -I"$REPO_ROOT/include" "$REPO_ROOT/tests/test_engine.c" -o "$WORK_DIR/test_engine" -ldl -lpthread
"$WORK_DIR/test_engine" "$LIB_PATH" "$STDLIB_DIR" | tee "$OUT_DIR/producer-test-$PLATFORM.log"

# Package release archive
STAGE="$WORK_DIR/stage-$PLATFORM"
rm -rf "$STAGE"
mkdir -p "$STAGE/include" "$STAGE/LICENSES"
cp "$LIB_PATH" "$STAGE/$LIB_NAME"
cp "$REPO_ROOT/include/pw_py_engine.h" "$STAGE/include/pw_py_engine.h"
cp "$REPO_ROOT/imports.json" "$STAGE/imports.json"
cp "$REPO_ROOT/LICENSES/"* "$STAGE/LICENSES/"
cp "$WABT_SRC/LICENSE" "$STAGE/LICENSES/WABT-UPSTREAM-LICENSE.txt"

LIB_SHA="$($SHA256_CMD "$STAGE/$LIB_NAME" | awk '{print $1}')"
HDR_SHA="$($SHA256_CMD "$STAGE/include/pw_py_engine.h" | awk '{print $1}')"
INV_SHA="$($SHA256_CMD "$STAGE/imports.json" | awk '{print $1}')"
CC_VER="$(cc --version | head -n1 | tr '"' "'")"

cat > "$STAGE/artifact-manifest.json" <<EOF
{
  "schema_version": "paintedwolf.native-python-engine-artifact/v1",
  "engine_tag": "$ENGINE_TAG",
  "source_commit": "$SOURCE_COMMIT",
  "platform": "$PLATFORM",
  "abi_version": 1,
  "abi_header_sha256": "$HDR_SHA",
  "library_name": "$LIB_NAME",
  "library_sha256": "$LIB_SHA",
  "cpython_version": "3.12.0",
  "cpython_wasm_sha256": "$CPYTHON_WASM_SHA256",
  "metered_wasm_sha256": "$METERED_WASM_SHA256",
  "fuel_algorithm_version": "pw-loop-func-i64/v1",
  "wabt_version": "$WABT_VERSION",
  "wabt_source_sha256": "$WABT_SHA256",
  "import_count": 43,
  "import_inventory_sha256": "$INV_SHA",
  "toolchain": "$CC_VER",
  "compile_flags": ["-O1", "-g0", "-fPIC", "-fvisibility=hidden", "-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1", "-DWASM_RT_STACK_DEPTH_COUNT=1", "-DWASM_RT_USE_SEGUE=0"],
  "codesign_hardened": $( [[ "$PLATFORM" == "darwin-arm64" ]] && echo "true" || echo "false" ),
  "jit_entitlements": []
}
EOF

(cd "$STAGE" && $SHA256_CMD "$LIB_NAME" include/pw_py_engine.h imports.json artifact-manifest.json LICENSES/* > SHA256SUMS)

ARCHIVE_NAME="$ENGINE_TAG-$PLATFORM.tar.gz"
tar -czf "$OUT_DIR/$ARCHIVE_NAME" -C "$STAGE" .
cp "$STAGE/artifact-manifest.json" "$OUT_DIR/artifact-manifest-$PLATFORM.json"
(cd "$OUT_DIR" && $SHA256_CMD "$ARCHIVE_NAME" "artifact-manifest-$PLATFORM.json" > "SHA256SUMS-$PLATFORM")
echo "[7/7] Packaged $OUT_DIR/$ARCHIVE_NAME"
cat "$OUT_DIR/SHA256SUMS-$PLATFORM"
