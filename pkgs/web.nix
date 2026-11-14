{ lib, buildNpmPackage, nodejs_26, source ? ../. }:

buildNpmPackage {
  pname = "pancakeslicer-web";
  version = "0.4.2";
  src = source;

  nodejs = nodejs_26;
  npmRoot = "web";
  npmDepsHash = "sha256-FuxSU9tv/hcRBMrtHjck+f0476xmz21W6RUFfMSZhKY=";

  npmBuildScript = "build";

  installPhase = ''
    mkdir -p $out/share/pancakeslicer/web
    cp -R dist/. $out/share/pancakeslicer/web/
  '';

  meta = {
    description = "PancakeSlicer's web interface";
    homepage = "https://github.com/ZimengXiong/PancakeSlicer";
    license = lib.licenses.agpl3Only;
    platforms = [ "x86_64-linux" ];
  };
}
