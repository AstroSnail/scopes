{
  description = "Scopes retargetable programming language & infrastructure";

  inputs = {
    genie-src.url = "github:bkaradzic/genie";
    genie-src.flake = false;
    spirv-cross-src.url = "github:KhronosGroup/SPIRV-Cross";
    spirv-cross-src.flake = false;
  };

  outputs = { self, nixpkgs, genie-src, spirv-cross-src }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      nixpkgsFor = forAllSystems (system: import nixpkgs { inherit system; });

    in {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgsFor.${system};
          selfpkgs = self.packages.${system};
        in {
          genie = pkgs.callPackage ./genie.nix { inherit genie-src; };
          scopes = pkgs.callPackage ./scopes.nix {
            inherit spirv-cross-src;
            inherit (selfpkgs) genie;
            scopes-src = self;
          };
        });

      defaultPackage = forAllSystems (system: self.packages.${system}.scopes);

      hydraJobs = {
        tarball = forAllSystems (system:
          nixpkgsFor.${system}.releaseTools.sourceTarball {
            name = "scopes-tarball";
            src = self;
          }
        );
        coverage = forAllSystems (system:
          nixpkgsFor.${system}.releaseTools.coverageAnalysis {
            name = "scopes-coverage";
            src = self.hydraJobs.tarball.${system};
            
          }
        );
      };
    };
}
