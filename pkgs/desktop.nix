{ lib
, stdenv
, makeWrapper
, nodejs_26
, buildNpmPackage
, electron
, ffmpeg
, coreutils
, engine
, web
, server
, revision
, source ? ../desktop
}:

let
  shell = buildNpmPackage {
    pname = "pancakeslicer-desktop-shell";
    version = "0.4.2";
    src = source;
    nodejs = nodejs_26;
    npmDepsHash = "sha256-NK9qrqo+hcOQSlmvdx+njh079jY9pfWjiEjqyPVt0G4=";
    npmBuildScript = "compile";
    installPhase = ''
      mkdir -p $out/libexec/pancakeslicer-desktop
      cp -R out package.json $out/libexec/pancakeslicer-desktop/
    '';
  };
in
stdenv.mkDerivation {
  pname = "pancakeslicer-desktop";
  version = "0.4.2";
  dontUnpack = true;

  nativeBuildInputs = [ makeWrapper ];

  installPhase = ''
    mkdir -p $out/bin $out/libexec/pancakeslicer/engine $out/libexec/pancakeslicer/web
    cp ${shell}/libexec/pancakeslicer-desktop/out/main.js $out/libexec/pancakeslicer/
    cp ${shell}/libexec/pancakeslicer-desktop/package.json $out/libexec/pancakeslicer/
    cp ${engine}/bin/nonplanar_slice_tool $out/libexec/pancakeslicer/engine/
    cp -R ${engine}/resources $out/libexec/pancakeslicer/engine/
    cp ${web}/share/pancakeslicer/web/index.html $out/libexec/pancakeslicer/web/
    cp -R ${web}/share/pancakeslicer/web/assets $out/libexec/pancakeslicer/web/
    cp ${server}/bin/pancakeslicer-server $out/libexec/pancakeslicer/engine/orca-webapp-server
    cp ${server}/share/pancakeslicer/server/schemas.json $out/libexec/pancakeslicer/engine/
    printf '%s\n' '{}' > $out/libexec/pancakeslicer/engine/surface-manifest.json
    printf '%s\n' 'nix-built' > $out/libexec/pancakeslicer/engine/artifact-qualification.ready
    resource_hash=$(while IFS= read -r file; do
      relative=''${file#''$out/libexec/pancakeslicer/engine/resources/}
      printf '%s\0%s\n' "$relative" "$(sha256sum "$file" | cut -d' ' -f1)"
    done < <(find $out/libexec/pancakeslicer/engine/resources -type f -print | sort) | sha256sum | cut -d' ' -f1)
    native_hash=$(sha256sum $out/libexec/pancakeslicer/engine/nonplanar_slice_tool | cut -d' ' -f1)
    server_hash=$(sha256sum $out/libexec/pancakeslicer/engine/orca-webapp-server | cut -d' ' -f1)
    manifest_hash=$(sha256sum $out/libexec/pancakeslicer/engine/surface-manifest.json | cut -d' ' -f1)
    qualification_hash=$(sha256sum $out/libexec/pancakeslicer/engine/artifact-qualification.ready | cut -d' ' -f1)
    cat > $out/libexec/pancakeslicer/engine/COMPONENTS.json <<EOF
{"client_source":"${revision}","server_source":"${revision}","native_source":"${revision}","native_binary_sha256":"$native_hash","native_resources_sha256":"$resource_hash","server_binary_sha256":"$server_hash","native_manifest_sha256":"$manifest_hash","native_qualification_sha256":"$qualification_hash"}
EOF
    cp $out/libexec/pancakeslicer/engine/COMPONENTS.json $out/libexec/pancakeslicer/web/COMPONENTS.json
    printf '%s\n' '${revision}' > $out/libexec/pancakeslicer/web/REVISION.txt
    makeWrapper ${electron}/bin/electron $out/bin/pancakeslicer \
      --add-flags $out/libexec/pancakeslicer/main.js \
      --prefix PATH : ${lib.makeBinPath [ ffmpeg coreutils ]} \
      --run "cd $out/libexec/pancakeslicer"
  '';

  meta = {
    description = "PancakeSlicer Electron desktop application";
    homepage = "https://github.com/ZimengXiong/PancakeSlicer";
    license = lib.licenses.agpl3Only;
    platforms = [ "x86_64-linux" ];
    mainProgram = "pancakeslicer";
  };
}
