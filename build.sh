BUILD_DATE=$(date "+%Y%m%d-%H%M")

#ccache
export CCACHE_DIR="$HOME/.cache/ccache_mikernel"
echo "CCACHE_DIR: [$CCACHE_DIR]"

MAKE_ARGS=(
    O=out
    "CC=ccache clang"
    "CXX=ccache clang++"
    AR=llvm-ar
    NM=llvm-nm
    OBJDUMP=llvm-objdump
    STRIP=llvm-strip
    LD=ld.lld
    CROSS_COMPILE=aarch64-linux-gnu-
    CROSS_COMPILE_ARM32=arm-linux-gnueabi-
    LLVM=1
    LLVM_IAS=1
)

local_version_str="-perf"
local_version_date_str="-OverHeat-Next-$(date +%Y%m%d)"

KSU_ZIP_STR=noksu
if [ "$2" == "ksu" ]; then
    KSU_E=1
    KSU_ZIP_STR=ksu
else
    KSU_E=0
fi

rm -rf out/
rm -rf AnyKernel3/

#setting up AK
git clone https://github.com/mtkpapa/AnyKernel3 -b master

if [ $KSU_E -eq 1 ]; then
    echo "Downloading KernelSU-Next"
    curl -LSs "https://raw.githubusercontent.com/mtkpapa/KernelSU-Next/next-susfs/kernel/setup.sh" | bash -s next-susfs
else 
    echo "Building without KernelSU-Next"
fi

#----------------------build stuff here

echo "======= START OF BUILD ======="
make "${MAKE_ARGS[@]}" rosemary_defconfig

sed -i "s/${local_version_str}/${local_version_date_str}/g" out/.config

if [ $KSU_E -eq 1 ]; then
    scripts/config --file out/.config -e KSU \
    -e KSU_SUSFS_HAS_MAGIC_MOUNT \
    -d KSU_SUSFS_SUS_PATH \
    -e KSU_SUSFS_SUS_MOUNT \
    -e KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT \
    -e KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT \
    -e KSU_SUSFS_SUS_KSTAT \
    -d KSU_SUSFS_SUS_OVERLAYFS \
    -e KSU_SUSFS_TRY_UMOUNT \
    -e KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT \
    -e KSU_SUSFS_SPOOF_UNAME \
    -e KSU_SUSFS_ENABLE_LOG \
    -e KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS \
    -e KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG \
    -d KSU_SUSFS_OPEN_REDIRECT \
    -d KSU_SUSFS_SUS_SU
else
scripts/config --file out/.config -d KSU \
    -d KSU_SUSFS_HAS_MAGIC_MOUNT \
    -d KSU_SUSFS_SUS_PATH \
    -d KSU_SUSFS_SUS_MOUNT \
    -d KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT \
    -d KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT \
    -d KSU_SUSFS_SUS_KSTAT \
    -d KSU_SUSFS_SUS_OVERLAYFS \
    -d KSU_SUSFS_TRY_UMOUNT \
    -d KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT \
    -d KSU_SUSFS_SPOOF_UNAME \
    -d KSU_SUSFS_ENABLE_LOG \
    -d KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS \
    -d KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG \
    -d KSU_SUSFS_OPEN_REDIRECT \
    -d KSU_SUSFS_SUS_SU
fi

make "${MAKE_ARGS[@]}" -j$(nproc --all)
echo "======= END OF BUILD ======="

ZIP_NAME="OverHeat-Next-$(date "+%Y%m%d-%H%M").zip"

if [ -f "out/arch/arm64/boot/Image.gz-dtb" ]; then
    echo "Image found. Build successful"
    cd AnyKernel3
	cp ../out/arch/arm64/boot/Image.gz-dtb Image.gz-dtb
	zip -r9 ../$ZIP_NAME -- *
	cd ..
	cp $ZIP_NAME ../
	rm -rf $ZIP_NAME
else
    echo "Image not found. Build failed!"
    exit 1
fi

echo "======= CLEANING UP ======="

rm -rf KernelSU-Next/ && echo "  RM      KernelSU-Next"
rm -rf out/ && echo "  RM      out"
rm -rf AnyKernel3 && echo "  RM      AnyKernel3"
