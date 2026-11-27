# Target adapter for linux-amd64 (Linux x86_64).
# Sourced by scripts/build_target.sh; owns platform-specific toolchain, link,
# and ELF exported-symbol audit steps.

PLATFORM="linux-amd64"
LIB_NAME="libpw_cpython_wasi.so"
CODESIGN_HARDENED="false"

target_sha256() {
  sha256sum "$@"
}

target_jobs() {
  nproc
}

target_link_and_sign() {
  local repo_root="$1"
  local obj_dir="$2"
  local lib_path="$3"
  cc -shared -o "$lib_path" "$obj_dir"/*.o \
    -Wl,-soname,"$LIB_NAME" \
    -Wl,--version-script="$repo_root/exports/linux.map" \
    -lm -lpthread
}

target_audit_symbols() {
  local lib_path="$1"
  local exported non_pw
  exported="$(nm -D --defined-only "$lib_path" | awk '$2 ~ /^[TDRB]$/ {print $3}' | sort)"
  echo "$exported"
  non_pw="$(echo "$exported" | grep -v '^pw_py_' || true)"
  if [[ -n "$non_pw" ]]; then
    echo "ERROR: Unexpected exported symbols found: $non_pw" >&2
    return 1
  fi
}
