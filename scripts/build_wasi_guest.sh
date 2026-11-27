#!/usr/bin/env bash
# Reproducible PY10 source builder for CPython 3.12.0/WASI with statically linked
# zlib, bzip2, sqlite3, libjpeg, libxml2 (--without-http --without-ftp), libxslt,
# and built-in Pillow 11.1.0 (PIL._imaging) + lxml 5.3.0 (lxml.etree, lxml._elementpath).
#
# Usage: scripts/build_wasi_guest.sh <cache_dir> <work_dir> <out_dir>
set -euo pipefail

CACHE_DIR="$1"
WORK_DIR="$2"
OUT_DIR="$3"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

DL="$CACHE_DIR/downloads"
SYSROOT="$WORK_DIR/wasi-deps"
mkdir -p "$DL" "$WORK_DIR/src" "$SYSROOT/include" "$SYSROOT/lib" "$OUT_DIR/bin" "$OUT_DIR/lib" "$OUT_DIR/LICENSES"

fetch_verified() {
  local name="$1" url="$2" want_sha="$3"
  local dst="$DL/$name"
  if [[ ! -f "$dst" ]]; then
    curl -fsSL --retry 3 -o "$dst.tmp" "$url"
    mv "$dst.tmp" "$dst"
  fi
  local got_sha
  got_sha="$(sha256sum "$dst" | awk '{print $1}')"
  if [[ "$got_sha" != "$want_sha" ]]; then
    echo "ERROR: $name sha256 mismatch: got $got_sha want $want_sha" >&2
    exit 1
  fi
}

fetch_verified wasi-sdk-20.0-linux.tar.gz     "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-20/wasi-sdk-20.0-linux.tar.gz"                 "7030139d495a19fbeccb9449150c2b1531e15d8fb74419872a719a7580aad0f9"
fetch_verified Python-3.12.0.tar.xz           "https://www.python.org/ftp/python/3.12.0/Python-3.12.0.tar.xz"                                                  "795c34f44df45a0e9b9710c8c71c15c671871524cd412ca14def212e8ccb155d"
fetch_verified zlib-1.3.1.tar.xz              "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz"                                      "38ef96b8dfe510d42707d9c781877914792541133e1870841463bfa73f883e32"
fetch_verified bzip2-1.0.8.tar.gz             "https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz"                                                            "ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"
fetch_verified sqlite-autoconf-3450300.tar.gz "https://www.sqlite.org/2024/sqlite-autoconf-3450300.tar.gz"                                                     "b2809ca53124c19c60f42bf627736eae011afdcc205bb48270a5ee9a38191531"
fetch_verified jpegsrc.v9f.tar.gz             "https://www.ijg.org/files/jpegsrc.v9f.tar.gz"                                                                   "04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b"
fetch_verified libxml2-2.12.9.tar.xz          "https://download.gnome.org/sources/libxml2/2.12/libxml2-2.12.9.tar.xz"                                          "59912db536ab56a3996489ea0299768c7bcffe57169f0235e7f962a91f483590"
fetch_verified libxslt-1.1.42.tar.xz          "https://download.gnome.org/sources/libxslt/1.1/libxslt-1.1.42.tar.xz"                                           "85ca62cac0d41fc77d3f6033da9df6fd73d20ea2fc18b0a3609ffb4110e1baeb"
fetch_verified pillow-11.1.0.tar.gz           "https://files.pythonhosted.org/packages/source/p/pillow/pillow-11.1.0.tar.gz"                                   "368da70808b36d73b4b390a8ffac11069f8a5c85f29eff1f1b01bcf3ef5b2a20"
fetch_verified lxml-5.3.0.tar.gz              "https://files.pythonhosted.org/packages/source/l/lxml/lxml-5.3.0.tar.gz"                                        "4e109ca30d1edec1ac60cdbe341905dc3b8f55b16855e03a54aaf59e51ec8c6f"

WASI_SDK="$CACHE_DIR/wasi-sdk-20.0"
if [[ ! -x "$WASI_SDK/bin/clang" ]]; then
  rm -rf "$WASI_SDK"
  tar -xzf "$DL/wasi-sdk-20.0-linux.tar.gz" -C "$CACHE_DIR"
