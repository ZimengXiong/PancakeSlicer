{ lib
, stdenv
, cmake
, ninja
, pkg-config
, boost
, tbb
, openssl
, curl
, freetype
, zlib
, libpng
, libjpeg
, libtiff
, libwebp
, expat
, libxml2
, sqlite
, gettext
, libGL
, libGLU
, xorg
, wayland
, mesa
, self
}:

stdenv.mkDerivation {
  pname = "pancakeslicer-headless-engine";
  version = "0.4.2";
  src = self;

  nativeBuildInputs = [ cmake ninja pkg-config gettext ];
  buildInputs = [
    boost tbb openssl curl freetype zlib libpng libjpeg libtiff libwebp expat
    libxml2 sqlite libGL libGLU mesa xorg.libX11 xorg.libXext xorg.libXrender
    xorg.libXfixes xorg.libXi xorg.libXrandr wayland
  ];

  dontConfigure = true;
  configurePhase = ''
    cmake -S engine -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DSLIC3R_GUI=OFF \
      -DSLIC3R_CAD=OFF \
      -DSLIC3R_STATIC=OFF \
      -DBUILD_TESTS=ON \
      -DCMAKE_INSTALL_PREFIX=$out
  '';

  buildPhase = ''
    cmake --build build --target nonplanar_slice_tool --parallel $NIX_BUILD_CORES
  '';

  installPhase = ''
    install -Dm755 build/tests/fff_print/nonplanar_slice_tool $out/bin/nonplanar_slice_tool
    cp -R engine/resources $out/resources
    chmod -R u+rwX,go+rX $out/resources
  '';

  enableParallelBuilding = true;
  strictDeps = true;

  meta = {
    description = "PancakeSlicer's headless non-planar slicing engine";
    homepage = "https://github.com/ZimengXiong/PancakeSlicer";
    license = lib.licenses.agpl3Only;
    platforms = [ "x86_64-linux" ];
  };
}
