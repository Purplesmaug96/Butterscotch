rm -rf build-xbox

export NXDK_DIR=$(pwd)/nxdk

cmake -DCMAKE_TOOLCHAIN_FILE=$NXDK_DIR/share/toolchain-nxdk.cmake -DPLATFORM=xbox -B build-xbox -DNXDK_DIR=$NXDK_DIR .

make -C build-xbox -j$(nproc)

$NXDK_DIR/tools/cxbe/cxbe -OUT:build-xbox/butterscotch.xbe build-xbox/butterscotch.exe
