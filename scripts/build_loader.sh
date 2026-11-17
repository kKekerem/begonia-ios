#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/mtkloader"
OUT="${1:-$ROOT/build}"
mkdir -p "$OUT"

# Auto-detect Android NDK LLVM toolchain in WSL Ubuntu, or fall back to system clang/lld
find_llvm_bin() {
    if [ -n "${LLVM_BIN:-}" ] && [ -x "$LLVM_BIN/clang" ]; then
        echo "$LLVM_BIN"
        return 0
    fi
    for d in "$HOME"/toolchains/android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin; do
        if [ -x "$d/clang" ] && [ -x "$d/ld.lld" ]; then
            echo "$d"
            return 0
        fi
    done
    if command -v clang >/dev/null 2>&1 && command -v ld.lld >/dev/null 2>&1; then
        dirname "$(command -v clang)"
        return 0
    fi
    return 1
}

BIN_DIR="$(find_llvm_bin)" || {
    echo "error: clang + ld.lld not found (set LLVM_BIN or install clang lld)" >&2
    exit 1
}

CLANG="$BIN_DIR/clang"
LLD="$BIN_DIR/ld.lld"
OBJCOPY="$BIN_DIR/llvm-objcopy"
if [ ! -x "$OBJCOPY" ]; then
    OBJCOPY="$(command -v llvm-objcopy || command -v aarch64-linux-gnu-objcopy)"
fi

CFLAGS=(
    --target=aarch64-none-elf
    -ffreestanding
    -nostdlib
    -fno-builtin
    -mgeneral-regs-only
    -mstrict-align
    -Os
    -Wall
    -Wextra
    -Werror
    -I"$SRC/include"
)

OBJS=()
for sfile in "$SRC"/*.S; do
    obj="$OUT/$(basename "${sfile%.S}").o"
    "$CLANG" "${CFLAGS[@]}" -c "$sfile" -o "$obj"
    OBJS+=("$obj")
done

for cfile in "$SRC"/*.c; do
    obj="$OUT/$(basename "${cfile%.c}").o"
    "$CLANG" "${CFLAGS[@]}" -c "$cfile" -o "$obj"
    OBJS+=("$obj")
done

"$LLD" -T "$SRC/link.ld" "${OBJS[@]}" -o "$OUT/mtkloader.elf"
"$OBJCOPY" -O binary "$OUT/mtkloader.elf" "$OUT/mtkloader.bin"
ls -lh "$OUT/mtkloader.elf" "$OUT/mtkloader.bin"
