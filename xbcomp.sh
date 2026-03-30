rm -rf build-xbox

export NXDK_DIR=$(pwd)/nxdk

cmake -DCMAKE_TOOLCHAIN_FILE=$NXDK_DIR/share/toolchain-nxdk.cmake -DPLATFORM=xbox -B build-xbox -DNXDK_DIR=$NXDK_DIR .

cd build-xbox

make -j$(nproc)

$NXDK_DIR/tools/cxbe/cxbe -OUT:butterscotch.xbe butterscotch.exe

mkdir -p iso_root

cp butterscotch.xbe iso_root/
cp -r ../undertale/* iso_root/

/hdd/Butterscotch/nxdk/tools/extract-xiso/extract-xiso -c iso_root/ butterscotch.iso
