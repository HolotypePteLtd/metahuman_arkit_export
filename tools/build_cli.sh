#!/bin/sh
# Build bake-arkit-dna. Works around the sandbox hiding /usr/lib64:
#  - scratch/bin/ld  -> real ld.bfd (system ld symlink is dangling here)
#  - scratch/lib/    -> copies of glibc linker scripts with /usr/lib64
#                       rewritten to /lib64 (INPUT() paths are absolute)
# Fetches pinned OpenRigLogic only if not already present.
set -e
ROOT=/mnt/sdd_1tb/mh_arkit_export
SRC=$ROOT/scratch/OpenRigLogic
BUILD=$ROOT/scratch/build-cli
PIN=d3d329bbd6992ac38adc1c1b24203ceb998f75c0

# --- toolchain fixups ---
mkdir -p "$ROOT/scratch/bin" "$ROOT/scratch/lib"
ln -sf /usr/bin/ld.bfd "$ROOT/scratch/bin/ld"
for f in /lib64/libc.so /lib64/libm.so /lib64/libpthread.so /lib64/libdl.so /lib64/librt.so; do
  [ -f "$f" ] || continue
  sed 's|/usr/lib64|/lib64|g' "$f" > "$ROOT/scratch/lib/$(basename "$f")"
done
export PATH="$ROOT/scratch/bin:$PATH"
export LIBRARY_PATH="$ROOT/scratch/lib:/usr/lib:/lib64"
# -B puts the rewritten linker scripts ahead of the internal /lib64 search
# order (LIBRARY_PATH alone is searched after the built-in dirs, so the
# original /lib64/libc.so would still hand ld a hidden /usr/lib64 path).
CC_FLAGS="-B$ROOT/scratch/lib/"
echo 'int main(){return 0;}' > "$ROOT/scratch/linktest.c"
/usr/bin/cc $CC_FLAGS -o "$ROOT/scratch/linktest" "$ROOT/scratch/linktest.c"
"$ROOT/scratch/linktest" && echo "SANITY LINK OK" || exit 1
rm -f "$ROOT/scratch/linktest" "$ROOT/scratch/linktest.c"

# --- pinned source ---
if [ ! -f "$SRC/.git/HEAD" ]; then
  rm -rf "$SRC"; mkdir -p "$SRC"; git init -q "$SRC"
  git -C "$SRC" fetch --depth 1 https://github.com/EpicGames/OpenRigLogic.git "$PIN"
  git -C "$SRC" checkout --detach -q FETCH_HEAD
fi
[ "$(git -C "$SRC" rev-parse HEAD)" = "$PIN" ] || { echo "wrong commit" >&2; exit 1; }
[ -z "$(git -C "$SRC" status --porcelain)" ] || { echo "dirty checkout" >&2; exit 1; }

# --- configure + build ---
rm -rf "$BUILD"
/usr/bin/cmake -S "$ROOT/cli" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DLINK_STATIC_STDLIB=OFF \
      -DCMAKE_EXE_LINKER_FLAGS="$CC_FLAGS" \
      -DOPENRIGLOGIC_SOURCE="$SRC" -G Ninja
/usr/bin/cmake --build "$BUILD" -j"$(nproc)"
echo "BINARY: $BUILD/bake-arkit-dna"
