#!/bin/bash
#set -x 

PROJECT=P89990GA1
CONFIG=${PROJECT}_defconfig
GCC_PATH=${PWD}/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin
CC=${PWD}/prebuilts/clang/host/linux-x86/clang-r383902b1/bin/clang

export PATH=$GCC_PATH:$PATH

make -j6 -C kernel-4.19.157 O=../kernel-4.19.157/out/target/product/${PROJECT}/obj/KERNEL_OBJ ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- CLANG_TRIPLE=aarch64-linux-gnu- CC=${CC} ${CONFIG}
cp kernel-4.19.157/include out/target/product/${PROJECT}/obj/KERNEL_OBJ/ -rf
make -j6 V=1 -C kernel-4.19.157 O=../kernel-4.19.157/out/target/product/${PROJECT}/obj/KERNEL_OBJ ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- CLANG_TRIPLE=aarch64-linux-gnu- CC=${CC} 
make -j6 -C kernel-4.19.157 O=../kernel-4.19.157/out/target/product/${PROJECT}/obj/KERNEL_OBJ ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- CLANG_TRIPLE=aarch64-linux-gnu- CC=${CC} modules
make -j6 -C kernel-4.19.157 O=../kernel-4.19.157/out/target/product/${PROJECT}/obj/KERNEL_OBJ ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- CLANG_TRIPLE=aarch64-linux-gnu- CC=${CC} modules_install INSTALL_MOD_PATH=../vendor

