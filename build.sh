#!/bin/bash

# HOME path
export HOME=/home/saintz

echo
echo "Clean Build Directory"
echo 

make clean && make mrproper

# Compiler environment
mkdir -p out
export CLANG_PATH=$HOME/proton-clang/install/bin
export PATH="$CLANG_PATH:$PATH"
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-
export KBUILD_BUILD_USER=SaintZ13
export KBUILD_BUILD_HOST=1337

echo
echo "Setting defconfig"
echo

make CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip O=out schwifty_defconfig

echo
echo "Compiling kernel"
echo

make CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip O=out -j$(nproc --all) || exit 1
