# UMDos
UMDos: An independent from-scratch OS by Universal Micro Devices. Built with pure C &amp; NASM assembly to reject bloated black boxes and directly command hardware. Features a dual-track boot chain (Legacy BIOS &amp; UEFI). GPL licensed—we believe the foundation of computing must remain absolutely free and open.

## Source layout

- `kernel/` contains the kernel sources, with C headers in `include/kernel/`.
- `installer/src/` contains the installer sources, with C headers in `include/installer/`.
- `loader/` contains the BIOS boot stages, with NASM includes in `loader/inc/`.
  The installer uses `loader/firstmain.asm` and `loader/nextmain.asm` after its own boot sector.

Use the matching C header directory for each target. The kernel and installer have
separate headers for interfaces such as `ata.h` and `input.h`.

## Build the BIOS images

Put NASM on your PATH and install the Python build dependencies.

```sh
python -m pip install ziglang pycdlib
python build.py
```

The script writes the main system's floppy, hard disk and El Torito ISO images
to `build/`. An EFI boot image is included when the EFI loader sources are
available under `efi/`. The installer has its own boot chain.
