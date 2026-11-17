{ pkgs }:

pkgs.mkShell {
  packages = with pkgs; [
    cmake
    ninja
    pkg-config
    gcc
    gdb
    clang-tools
    nodejs_26
    go_1_27
    python313
    python313Packages.pip
    electron
    ffmpeg
    coreutils
  ];

  nativeBuildInputs = with pkgs; [
    boost
    tbb
    openssl
    curl
    freetype
    zlib
    libpng
    libjpeg
    libtiff
    libwebp
    expat
    libxml2
    sqlite
  ];

  shellHook = ''
    export ORCA_WEB_TOOL="$PWD/result-engine/bin/nonplanar_slice_tool"
    export ORCA_WEB_RESOURCES="$PWD/result-engine/resources"
    export ORCA_WEB_STATIC="$PWD/web/dist"
  '';
}
