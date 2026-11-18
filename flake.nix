{
  description = "planner-cli: headless Subsurface dive planner (reference deco planner without GUI)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Subsurface's fork of libdivecomputer, pinned to the submodule commit.
        libdc = pkgs.stdenv.mkDerivation {
          pname = "libdivecomputer-subsurface";
          version = "0.9.0-6f19abf8";
          src = pkgs.fetchFromGitHub {
            owner = "Subsurface";
            repo = "libdc";
            rev = "6f19abf8d260dfb85257e33c08411dd08f9f4dc0";
            hash = "sha256-j5JiUgNuoDznabW5Hz36R6TLUbq9qjKVjVXvQL6ewAE=";
          };
          nativeBuildInputs = with pkgs; [ autoreconfHook pkg-config ];
          buildInputs = with pkgs; [ libusb1 bluez ];
          configureFlags = [ "--disable-examples" ];
          enableParallelBuilding = true;
        };

        planner-cli = pkgs.stdenv.mkDerivation {
          pname = "subsurface-planner-cli";
          version = "6.0-planner-cli";

          src = pkgs.lib.cleanSourceWith {
            src = ./.;
            filter = path: type:
              !(pkgs.lib.hasPrefix "build" (baseNameOf path))
              && baseNameOf path != "result";
          };

          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
            libsForQt5.wrapQtAppsHook
          ];

          buildInputs = with pkgs; [
            libdc
            libsForQt5.qtbase
            libsForQt5.qtconnectivity
            libsForQt5.qtlocation
            libsForQt5.qtsvg
            libsForQt5.qttools
            curl
            libgit2
            libssh2
            libxml2
            libxslt
            libzip
            libusb1
            libmtp
            sqlite
            zlib
            bluez
          ];

          cmakeFlags = [
            "-DSUBSURFACE_TARGET_EXECUTABLE=DownloaderExecutable"
            "-DBUILD_PLANNER_CLI=ON"
            "-DLIBDC_FROM_PKGCONFIG=ON"
            "-DLIBGIT2_FROM_PKGCONFIG=ON"
            "-DLIBGIT2_DYNAMIC=ON"
            "-DNO_USERMANUAL=ON"
            "-DNO_PRINTING=ON"
            "-DFTDISUPPORT=OFF"
            "-DCMAKE_BUILD_TYPE=Release"
          ];

          # get-version.sh needs git history + network; provide the version instead.
          CANONICALVERSION = "6.0.0-planner-cli";
          CANONICALVERSION_4 = "6.0.0.0";

          enableParallelBuilding = true;

          buildPhase = ''
            runHook preBuild
            make -j$NIX_BUILD_CORES planner-cli
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            install -Dm755 planner-cli/planner-cli $out/bin/planner-cli
            mkdir -p $out/share/planner-cli
            cp -r $src/planner-cli/examples $src/planner-cli/README.md $out/share/planner-cli/
            runHook postInstall
          '';

          meta = {
            description = "Headless Subsurface dive planner";
            license = pkgs.lib.licenses.gpl2Only;
            mainProgram = "planner-cli";
            platforms = pkgs.lib.platforms.linux;
          };
        };
      in
      {
        packages = {
          inherit planner-cli libdc;
          default = planner-cli;
        };

        apps.default = flake-utils.lib.mkApp { drv = planner-cli; name = "planner-cli"; };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ planner-cli ];
          packages = with pkgs; [ autoreconfHook gdb ];
        };
      });
}
