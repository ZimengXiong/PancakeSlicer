configure() {
  cmake -S engine/deps -B engine/deps/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSLIC3R_CAD=OFF -DSLIC3R_GUI=OFF
}
has() { ninja -C engine/deps/build -t targets all | grep -q "^$1:"; }
