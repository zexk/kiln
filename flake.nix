{
  description = "Kiln — a game engine";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        nativeBuildInputs = with pkgs; [
          gcc
          gnumake
          pkg-config
        ];

        buildInputs = with pkgs; [
          libX11
          libGL
          vulkan-loader
          vulkan-headers
          shaderc
          stb
        ];
      in
      {
        formatter = pkgs.nixpkgs-fmt;

        devShells.default = pkgs.mkShell {
          inherit nativeBuildInputs buildInputs;
          packages = with pkgs; [
            gdb
            vulkan-validation-layers
            spirv-tools
          ];
          shellHook = ''
            echo "Kiln dev shell"
          '';
        };
      }
    );
}