fi

CLANG="$WASI_SDK/bin/clang"
WASI_SYSROOT="$WASI_SDK/share/wasi-sysroot"
AR="$WASI_SDK/bin/llvm-ar"
RANLIB="$WASI_SDK/bin/llvm-ranlib"
export CC="$CLANG --sysroot=$WASI_SYSROOT"
export CXX="$WASI_SDK/bin/clang++ --sysroot=$WASI_SYSROOT"
export AR RANLIB
export NM="$WASI_SDK/bin/llvm-nm"
export STRIP="$WASI_SDK/bin/llvm-strip"
export READELF="$WASI_SDK/bin/llvm-readelf"
WASI_CFLAGS="-O2 -g0 -I$SYSROOT/include -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS"

# Minimal WASI setjmp.h shim (MVP Wasm has no native setjmp; fatal libjpeg ERREXIT traps cleanly).
cat > "$SYSROOT/include/setjmp.h" <<'EOF'
#ifndef PW_WASI_SETJMP_H
#define PW_WASI_SETJMP_H
typedef int jmp_buf[1];
static inline int setjmp(jmp_buf env) { (void)env; return 0; }
static inline _Noreturn void longjmp(jmp_buf env, int val) { (void)env; (void)val; __builtin_trap(); }
#endif
EOF

# 1. zlib 1.3.1
if [[ ! -f "$SYSROOT/lib/libz.a" ]]; then
  rm -rf "$WORK_DIR/src/zlib-1.3.1"
  tar -xf "$DL/zlib-1.3.1.tar.xz" -C "$WORK_DIR/src"
  (cd "$WORK_DIR/src/zlib-1.3.1" && CFLAGS="$WASI_CFLAGS" ./configure --static --prefix="$SYSROOT" && make -j"$JOBS" libz.a && cp libz.a "$SYSROOT/lib/" && cp zlib.h zconf.h "$SYSROOT/include/")
fi

