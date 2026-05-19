set -e

rm -rf build-xbox

export NXDK_DIR=$(pwd)/vendor/nxdk

cmake -DCMAKE_TOOLCHAIN_FILE=$NXDK_DIR/share/toolchain-nxdk.cmake -DPLATFORM=xbox -DAUDIO_BACKEND=xbox -B build-xbox -DNXDK_DIR=$NXDK_DIR .

cd build-xbox

make -j$(nproc)

$NXDK_DIR/tools/cxbe/cxbe -OUT:default.xbe butterscotch.exe

mkdir -p iso_root

cp default.xbe iso_root/
cp -r ../undertale/* iso_root/

$NXDK_DIR/tools/extract-xiso/extract-xiso -c iso_root/ butterscotch.iso
