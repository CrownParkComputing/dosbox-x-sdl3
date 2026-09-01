#!/usr/bin/env bash
# Build the DOS core for Android: libretrodos.so, exposing the plain-C host
# API in include/retrodos_host.h.
#
#   ANDROID_ABI=arm64-v8a ./android/build-core.sh
#   ANDROID_ABI=x86_64    ./android/build-core.sh    # emulator
#
# Shaped after retro-x86's core-as-shared-library: the emulator becomes a .so
# with one plain-C door, and every host (SDL3+ImGui app, test harness) is a
# dlopen client of that door.
#
# Two things this deliberately does NOT do, both of which the previous
# Retro-Dosbox Android build did and paid for:
#
#  1. It does not borrow prebuilt SDL/libpng out of a retired app's source
#     tree. That made the build unreproducible on any other machine and
#     impossible in CI. SDL3 is built from source here.
#  2. It does not share a configured tree with the host build. Autotools trees
#     are single-config: an Android ./configure over a host tree leaves stale
#     objects that link with "incompatible with aarch64linux", or silently
#     compiles host sources with the NDK compiler.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_API="${ANDROID_API:-28}"   # bionic gained iconv_open at 28; DOSBox-X needs it
ANDROID_NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/30.0.14904198}"
SDL3_TAG="${SDL3_TAG:-release-3.2.20}"
JOBS="${JOBS:-$(nproc)}"

OUT="$HERE/build/$ANDROID_ABI"
SDL3_SRC="$HERE/build/SDL"
SDL3_PREFIX="$OUT/sdl3"
TREE="$HERE/build/tree-$ANDROID_ABI"   # its own configured tree, never shared

case "$ANDROID_ABI" in
    arm64-v8a)   TRIPLE=aarch64-linux-android ;;
    x86_64)      TRIPLE=x86_64-linux-android ;;
    armeabi-v7a) TRIPLE=armv7a-linux-androideabi ;;
    *) echo "error: unsupported ANDROID_ABI '$ANDROID_ABI'" >&2; exit 1 ;;
esac

[ -d "$ANDROID_NDK" ] || { echo "error: no NDK at $ANDROID_NDK" >&2; exit 1; }

TOOLCHAIN="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64"
export CC="$TOOLCHAIN/bin/${TRIPLE}${ANDROID_API}-clang"
export CXX="$TOOLCHAIN/bin/${TRIPLE}${ANDROID_API}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"

[ -x "$CC" ] || { echo "error: no compiler at $CC" >&2; exit 1; }

mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# 1. SDL3 for Android
# ---------------------------------------------------------------------------
if [ ! -f "$SDL3_PREFIX/lib/libSDL3.so" ]; then
    echo "==> SDL3 ($SDL3_TAG) for $ANDROID_ABI"
    [ -d "$SDL3_SRC" ] || git clone --depth 1 --branch "$SDL3_TAG" \
        https://github.com/libsdl-org/SDL.git "$SDL3_SRC"
    cmake -S "$SDL3_SRC" -B "$OUT/sdl3-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="android-$ANDROID_API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL3_PREFIX" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF
    cmake --build "$OUT/sdl3-build" -j"$JOBS"
    cmake --install "$OUT/sdl3-build"
fi
echo "==> SDL3: $SDL3_PREFIX/lib/libSDL3.so"

# SDL3 has no sdl3-config; the tree finds it through pkg-config.
export PKG_CONFIG_PATH="$SDL3_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$SDL3_PREFIX/lib/pkgconfig"

# ---------------------------------------------------------------------------
# 2. DOSBox-X tree, configured for Android
# ---------------------------------------------------------------------------
if [ ! -d "$TREE" ]; then
    echo "==> cloning a private tree for $ANDROID_ABI"
    git -C "$ROOT" worktree list >/dev/null 2>&1 || true
    mkdir -p "$TREE"
    # Copy rather than git-clone: the working tree may have uncommitted work
    # in progress, and that is what we want to build.
    tar -C "$ROOT" --exclude=android/build --exclude=.git -cf - . | tar -C "$TREE" -xf -