# 2. bzip2 1.0.8
if [[ ! -f "$SYSROOT/lib/libbz2.a" ]]; then
  rm -rf "$WORK_DIR/src/bzip2-1.0.8"
  tar -xzf "$DL/bzip2-1.0.8.tar.gz" -C "$WORK_DIR/src"
  (cd "$WORK_DIR/src/bzip2-1.0.8" && make -j"$JOBS" CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$WASI_CFLAGS" libbz2.a && cp libbz2.a "$SYSROOT/lib/" && cp bzlib.h "$SYSROOT/include/")
fi

# 3. sqlite3 3.45.3
if [[ ! -f "$SYSROOT/lib/libsqlite3.a" ]]; then
  rm -rf "$WORK_DIR/src/sqlite-autoconf-3450300"
  tar -xzf "$DL/sqlite-autoconf-3450300.tar.gz" -C "$WORK_DIR/src"
  (
    cd "$WORK_DIR/src/sqlite-autoconf-3450300"
    $CC $WASI_CFLAGS -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_ENABLE_FTS5=1 -DSQLITE_ENABLE_JSON1=1 -DSQLITE_ENABLE_RTREE=1 -c sqlite3.c -o sqlite3.o
    "$AR" rcs "$SYSROOT/lib/libsqlite3.a" sqlite3.o
    "$RANLIB" "$SYSROOT/lib/libsqlite3.a"
    cp sqlite3.h sqlite3ext.h "$SYSROOT/include/"
  )
fi

# 4. libjpeg 9f
if [[ ! -f "$SYSROOT/lib/libjpeg.a" ]]; then
  rm -rf "$WORK_DIR/src/jpeg-9f"
  tar -xzf "$DL/jpegsrc.v9f.tar.gz" -C "$WORK_DIR/src"
  (
    cd "$WORK_DIR/src/jpeg-9f"
    CFLAGS="$WASI_CFLAGS" ./configure --host=wasm32-unknown-wasi --build="$(./config.guess)" --prefix="$SYSROOT" --enable-static --disable-shared
    make -j"$JOBS" libjpeg.la
    make install-libLTLIBRARIES install-includeHEADERS
    cp jconfig.h "$SYSROOT/include/"
  )
fi

# 5. libxml2 2.12.9 (--without-http --without-ftp)
if [[ ! -f "$SYSROOT/lib/libxml2.a" ]]; then
  rm -rf "$WORK_DIR/src/libxml2-2.12.9"
  tar -xf "$DL/libxml2-2.12.9.tar.xz" -C "$WORK_DIR/src"
  (
    cd "$WORK_DIR/src/libxml2-2.12.9"
    python3 -c '
p = "xmlIO.c"
s = open(p).read()
s = s.replace("int duped_fd = dup(fileno(stdin));", "int duped_fd = -1; /* WASI has no dup() */")
s = s.replace("int duped_fd = dup(fileno(stdout));", "int duped_fd = -1; /* WASI has no dup() */")
open(p, "w").write(s)
'
    CFLAGS="$WASI_CFLAGS" LDFLAGS="-L$SYSROOT/lib -lwasi-emulated-signal -lwasi-emulated-process-clocks" \
      ./configure --host=wasm32-unknown-wasi --build="$(./config.guess)" --prefix="$SYSROOT" \
        --enable-static --disable-shared --without-python --without-http --without-ftp --without-threads \
        --without-lzma --without-iconv --without-icu --without-readline --without-history --without-modules --with-zlib="$SYSROOT"
    make -j"$JOBS" libxml2.la
    make install-libLTLIBRARIES install-binSCRIPTS install-pkgconfigDATA || true
    make -C include install
  )
fi

# 6. libxslt 1.1.42
if [[ ! -f "$SYSROOT/lib/libxslt.a" ]]; then
  rm -rf "$WORK_DIR/src/libxslt-1.1.42"
  tar -xf "$DL/libxslt-1.1.42.tar.xz" -C "$WORK_DIR/src"
  (
    cd "$WORK_DIR/src/libxslt-1.1.42"
    CFLAGS="$WASI_CFLAGS -I$SYSROOT/include/libxml2" LDFLAGS="-L$SYSROOT/lib -lwasi-emulated-signal -lwasi-emulated-process-clocks" \
    LIBXML_CFLAGS="-I$SYSROOT/include/libxml2" LIBXML_LIBS="-L$SYSROOT/lib -lxml2 -lz -lm" \
      ./configure --host=wasm32-unknown-wasi --build="$(./config.guess)" --prefix="$SYSROOT" \
        --enable-static --disable-shared --without-python --without-crypto --without-plugins --without-debugger --with-libxml-prefix="$SYSROOT"
    make -j"$JOBS" -C libxslt
    make -j"$JOBS" -C libexslt
    make -C libxslt install
    make -C libexslt install
  )
fi

# 7. CPython 3.12.0 + Pillow 11.1.0 + lxml 5.3.0
PY_SRC="$WORK_DIR/src/Python-3.12.0"
PIL_SRC="$WORK_DIR/src/pillow-11.1.0"
LXML_SRC="$WORK_DIR/src/lxml-5.3.0"
PY_BUILD="$WORK_DIR/build-python-wasi"
[[ -d "$PY_SRC" ]] || tar -xf "$DL/Python-3.12.0.tar.xz" -C "$WORK_DIR/src"
[[ -d "$PIL_SRC" ]] || tar -xzf "$DL/pillow-11.1.0.tar.gz" -C "$WORK_DIR/src"
[[ -d "$LXML_SRC" ]] || tar -xzf "$DL/lxml-5.3.0.tar.gz" -C "$WORK_DIR/src"

cat > "$PY_SRC/Modules/Setup.local" <<EOF
*static*
zlib zlibmodule.c -I$SYSROOT/include $SYSROOT/lib/libz.a
_bz2 _bz2module.c -I$SYSROOT/include $SYSROOT/lib/libbz2.a
_sqlite3 _sqlite/blob.c _sqlite/connection.c _sqlite/cursor.c _sqlite/microprotocols.c _sqlite/module.c _sqlite/prepare_protocol.c _sqlite/row.c _sqlite/statement.c _sqlite/util.c -I$PY_SRC/Modules/_sqlite -I$SYSROOT/include -DMODULE_NAME='\"sqlite3\"' $SYSROOT/lib/libsqlite3.a
EOF

rm -rf "$PY_BUILD"
mkdir -p "$PY_BUILD"
(
  cd "$PY_BUILD"
  CONFIG_SITE="$PY_SRC/Tools/wasm/config.site-wasm32-wasi" \
  CFLAGS="$WASI_CFLAGS" CPPFLAGS="-I$SYSROOT/include" \
  LDFLAGS="-L$SYSROOT/lib -lwasi-emulated-signal -lwasi-emulated-process-clocks -Wl,--stack-first -Wl,-z,stack-size=1048576" \
    "$PY_SRC/configure" --host=wasm32-unknown-wasi --build="$("$PY_SRC/config.guess")" \
      --with-build-python=/usr/bin/python3.12 --prefix=/usr/local \
      --disable-ipv6 --disable-test-modules --with-ensurepip=no --without-pymalloc PKG_CONFIG=false
  make -j"$JOBS" libpython3.12.a Modules/config.c
)

# Build lxml 5.3.0 static archive
LXML_OBJ="$WORK_DIR/obj-lxml"
rm -rf "$LXML_OBJ" && mkdir -p "$LXML_OBJ"
for src in etree _elementpath; do
  "$CLANG" "--sysroot=$WASI_SYSROOT" -O2 -g0 -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
    -I"$SYSROOT/include" -I"$PY_BUILD" -I"$PY_SRC/Include" -I"$PY_SRC/Include/internal" \
    -I"$SYSROOT/include/libxml2" -I"$LXML_SRC/src" -I"$LXML_SRC/src/lxml/includes" \
    -DCYTHON_CLINE_IN_TRACEBACK=0 '-DLXML_VERSION_STRING="5.3.0"' -Wno-deprecated-declarations \
    -c "$LXML_SRC/src/lxml/$src.c" -o "$LXML_OBJ/$src.o" &
done
wait
"$AR" rcs "$SYSROOT/lib/libpw_lxml.a" "$LXML_OBJ/etree.o" "$LXML_OBJ/_elementpath.o"
"$RANLIB" "$SYSROOT/lib/libpw_lxml.a"

# Build Pillow 11.1.0 _imaging static archive
# Patch Pillow's 7 1-arg METH_NOARGS functions to 2-arg PyCFunction for Wasm call_indirect type safety
python3 - "$PIL_SRC" <<'PYEOF'
import os, sys
pil_src = sys.argv[1]
patches = {
    "src/_imaging.c": [
        ("_isblock(ImagingObject *self)", "_isblock(ImagingObject *self, PyObject *Py_UNUSED(ignored))"),
        ("_getextrema(ImagingObject *self)", "_getextrema(ImagingObject *self, PyObject *Py_UNUSED(ignored))"),
        ("_getprojection(ImagingObject *self)", "_getprojection(ImagingObject *self, PyObject *Py_UNUSED(ignored))"),
        ("_split(ImagingObject *self)", "_split(ImagingObject *self, PyObject *Py_UNUSED(ignored))"),
        ("_getpalettemode(ImagingObject *self)", "_getpalettemode(ImagingObject *self, PyObject *Py_UNUSED(ignored))"),
        ("_chop_invert(ImagingObject *self)", "_chop_invert(ImagingObject *self, PyObject *Py_UNUSED(ignored))"),
    ],
    "src/encode.c": [
        ("_encode_to_pyfd(ImagingEncoderObject *encoder)", "_encode_to_pyfd(ImagingEncoderObject *encoder, PyObject *Py_UNUSED(ignored))"),
    ],
}
for rel, reps in patches.items():
    p = os.path.join(pil_src, rel)
    s = open(p).read()
    for old, new in reps:
        if new not in s:
            assert old in s, f"Pattern not found in {rel}: {old}"
            s = s.replace(old, new, 1)
    open(p, "w").write(s)
PYEOF

PIL_OBJ="$WORK_DIR/obj-pillow"
rm -rf "$PIL_OBJ" && mkdir -p "$PIL_OBJ"
python3 - "$CLANG" "$WASI_SYSROOT" "$SYSROOT" "$PY_BUILD" "$PY_SRC" "$PIL_SRC" "$PIL_OBJ" "$JOBS" <<'PYEOF'
import concurrent.futures, os, subprocess, sys
clang, wasi_sysroot, sysroot, py_build, py_src, pil_src, pil_obj, jobs = sys.argv[1:]
srcs = ["src/_imaging.c", "src/decode.c", "src/encode.c", "src/display.c", "src/map.c", "src/outline.c", "src/path.c"]
for fn in sorted(os.listdir(os.path.join(pil_src, "src", "libImaging"))):
    if fn.endswith(".c") and fn not in ("Jpeg2KDecode.c", "Jpeg2KEncode.c", "TiffDecode.c", "Dib.c"):
        srcs.append(os.path.join("src", "libImaging", fn))
base = [
    clang, f"--sysroot={wasi_sysroot}", "-O2", "-g0",
    "-D_WASI_EMULATED_SIGNAL", "-D_WASI_EMULATED_PROCESS_CLOCKS",
    f"-I{sysroot}/include", f"-I{py_build}", f"-I{py_src}/Include", f"-I{py_src}/Include/internal",
    f"-I{pil_src}/src/libImaging", "-DHAVE_LIBZ=1", "-DHAVE_LIBJPEG=1", '-DPILLOW_VERSION="11.1.0"',
    "-Werror=cast-function-type", "-Wno-cast-function-type-strict",
]
def one(rel):
    out = os.path.join(pil_obj, rel.replace("/", "__").replace(".", "_") + ".o")
    r = subprocess.run(base + ["-c", os.path.join(pil_src, rel), "-o", out], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"Failed {rel}:\n{r.stderr}")
with concurrent.futures.ThreadPoolExecutor(max_workers=int(jobs)) as ex:
    list(ex.map(one, srcs))
PYEOF
"$AR" rcs "$SYSROOT/lib/libpw_pillow_imaging.a" "$PIL_OBJ"/*.o
"$RANLIB" "$SYSROOT/lib/libpw_pillow_imaging.a"

# Register built-in modules in Modules/config.c and link python.wasm
(
  cd "$PY_BUILD"
  python3 - <<'PYEOF'
s = open("Modules/config.c").read()
if "PIL._imaging" not in s:
    decls = "\nextern PyObject* PyInit__imaging(void);\nextern PyObject* PyInit_etree(void);\nextern PyObject* PyInit__elementpath(void);\n"
    entries = '\n    {"PIL._imaging", PyInit__imaging},\n    {"_imaging", PyInit__imaging},\n    {"lxml.etree", PyInit_etree},\n    {"lxml._elementpath", PyInit__elementpath},\n'
    s = s.replace("/* -- ADDMODULE MARKER 1 -- */", "/* -- ADDMODULE MARKER 1 -- */" + decls, 1)
    s = s.replace("/* -- ADDMODULE MARKER 2 -- */", "/* -- ADDMODULE MARKER 2 -- */" + entries, 1)
    open("Modules/config.c", "w").write(s)
PYEOF
  make -j"$JOBS" python.wasm \
    SYSLIBS="$SYSROOT/lib/libpw_pillow_imaging.a $SYSROOT/lib/libpw_lxml.a $SYSROOT/lib/libjpeg.a $SYSROOT/lib/libexslt.a $SYSROOT/lib/libxslt.a $SYSROOT/lib/libxml2.a $SYSROOT/lib/libsqlite3.a $SYSROOT/lib/libbz2.a $SYSROOT/lib/libz.a -lm"
  rm -rf "$WORK_DIR/install"
  make install DESTDIR="$WORK_DIR/install" >/dev/null
)

cp "$PY_BUILD/python.wasm" "$OUT_DIR/bin/python.wasm"
rm -rf "$OUT_DIR/lib/python3.12"
cp -a "$WORK_DIR/install/usr/local/lib/python3.12" "$OUT_DIR/lib/python3.12"
rm -rf "$OUT_DIR/lib/python3.12/test" "$OUT_DIR/lib/python3.12/idlelib" "$OUT_DIR/lib/python3.12/tkinter" "$OUT_DIR/lib/python3.12/turtledemo"
find "$OUT_DIR/lib/python3.12" -name '__pycache__' -type d -exec rm -rf {} +
sha256sum "$OUT_DIR/bin/python.wasm"
