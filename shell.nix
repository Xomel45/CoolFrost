{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  # List the packages required for your development environment
  packages = with pkgs; [
    tinyxxd
    # qemu_full
	gnumake
	gdb
	coreboot-toolchain.x86_64
	grub2
    libisoburn
	# kvmtool
  ];

  # Define environment variables (optional)
  shellHook = ''
    echo "OSDev Shell"
    fish
    exit
  '';
}
