configure() {
  export PATH="/c/Strawberry/perl/bin:$PATH"   # OpenSSL's configure rejects Git Bash's Cygwin Perl
  cmake -S engine/deps -B engine/deps/build -G 'Visual Studio 18 2026' -A x64 -DDEPS_ARCH=x64 -DCMAKE_BUILD_TYPE=Release \
    -DSLIC3R_CAD=OFF -DSLIC3R_GUI=OFF
}
has() { [ -f "engine/deps/build/$1.vcxproj" ]; }
