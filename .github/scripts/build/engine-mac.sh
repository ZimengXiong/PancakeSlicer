configure() {
  cmake -S engine -B build-native -G 'Ninja Multi-Config' "${common[@]}" "${ccache[@]}" \
    -DCMAKE_OSX_ARCHITECTURES=$([ "$1" = x64 ] && echo x86_64 || echo arm64) -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    '-DCMAKE_IGNORE_PREFIX_PATH=/opt/local;/usr/local;/opt/homebrew' -DLIBLZMA_INCLUDE_DIR="$(brew --prefix xz)/include"
}
