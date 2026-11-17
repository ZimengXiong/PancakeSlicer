{
  description = "PancakeSlicer native Linux packaging and development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      revision = if self ? rev then self.rev else self.dirtyRev;
      forEachSystem = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs {
        inherit system;
        config.allowUnfree = true;
      }));
    in {
      packages = forEachSystem (pkgs: {
        engine = pkgs.callPackage ./pkgs/engine.nix { inherit self; };
        web = pkgs.callPackage ./pkgs/web.nix { };
        server = pkgs.callPackage ./pkgs/server.nix { };
        desktop = pkgs.callPackage ./pkgs/desktop.nix {
          engine = self.packages.${pkgs.system}.engine;
          web = self.packages.${pkgs.system}.web;
          server = self.packages.${pkgs.system}.server;
          inherit revision;
        };
        default = self.packages.${pkgs.system}.desktop;
      });

      apps = forEachSystem (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.system}.desktop}/bin/pancakeslicer";
        };
        server = {
          type = "app";
          program = "${self.packages.${pkgs.system}.server}/bin/pancakeslicer-server";
        };
      });

      devShells = forEachSystem (pkgs: {
        default = pkgs.callPackage ./pkgs/shell.nix { };
      });

      checks = forEachSystem (pkgs: {
        flake-eval = pkgs.runCommand "pancakeslicer-flake-check" { } ''
          test -f ${./flake.nix}
          test -d ${./pkgs}
          mkdir -p $out
        '';
      });
    };
}
