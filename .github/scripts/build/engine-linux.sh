configure() {
  cmake -S engine -B build-native -G 'Ninja Multi-Config' "${common[@]}" "${ccache[@]}"
}
