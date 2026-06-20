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
          cmake
          ninja
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

        checkInputs = [ pkgs.criterion ];
      in
      {
        formatter = pkgs.nixpkgs-fmt;

        packages.default = pkgs.stdenv.mkDerivation {
          pname = "kiln";
          version = "0.1.0";
          src = ./.;
          inherit nativeBuildInputs buildInputs checkInputs;
          doCheck = true;
        };

        devShells.default = pkgs.mkShell {
          inherit nativeBuildInputs;
          buildInputs = buildInputs ++ checkInputs;
          packages = with pkgs; [
            gdb
            vulkan-validation-layers
            spirv-tools
          ];
          shellHook = ''
            # The validation layers are on PATH but the Vulkan loader can't find
            # their manifests without this — otherwise it silently runs none.
            export VK_LAYER_PATH="${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d"
            # Generate .clangd from NIX_CFLAGS_COMPILE so clangd finds system headers
            ${pkgs.bash}/bin/bash scripts/gen_clangd.sh
            echo "Kiln dev shell"
          '';
        };
      }
    );
}
