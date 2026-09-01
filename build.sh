#!/usr/bin/env sh
set -eu

BUILD_DIR=$(pwd)/build

cd libv
./configure --prefix=$BUILD_DIR
make
make install

cd ..

CPPFLAGS="-I$BUILD_DIR/include -I$BUILD_DIR/include/libv" \
LDFLAGS="-L$BUILD_DIR/lib" \
LIBS="-lv" \
./configure --prefix=$BUILD_DIR
make

