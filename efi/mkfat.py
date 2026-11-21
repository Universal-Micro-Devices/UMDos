#!/usr/bin/env python3
"""生成 FAT12 软盘镜像，用于 UEFI 启动项（P13）。

用法: python efi/mkfat.py <bootx64.efi> <out.img> [size_kb]
镜像内固定路径：/EFI/BOOT/BOOTX64.EFI
"""
import os
import struct
import sys

SEC = 512


def build(efi_path, size_kb=1440):
    efi = open(efi_path, "rb").read()
    total_sectors = size_kb * 1024 // SEC
    sectors_per_cluster = 1
    reserved = 1
    fats = 2
    root_entries = 224
    root_sectors = (root_entries * 32 + SEC - 1) // SEC
    # FAT12：先占位，之后按实际需要回填
    fat_sectors = 9
    data_start = reserved + fats * fat_sectors + root_sectors
    clusters = total_sectors - data_start
    assert clusters < 4085, clusters

    img = bytearray(size_kb * 1024)
    # BPB
    struct.pack_into("<3s8sHBHBHHBHHHII", img, 0, b"\xEB\x3C\x90", b"UMDOS13 ", SEC,
                     sectors_per_cluster, reserved, fats, root_entries,
                     total_sectors if total_sectors < 65536 else 0, 0xF8,
                     fat_sectors, 1, 1, 0, total_sectors)
    struct.pack_into("<BBBI11s8s", img, 36, 0x00, 0x00, 0x29, 0x12345678,
                     b"UMDOS FAT12", b"FAT12   ")

    fat = bytearray(fat_sectors * SEC)
    fat[0] = 0xF8
    fat[1] = 0xFF
    fat[2] = 0xFF
    next_cluster = [2]

    def alloc(n_clusters):
        start = next_cluster[0]
        for i in range(n_clusters):
            c = start + i
            nxt = c + 1 if i + 1 < n_clusters else 0xFFF
            if nxt == 0xFFF:
                set_fat(c, 0xFFF)
            else:
                set_fat(c, nxt)
        next_cluster[0] = start + n_clusters
        return start

    def set_fat(c, value):
        off = c * 3 // 2
        v = fat[off] | (fat[off + 1] << 8)
        if c & 1:
            v = (v & 0x000F) | (value << 4)
        else:
            v = (v & 0xF000) | (value & 0x0FFF)
        fat[off] = v & 0xFF
        fat[off + 1] = (v >> 8) & 0xFF

    def dir_entry(name83, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        struct.pack_into("<H", e, 26, cluster)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    # 目录树：根 -> EFI -> BOOT -> BOOTX64.EFI
    root = bytearray(root_sectors * SEC)
    root_off = 0
    efi_dir_cluster = alloc(1)
    root[root_off:root_off + 32] = dir_entry(b"EFI        ", 0x10, efi_dir_cluster, 0)
    root_off += 32
    root[root_off:root_off + 32] = dir_entry(b"UMDOS      ", 0x08, 0, 0)  # volume label
    root_off += 32

    efi_dir = bytearray(SEC)
    boot_dir_cluster = alloc(1)
    efi_dir[0:32] = dir_entry(b"BOOT       ", 0x10, boot_dir_cluster, 0)

    boot_dir = bytearray(SEC)
    n_clusters = (len(efi) + SEC - 1) // SEC
    efi_cluster = alloc(n_clusters)
    boot_dir[0:32] = dir_entry(b"BOOTX64 EFI", 0x20, efi_cluster, len(efi))

    def cluster_off(c):
        return (data_start + c - 2) * SEC

    img[cluster_off(efi_dir_cluster):cluster_off(efi_dir_cluster) + SEC] = efi_dir
    img[cluster_off(boot_dir_cluster):cluster_off(boot_dir_cluster) + SEC] = boot_dir
    off = cluster_off(efi_cluster)
    img[off:off + len(efi)] = efi

    for i in range(fats):
        base = (reserved + i * fat_sectors) * SEC
        img[base:base + len(fat)] = fat
    root_base = (reserved + fats * fat_sectors) * SEC
    img[root_base:root_base + len(root)] = root
    return bytes(img), len(efi)


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    size_kb = int(sys.argv[3]) if len(sys.argv) > 3 else 1440
    data, n = build(src, size_kb)
    open(dst, "wb").write(data)
    print("  %s: %d bytes (FAT12, BOOTX64.EFI %d bytes)" % (dst, len(data), n))
