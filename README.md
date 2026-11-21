# UMDos

**An independent, from-scratch operating system by
[Universal Micro Devices](https://github.com/Universal-Micro-Devices).**

Built with pure C and NASM assembly — no Linux, no GRUB, no borrowed kernel.
UMDos rejects bloated black boxes and drives the hardware directly: our own
boot chain, our own kernel, our own VFS, our own shell, our own UI.

> "We believe the foundation of computing must remain absolutely free and open."
> Licensed under **GPL-2.0** — see [LICENSE](LICENSE).

---

## Current version

| Item | Value |
|---|---|
| Version | `v2.6.112-dev.15` (development snapshot) |
| Baseline | `v2.6.112-dev.14` |
| Focus | P18 — AHCI port-reset / retry error recovery, multi-disk self-test |
| Boot chain | Legacy BIOS (4-stage) **and** UEFI (dual-track) |
| Arch | x86, 32-bit protected mode to 64-bit long-mode kernel |

## Highlights (P18)

- **AHCI error recovery** — a failed `disk_cmd` triggers `port_reset()`
  (COMRESET plus CLB/FB/ST/FRE re-init) followed by one retry; the caller
  still gets a clean error on persistent failure.
- **Out-of-range detection** — the emulator answers over-capacity reads/writes
  with `PxTFD.ERR`, so the driver is exercised through the reset/retry path.
- **Self-test** — out-of-range error path, write-read-compare on the last
  sector, and re-reading the boot area after recovery. Kernel log:
  `ahci: port reset (cmd error)` / `disk1 range check/write-read/recheck ok`.

## Feature set

- 4-stage legacy boot chain (`boot` -> `loader` -> `head` -> kernel) plus a
  UEFI loader in `efi/`
- Freestanding C kernel: GDT / IDT / IRQ, paging and heap, SMP (APIC +
  trampoline), per-CPU data
- Preemptive tasks, syscalls, user mode with its own libc (`user/`)
- VFS and a FAT-like filesystem generator (`fsgen.py`), persistence
- ATA PIO **and** AHCI drivers, PCI enumeration, ACPI tables
- SHA-256 / SHA-3 authentication, multi-user support
- Text console, graphics mode with bitmap fonts, classic-DOS-style UI + shell
- Full-screen text editor (find / replace / paste / indent) — see `shots/`

## Repository layout

```
.
├── build.py            # staged build -> floppy/hdd images -> El Torito ISO
├── fsgen.py            # generates the filesystem image used by the kernel
├── VERSION             # single source of truth for the version string
├── src/                # 16-bit stage assembly + 64-bit C kernel
│   ├── boot.asm  loader.asm  head.asm  firstmain.asm  nextmain.asm
│   ├── inc/            # 16-bit helpers: disk, io, vga, serial, sha256, fonts
│   └── kernel/         # kmain, gdt, idt, isr, irq, mem, smp, proc, task,
│                       # syscall, vfs, fs, dev, ata, ahci, pci, acpi, gfx,
│                       # term, sh, ui, auth, sec, sha256, sha3, user, libk
├── efi/                # UEFI loader (efi_loader.c, mkfat.py, bootx64.efi)
├── installer/          # standalone installer (its own mini-kernel + FAT32)
├── user/               # user-mode program and minimal libc
├── tools/              # emulator, image builders, and the test suite
├── docs/               # per-phase design notes (P3-P4 ... P18, roadmap)
├── assets/             # icons and logo
├── shots/              # screenshots of UI / editor features
└── img & iso/dev15/    # prebuilt floppy / hdd / ISO images for this version
```

## Build

Requirements: `nasm`, `python3`, `pycdlib`, and a freestanding-capable C compiler
(`pip install ziglang`, used as `python -m ziglang cc`).

```sh
python build.py                   # floppy + hdd + ISO
python tools/build_installer.py   # installer ISO
```

Outputs land in the repository root; the prebuilt copies live in
`img & iso/dev15/`.

## Run (QEMU)

```sh
# floppy, serial on stdout
qemu-system-x86_64 -m 512M \
  -drive format=raw,file="UMDos v2.6.112-dev.15 floppy.img",if=floppy \
  -serial stdio

# dual-disk AHCI
qemu-system-x86_64 -m 512M -device ahci,id=ahci \
  -drive id=d0,file="UMDos v2.6.112-dev.15 hdd.img",if=none,format=raw \
  -device ide-hd,drive=d0,bus=ahci.0 \
  -drive id=d1,file="UMDos v2.6.112-dev.15 floppy.img",if=none,format=raw \
  -device ide-hd,drive=d1,bus=ahci.1 -serial stdio
```

## Tests

```sh
python tools/test_all.py          # full suite
python tools/test_p15p16.py       # P15-P18 disk / AHCI assertions
python tools/verify_iso.py        # ISO boot-record checks
```

## Docs

Design notes for every phase are in [`docs/`](docs/): `p3-p4.md` ...
`p18.md`, plus `roadmap.md`, `boot-map.md`, `audit.md`, `branding.md`,
`classic-ui.md`. Chinese-language working notes are kept as-is.

## License

GPL-2.0. See [LICENSE](LICENSE).
