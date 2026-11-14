{
  description = "PancakeSlicer, a non-planar 3D printing slicer";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      version = "0.4.8";
      releases = {
        x86_64-linux = { arch = "x86_64"; hash = "sha256:953551c217362e2ad20495839f59f1347cd82f809d6890493fe8b102c72ec918"; };
        aarch64-linux = { arch = "arm64"; hash = "sha256:e8804a0e2ef00ce68d1ab29ff3bae9423a100a89f6b92df52086e6bfd97d6370"; };
      };
      forAll = f: nixpkgs.lib.genAttrs (builtins.attrNames releases) (system: f system nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (system: pkgs:
        let
          pname = "pancakeslicer";
          src = pkgs.fetchurl {
            url = "https://github.com/ZimengXiong/PancakeSlicer/releases/download/v${version}/PancakeSlicer-${version}-linux-${releases.${system}.arch}.AppImage";
            inherit (releases.${system}) hash;
          };
          contents = pkgs.appimageTools.extract { inherit pname version src; };
        in
        {
          default = pkgs.appimageTools.wrapType2 {
            inherit pname version src;
            extraInstallCommands = ''
              install -Dm444 ${contents}/pancakeslicer.desktop -t $out/share/applications
              substituteInPlace $out/share/applications/pancakeslicer.desktop --replace-fail 'Exec=AppRun' 'Exec=pancakeslicer'
              cp -r ${contents}/usr/share/icons $out/share
            '';
            meta = {
              description = "Non-planar 3D printing slicer";
              homepage = "https://github.com/ZimengXiong/PancakeSlicer";
              license = nixpkgs.lib.licenses.agpl3Only;
              sourceProvenance = [ nixpkgs.lib.sourceTypes.binaryNativeCode ];
              mainProgram = pname;
              platforms = builtins.attrNames releases;
            };
          };
        });

      apps = forAll (system: pkgs: {
        default = { type = "app"; program = nixpkgs.lib.getExe self.packages.${system}.default; };
      });

      devShells = forAll (system: pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            go nodejs_26 python3 git
            cmake ninja pkg-config autoconf automake libtool texinfo nasm yasm gettext ccache perl
            curl openssl libGL dbus fontconfig freetype xorg.libX11 xorg.libXrandr xorg.libXinerama xorg.libXcursor xorg.libXi
            systemd libva libjpeg xz libxkbcommon wayland wayland-protocols kdePackages.extra-cmake-modules
          ];
        };
      });
    };
}
