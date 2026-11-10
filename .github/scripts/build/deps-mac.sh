configure() {
  cmake -S engine/deps -B engine/deps/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=$([ "$1" = x64 ] && echo x86_64 || echo arm64) -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    '-DCMAKE_IGNORE_PREFIX_PATH=/opt/local;/usr/local;/opt/homebrew' -DSLIC3R_CAD=OFF -DSLIC3R_GUI=OFF
}
has() { ninja -C engine/deps/build -t targets all | grep -q "^$1:"; }
