# Ninja with cl and ccache. ccache cannot cache cl precompiled headers or /Zi objects, so both are off.
configure() {
  cmake -S engine -B build-native -G 'Ninja Multi-Config' -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "${common[@]}" "${ccache[@]}" \
    -DSLIC3R_PCH=OFF -DSLIC3R_MSVC_PDB=OFF
}
