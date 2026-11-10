# Ninja with cl from the MSVC developer environment: the Visual Studio generator builds the dependencies one at a time.
configure() {
  export PATH="/c/Strawberry/perl/bin:$PATH"   # OpenSSL's configure rejects Git Bash's Cygwin Perl
  # CPython's PCbuild needs the 64-bit MSBuild; under Ninja the superbuild has no VS instance to derive it from.
  MSBUILD=$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -find 'MSBuild\Current\Bin\amd64\MSBuild.exe' | tr -d '\r')
  [ -n "$MSBUILD" ]
  export MSBUILD
  cmake -S engine/deps -B engine/deps/build -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DDEPS_ARCH=x64 \
    -DCMAKE_BUILD_TYPE=Release -DSLIC3R_CAD=OFF -DSLIC3R_GUI=OFF
}
has() { ninja -C engine/deps/build -t targets all | grep -q "^$1:"; }
