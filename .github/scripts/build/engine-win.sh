configure() {
  cmake -S engine -B build-native -G 'Visual Studio 18 2026' -A x64 "${common[@]}"
}
