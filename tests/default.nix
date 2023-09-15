{
  pkgs ? import (fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/refs/tags/22.11.tar.gz";
    sha256 = "sha256:11w3wn2yjhaa5pv20gbfbirvjq6i3m7pqrq2msf0g7cv44vijwgw";
  }) {},
  platform ? " ",
  list_tests ? " ",
  list_suites ? " "
}:

with pkgs;

let
  packages = rec {
    # platform = "qemu-aarch64-virt";
    aarch64-none-elf = callPackage ./bao-nix/pkgs/toolchains/aarch64-none-elf-11-3.nix{};
    demos = callPackage ./bao-nix/pkgs/demos/demos.nix {};
    bao-tests = callPackage ./bao-nix/pkgs/bao-tests/bao-tests.nix {};
    tests = callPackage ./bao-nix/pkgs/tests/tests.nix {};
    baremetal = callPackage ./bao-nix/pkgs/guest/baremetal-bao-tf.nix 
                {
                  toolchain = aarch64-none-elf; 
                  inherit platform;
                  inherit list_tests; 
                  inherit list_suites;
                  inherit bao-tests;
                  inherit tests;
                };

    bao = callPackage ./bao-nix/pkgs/bao/bao_tf.nix 
                { 
                  toolchain = aarch64-none-elf; 
                  guest = baremetal; 
                  inherit demos; 
                  inherit platform;
                };

    u-boot = callPackage ./bao-nix/pkgs/u-boot/u-boot.nix 
                { 
                  toolchain = aarch64-none-elf; 
                };

    atf = callPackage ./bao-nix/pkgs/atf/atf.nix 
                { 
                  toolchain = aarch64-none-elf; 
                  inherit u-boot; 
                  inherit platform;
                };

    inherit pkgs;
  };
in
  packages


