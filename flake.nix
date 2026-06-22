{
  description = "Kiln — a game engine";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    # ── System-agnostic outputs ──────────────────────────────────────────────
    {
      # Helper for downstream games that use kiln as a flake input.
      # Usage:
      #   packages.default = inputs.kiln.lib.mkKilnGame {
      #     inherit pkgs;
      #     pname = "mygame";
      #     src = ./.;
      #   };
      lib.mkKilnGame =
        { pkgs
        , pname
        , version ? "0.1.0"
        , src
        , cmakeTarget ? pname
        , extraCmakeFlags ? []
        , extraBuildInputs ? []
        , installPhase ? ''
            mkdir -p $out/bin $out/share/${pname}/shaders
            cp ${cmakeTarget} $out/bin/
            cp shaders/*.spv $out/share/${pname}/shaders/
            [ -d assets ] && cp -r assets $out/share/${pname}/ || true
          ''
        , meta ? {}
        }:
        pkgs.stdenv.mkDerivation {
          inherit pname version src meta installPhase;
          nativeBuildInputs = with pkgs; [ gcc cmake ninja pkg-config shaderc ];
          buildInputs = with pkgs; [
            libX11
            vulkan-loader
            vulkan-headers
            shaderc
            stb
          ] ++ extraBuildInputs;
          cmakeFlags = [
            "-DKILN_DIR=${self.outPath}"
            "-DBUILD_TESTING=OFF"
          ] ++ extraCmakeFlags;
          buildPhase = "cmake --build . --target ${cmakeTarget}";
        };
    }

    # ── Per-system outputs ───────────────────────────────────────────────────
    // flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        mingw = pkgs.pkgsCross.mingwW64;

        hostTools = with pkgs; [
          cmake
          ninja
          pkg-config
          shaderc
        ];

        linuxBuildInputs = with pkgs; [
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

        # ── Linux native package ─────────────────────────────────────────────
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "kiln";
          version = "0.1.0";
          src = ./.;
          nativeBuildInputs = with pkgs; [ gcc ] ++ hostTools;
          buildInputs = linuxBuildInputs ++ checkInputs;
          doCheck = true;
        };

        # ── Buffon's Needle demo ─────────────────────────────────────────────
        packages.buffon = pkgs.stdenv.mkDerivation {
          pname = "kiln-demo-buffon";
          version = "0.1.0";
          meta.mainProgram = "buffon";
          src = ./.;
          nativeBuildInputs = with pkgs; [ gcc ] ++ hostTools;
          buildInputs = linuxBuildInputs;
          doCheck = false;
          cmakeFlags = [ "-DBUILD_TESTING=OFF" ];
          buildPhase = "cmake --build . --target buffon";
          installPhase = ''
            mkdir -p $out/bin $out/share/kiln/shaders
            cp demos/buffon/buffon $out/bin/
            cp src/render/shaders/*.spv $out/share/kiln/shaders/
          '';
        };

        # ── Win32 cross-compiled package ─────────────────────────────────────
        packages.win32 = mingw.stdenv.mkDerivation {
          pname = "kiln-win32";
          version = "0.1.0";
          src = ./.;
          nativeBuildInputs = hostTools ++ [ pkgs.gcc ];
          buildInputs = with mingw; [
            vulkan-headers
            vulkan-loader
            windows.pthreads
            windows.mcfgthreads
            stb
          ];
          cmakeFlags = [ "-DBUILD_TESTING=OFF" ];
          postInstall = ''
            cp ${mingw.windows.mcfgthreads}/bin/libmcfgthread-2.dll $out/bin/
          '';
          doCheck = false;
        };

        # ── Dev shell ────────────────────────────────────────────────────────
        devShells.default = pkgs.mkShell {
          nativeBuildInputs = with pkgs; [ gcc ] ++ hostTools;
          buildInputs = linuxBuildInputs ++ checkInputs;
          packages = with pkgs; [
            gdb
            vulkan-validation-layers
            spirv-tools
          ];
          shellHook = ''
            export VK_LAYER_PATH="${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d"
            ${pkgs.bash}/bin/bash scripts/gen_clangd.sh
            echo "Kiln dev shell"
          '';
        };
      });
}
