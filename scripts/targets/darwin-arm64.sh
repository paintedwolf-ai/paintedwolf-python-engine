# Target adapter for darwin-arm64 (macOS Apple Silicon).
# Sourced by scripts/build_target.sh; owns platform-specific toolchain, link,
# hardened-runtime codesign, and Mach-O exported-symbol audit steps.

PLATFORM="darwin-arm64"
LIB_NAME="libpw_cpython_wasi.dylib"
CODESIGN_HARDENED="true"

target_sha256() {
  shasum -a 256 "$@"
}

target_jobs() {
  sysctl -n hw.ncpu
}

target_link_and_sign() {
  local repo_root="$1"
  local obj_dir="$2"
  local lib_path="$3"
  cc -dynamiclib -o "$lib_path" "$obj_dir"/*.o \
    -Wl,-install_name,@rpath/"$LIB_NAME" \
    -Wl,-exported_symbols_list,"$repo_root/exports/darwin.exp" \
    -lm -lpthread
  codesign --force --sign - --options runtime --timestamp=none "$lib_path"
  codesign --verify --verbose=2 "$lib_path"
  echo "--- codesign entitlements check ---"
  codesign -d --entitlements - "$lib_path" 2>&1 || true
}

target_audit_symbols() {
  local lib_path="$1"
  local exported non_pw
  exported="$(nm -gU "$lib_path" | awk '{print $3}' | sort)"
  echo "$exported"
  non_pw="$(echo "$exported" | grep -v '^_pw_py_' || true)"
  if [[ -n "$non_pw" ]]; then
    echo "ERROR: Unexpected exported symbols found: $non_pw" >&2
    return 1
  fi
}
