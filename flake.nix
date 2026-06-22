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
        mingw = pkgs.pkgsCross.mingwW64;

        # Host tools used by both Linux and Win32 builds.
        hostTools = with pkgs; [
          cmake
          ninja
          pkg-config
          shaderc   # provides glslc; shader compilation always runs on host
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

        # ── Win32 cross-compiled package (host: Linux, target: x86_64-windows)
        #    Requires pkgs.pkgsCross.mingwW64 to be available.
        #    The Vulkan loader (vulkan-1.dll) is linked at build time via the
        #    mingw import lib; at runtime it must be present on the Windows
        #    machine (installed with any Vulkan-capable GPU driver).
        # ────────────────────────────────────────────────────────────────────
        packages.win32 = mingw.stdenv.mkDerivation {
          pname = "kiln-win32";
          version = "0.1.0";
          src = ./.;

          # Build-host tools: cmake/ninja/glslc run on Linux during the build.
          nativeBuildInputs = hostTools ++ [ pkgs.gcc ];

          # Target libraries: headers + import libs for the Windows binary.
          buildInputs = with mingw; [
            vulkan-headers
            vulkan-loader
            windows.pthreads
            windows.mcfgthreads   # provides libmcfgthread.a for static linking
            stb
          ];

          cmakeFlags = [
            "-DBUILD_TESTING=OFF"   # criterion not available for mingw
          ];

          # nixpkgs mingw uses the MCF GCC thread model.  libmcfgthread-2.dll
          # is not a system DLL — bundle it next to kiln.exe so the binary
          # runs on a stock Windows machine without any extra installs.
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
      }
    );
}
