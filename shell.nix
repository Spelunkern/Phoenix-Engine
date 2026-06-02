{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  packages = with pkgs; [
    gcc
    cmake
    pkg-config
    SDL2
    vulkan-loader
    vulkan-tools
  ];

  shellHook = ''
    echo "Phoenix Engine dev shell"
    echo "Build: ./scripts/build.sh"
    echo "Run:   ./scripts/run.sh"
  '';
}
