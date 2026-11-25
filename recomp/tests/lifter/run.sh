#!/bin/sh
# Differential test of the lifter: cases.c compiled as 32-bit x86 at several optimisation
# levels, translated and run, against the same code compiled natively.
#   tests/lifter/run.sh            (from recomp/, on an x86-64 Linux with gcc)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${OUT:-/tmp/lifter-test}
rm -rf "$out" && mkdir -p "$out"
status=0
for opt in -O0 -O1 -O2 -Os; do
    d="$out/$opt"
    mkdir -p "$d/gen"
    flags="-m32 $opt -fno-math-errno -ffreestanding -fno-pic -fno-pie -fno-stack-protector -mno-sse -mfpmath=387 -fno-asynchronous-unwind-tables"
    gcc $flags -c "$here/cases.c" -o "$d/cases32.o"
    gcc $flags -c "$here/minilibc.c" -o "$d/libc32.o"
    ld -m elf_i386 -Ttext 0x401000 -e c_arith -o "$d/cases.elf" "$d/cases32.o" "$d/libc32.o"
    python3 -I "$here/mkpe.py" "$d/cases.elf" "$d/cases.exe"
    python3 -I "$root/tools/lift.py" "$d/cases.exe" "$d/gen" --prefix T > "$d/lift.log"
    # Native build, every symbol prefixed n_.
    gcc -O1 -mfpmath=387 -fno-math-errno -ffreestanding -c "$here/cases.c" -o "$d/native.o"
    gcc -O1 -fno-math-errno -c "$here/minilibc.c" -o "$d/libcn.o"
    ld -r "$d/native.o" "$d/libcn.o" -o "$d/native_all.o"
    objcopy --prefix-symbols=n_ "$d/native_all.o" "$d/native_n.o"
    nm "$d/cases32.o" | awk '$2 == "T" && $3 ~ /^c_/ { print "NATIVE(" $3 ")" }' > "$d/native_list.h"
    g++ -std=c++17 -O1 -w -I"$root/runtime" -I"$d" -c "$here/harness.cpp" -o "$d/harness.o"
    for f in "$d"/gen/T_*.c; do gcc -O1 -w -I"$root/runtime" -I"$d/gen" -c "$f" -o "$f.o"; done
    g++ -std=c++17 -O1 -w -I"$root/runtime" -c "$root/runtime/runtime.cpp" -o "$d/runtime.o"
    g++ -std=c++17 -O1 -w -I"$root/runtime" -c "$root/runtime/host.cpp" -o "$d/host.o"
    g++ -std=c++17 -O1 -w -I"$root/runtime" -c "$root/runtime/log.cpp" -o "$d/log.o"
    g++ "$d/harness.o" "$d"/gen/*.o "$d/runtime.o" "$d/host.o" "$d/log.o" "$d/native_n.o" -o "$d/harness" -lpthread -lm
    echo "== $opt ($(tail -1 "$d/lift.log"))"
    "$d/harness" "$d/cases.exe" || status=1
done
exit $status
