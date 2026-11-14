{ lib, buildGoModule, source ? ../server }:

buildGoModule {
  pname = "pancakeslicer-server";
  version = "0.4.2";
  src = source;
  vendorHash = null;
  subPackages = [ "." ];
  ldflags = [ "-s" "-w" ];

  postInstall = ''
    mv $out/bin/server $out/bin/pancakeslicer-server
    install -Dm644 schemas.json $out/share/pancakeslicer/server/schemas.json
  '';

  meta = {
    description = "PancakeSlicer's Go web service";
    homepage = "https://github.com/ZimengXiong/PancakeSlicer";
    license = lib.licenses.agpl3Only;
    platforms = [ "x86_64-linux" ];
  };
}