fi

cd "$TREE"
[ -f configure ] || ./autogen.sh

if [ ! -f config.h ]; then
    echo "==> configure for $TRIPLE (API $ANDROID_API)"
    # -fPIC everywhere: these objects go into a shared library.
    # Feature set matches the known-good SDL3 config; TTF and the OpenGL
    # output backend are not ported to SDL3 yet.
    ./configure \
        --host="$TRIPLE" \
        --enable-sdl3 \
        --disable-opengl --disable-printer --disable-sdlnet \
        --disable-freetype --disable-libfluidsynth \
        --disable-alsa-midi --disable-avcodec \
        CFLAGS="-fPIC -O2 -g0 -DANDROID" \
        CXXFLAGS="-fPIC -O2 -g0 -DANDROID" \
        || { echo "configure failed; see $TREE/config.log" >&2; exit 1; }
fi

grep -q '^#define C_SDL3 1' config.h || { echo "error: SDL3 not selected" >&2; exit 1; }

echo "==> building the engine ($JOBS jobs)"
make -j"$JOBS"

# ---------------------------------------------------------------------------
# 3. Link the engine into one shared library
# ---------------------------------------------------------------------------
# The engine is pulled in WHOLE: nothing in the host API references most of
# DOSBox-X directly -- the mainloop pulls it in at runtime -- so without
# --whole-archive the linker would discard almost the entire emulator.
#
# The archive list comes from make expanding dosbox_x_LDADD, not from a glob:
# a glob over src/*/lib*.a silently misses the nested ones (libs/gui_tk,
# hardware/mame, hardware/reSID) and the failure only appears at dlopen as an
# undefined symbol. LDADD also repeats libgui.a, which is harmless normally
# but is a duplicate-symbol error under --whole-archive, so it is deduped.
echo "==> linking libretrodos.so"
LDADD_RAW="$(make -C src -s --eval='__print_ldadd:; @echo $(dosbox_x_LDADD)' __print_ldadd)"
[ -n "$LDADD_RAW" ] || { echo "error: could not read dosbox_x_LDADD" >&2; exit 1; }
LIBS_LINE="$(sed -n 's/^LIBS = //p' src/Makefile | head -1)"

ARCHIVES=""; EXTRA=""; seen=" "
for a in $LDADD_RAW; do
    case "$a" in
        *.a) case "$seen" in *" $a "*) continue ;; esac
             seen="$seen$a "; ARCHIVES="$ARCHIVES $TREE/src/$a" ;;
        -l*|-L*) EXTRA="$EXTRA $a" ;;
    esac
done

# shellcheck disable=SC2086
"$CXX" -shared -fPIC -o "$OUT/libretrodos.so" \
    -Wl,--whole-archive $ARCHIVES "$TREE"/src/*.o -Wl,--no-whole-archive \
    -L"$SDL3_PREFIX/lib" -lSDL3 \
    $LIBS_LINE $EXTRA \
    -lm -ldl -llog -landroid \
    -Wl,--no-undefined-version

# ---------------------------------------------------------------------------
# 4. Prove the ABI is actually there
# ---------------------------------------------------------------------------
echo "==> verifying the host ABI"
"$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$OUT/libretrodos.so" > "$OUT/exported.syms"
missing=0
for sym in $(grep -oE '\bretrodos_host_[a-z_]+\(' "$ROOT/include/retrodos_host.h" \
             | tr -d '(' | sort -u); do
    grep -q " $sym\$" "$OUT/exported.syms" || { echo "    MISSING: $sym"; missing=1; }
done
[ "$missing" = 0 ] || { echo "error: the host ABI is incomplete" >&2; exit 1; }

echo "==> SDL linkage:"
"$TOOLCHAIN/bin/llvm-readelf" -d "$OUT/libretrodos.so" | grep -i 'NEEDED.*SDL' || true

cp -f "$SDL3_PREFIX/lib/libSDL3.so" "$OUT/"
echo "==> done:"
ls -lh "$OUT/libretrodos.so" "$OUT/libSDL3.so"
