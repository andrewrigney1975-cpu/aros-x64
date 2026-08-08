#!/usr/bin/env python3
"""Builds a real bootable disk image for arOS-X64: a GPT-partitioned disk
with a single FAT32 EFI System Partition containing EFI/BOOT/BOOTX64.EFI
and AROS/KERNEL.BIN, copied straight from disk/ (populated by build.sh).

This exists because scripts/run-qemu.sh boots disk/ via QEMU's own
virtual-FAT passthrough (-drive file=fat:rw:...), which is a QEMU
convenience with no real partition table or filesystem behind it --
nothing a real machine's firmware, or a USB stick, can boot from. This
builds the real thing, as a single raw .img: bootable directly in QEMU
(-drive file=...,format=raw) for validation, or `dd`/Rufus'd onto a
physical USB stick for a real-hardware boot attempt.

Pure Python stdlib, no external tools -- GPT partitioning tools
(diskpart, sgdisk, parted) all require administrator/root privileges to
manipulate disk/volume objects even when the target is just a file, since
they go through the OS's disk-management APIs. Building the image as
plain file I/O sidesteps that entirely: GPT and FAT32 are both just byte
layouts on disk, spelled out below directly against their specs.

Usage: python scripts/make_image.py [--size-mb 300]
"""

import argparse
import os
import struct
import sys
import uuid
import zlib

SECTOR = 512

# GPT layout (LBA units)
MBR_LBA = 0
GPT_HEADER_LBA = 1
GPT_ARRAY_LBA = 2
GPT_ARRAY_SECTORS = 32          # 128 entries * 128 bytes / 512
FIRST_USABLE_LBA = GPT_ARRAY_LBA + GPT_ARRAY_SECTORS   # 34
ESP_START_LBA = 2048            # 1 MiB alignment, standard convention

ESP_TYPE_GUID = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")

# FAT32 layout (sectors, within the ESP)
FAT_RESERVED_SECTORS = 32
FAT_NUM_FATS = 2
FAT_SEC_PER_CLUS = 8            # 4096-byte clusters
FAT_BYTES_PER_CLUS = SECTOR * FAT_SEC_PER_CLUS


def guid_bytes(u: uuid.UUID) -> bytes:
    """GPT/UEFI mixed-endian GUID encoding (RFC 4122 fields 1-3 little-
    endian, fields 4-5 big-endian, i.e. left as the UUID's own byte
    order for those two)."""
    b = u.bytes
    return bytes([b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6]]) + b[8:]


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def fat32_cluster_count(total_sectors: int):
    """Iterates the FAT-size-depends-on-cluster-count-depends-on-FAT-size
    circularity to a fixed point, same as real mkfs.fat does."""
    fat_size = 1
    for _ in range(32):
        data_sectors = total_sectors - FAT_RESERVED_SECTORS - FAT_NUM_FATS * fat_size
        clusters = data_sectors // FAT_SEC_PER_CLUS
        new_fat_size = (clusters * 4 + SECTOR - 1) // SECTOR
        if new_fat_size == fat_size:
            return fat_size, clusters
        fat_size = new_fat_size
    raise RuntimeError("FAT32 size computation did not converge")


def build_fat32(total_sectors: int, entries):
    """entries: list of (path_components, data_or_None) -- data_or_None
    None means "directory". Returns the ESP's raw byte image."""
    fat_size, total_clusters = fat32_cluster_count(total_sectors)
    if total_clusters < 65525:
        raise RuntimeError(
            f"only {total_clusters} clusters -- below FAT32's spec-mandated "
            "65525 minimum (would be misidentified as FAT16); raise --size-mb")

    img = bytearray(total_sectors * SECTOR)

    def sec(n):
        return n * SECTOR

    fat1_lba = FAT_RESERVED_SECTORS
    fat2_lba = fat1_lba + fat_size
    data_lba = fat2_lba + fat_size   # cluster 2 starts here

    def cluster_lba(cluster):
        return data_lba + (cluster - 2) * FAT_SEC_PER_CLUS

    # --- FAT32 Boot Sector / BPB ---
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x58\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = FAT_SEC_PER_CLUS
    struct.pack_into("<H", bs, 14, FAT_RESERVED_SECTORS)
    bs[16] = FAT_NUM_FATS
    struct.pack_into("<H", bs, 17, 0)          # RootEntCnt (FAT32: 0)
    struct.pack_into("<H", bs, 19, 0)          # TotSec16 (use TotSec32)
    bs[21] = 0xF8                              # Media
    struct.pack_into("<H", bs, 22, 0)          # FATSz16 (use FATSz32)
    struct.pack_into("<H", bs, 24, 32)         # SecPerTrk (unused, CHS legacy)
    struct.pack_into("<H", bs, 26, 64)         # NumHeads (unused, CHS legacy)
    struct.pack_into("<I", bs, 28, ESP_START_LBA)   # HiddSec
    struct.pack_into("<I", bs, 32, total_sectors)   # TotSec32
    struct.pack_into("<I", bs, 36, fat_size)   # FATSz32
    struct.pack_into("<H", bs, 40, 0)          # ExtFlags
    struct.pack_into("<H", bs, 42, 0)          # FSVer
    struct.pack_into("<I", bs, 44, 2)          # RootClus
    struct.pack_into("<H", bs, 48, 1)          # FSInfo sector
    struct.pack_into("<H", bs, 50, 6)          # BkBootSec
    bs[64] = 0x80                              # DrvNum
    bs[66] = 0x29                              # BootSig
    struct.pack_into("<I", bs, 67, 0x41524F53)  # VolID ("AROS" as hex-ish)
    bs[71:82] = b"AROS       "[:11]
    bs[82:90] = b"FAT32   "
    bs[510] = 0x55
    bs[511] = 0xAA
    img[sec(0):sec(0) + SECTOR] = bs
    img[sec(6):sec(6) + SECTOR] = bs           # backup boot sector

    # --- FSInfo sector ---
    fsi = bytearray(SECTOR)
    struct.pack_into("<I", fsi, 0, 0x41615252)
    struct.pack_into("<I", fsi, 484, 0x61417272)
    struct.pack_into("<I", fsi, 488, 0xFFFFFFFF)   # free count: unknown/unused
    struct.pack_into("<I", fsi, 492, 0xFFFFFFFF)   # next free: unknown/unused
    struct.pack_into("<I", fsi, 508, 0xAA550000)
    img[sec(1):sec(1) + SECTOR] = fsi
    img[sec(7):sec(7) + SECTOR] = fsi          # backup FSInfo

    # --- Allocate clusters to entries, building FAT chains + dir data ---
    fat = [0] * total_clusters   # index by cluster number (0,1 unused-ish)
    fat[0] = 0x0FFFFFF8
    fat[1] = 0x0FFFFFFF
    next_free = 2

    def alloc_chain(n_clusters):
        nonlocal next_free
        chain = list(range(next_free, next_free + n_clusters))
        next_free += n_clusters
        for i, c in enumerate(chain):
            fat[c] = chain[i + 1] if i + 1 < len(chain) else 0x0FFFFFFF
        return chain

    def write_cluster(cluster, data: bytes):
        off = sec(cluster_lba(cluster))
        img[off:off + len(data)] = data

    def dirent(name83: str, attr: int, cluster: int, size: int) -> bytes:
        e = bytearray(32)
        e[0:11] = name83.encode("ascii")
        e[11] = attr
        # Fixed timestamp (2024-01-01 00:00:00) -- content, not clock, is
        # what matters for a boot medium.
        date = (44 << 9) | (1 << 5) | 1        # year-1980, month, day
        struct.pack_into("<H", e, 16, 0)       # CrtTime
        struct.pack_into("<H", e, 18, date)    # CrtDate
        struct.pack_into("<H", e, 20, cluster >> 16)  # FstClusHI
        struct.pack_into("<H", e, 22, 0)       # WrtTime
        struct.pack_into("<H", e, 24, date)    # WrtDate
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)  # FstClusLO
        struct.pack_into("<I", e, 28, size)    # FileSize
        return bytes(e)

    def short_name(name: str) -> str:
        if "." in name:
            base, ext = name.rsplit(".", 1)
        else:
            base, ext = name, ""
        if len(base) > 8 or len(ext) > 3:
            raise ValueError(f"{name!r} is not a valid 8.3 name")
        return base.upper().ljust(8) + ext.upper().ljust(3)

    # Reserve root dir's cluster first so its cluster number is 2.
    root_cluster = alloc_chain(1)[0]

    # Build a tree: {name: (attr_is_dir, cluster_or_data)}
    tree = {}
    for path_parts, data in entries:
        node = tree
        for part in path_parts[:-1]:
            node = node.setdefault(part, {})
        node[path_parts[-1]] = data

    def write_dir(node: dict, this_cluster: int, parent_cluster: int, is_root: bool):
        raw = bytearray()
        if not is_root:
            raw += dirent(short_name("."), 0x10, this_cluster, 0)
            raw += dirent(short_name(".."), 0x10, 0 if parent_cluster == root_cluster else parent_cluster, 0)
        for name, value in node.items():
            if isinstance(value, dict):
                child_cluster = alloc_chain(1)[0]
                raw += dirent(short_name(name), 0x10, child_cluster, 0)
                write_dir(value, child_cluster, this_cluster, False)
            else:
                n_clus = max(1, (len(value) + FAT_BYTES_PER_CLUS - 1) // FAT_BYTES_PER_CLUS)
                chain = alloc_chain(n_clus)
                for i, c in enumerate(chain):
                    write_cluster(c, value[i * FAT_BYTES_PER_CLUS:(i + 1) * FAT_BYTES_PER_CLUS])
                raw += dirent(short_name(name), 0x20, chain[0], len(value))
        # Pad to a whole number of clusters (dir data is cluster-chained too).
        n_dir_clus = max(1, (len(raw) + FAT_BYTES_PER_CLUS - 1) // FAT_BYTES_PER_CLUS)
        raw += bytes(n_dir_clus * FAT_BYTES_PER_CLUS - len(raw))
        # this_cluster was already reserved by the caller; extend the chain
        # if the directory grew past one cluster (won't happen here, but
        # keep it correct rather than assume).
        clusters = [this_cluster]
        if n_dir_clus > 1:
            clusters += alloc_chain(n_dir_clus - 1)
            for i in range(len(clusters) - 1):
                fat[clusters[i]] = clusters[i + 1]
            fat[clusters[-1]] = 0x0FFFFFFF
        for i, c in enumerate(clusters):
            write_cluster(c, bytes(raw[i * FAT_BYTES_PER_CLUS:(i + 1) * FAT_BYTES_PER_CLUS]))

    write_dir(tree, root_cluster, root_cluster, True)

    # --- Write both FATs ---
    fat_bytes = bytearray(fat_size * SECTOR)
    for i, val in enumerate(fat):
        struct.pack_into("<I", fat_bytes, i * 4, val & 0x0FFFFFFF)
    img[sec(fat1_lba):sec(fat1_lba) + len(fat_bytes)] = fat_bytes
    img[sec(fat2_lba):sec(fat2_lba) + len(fat_bytes)] = fat_bytes

    return bytes(img)


def build_gpt_image(esp_image: bytes, esp_sectors: int) -> bytes:
    backup_array_lba = ESP_START_LBA + esp_sectors
    backup_header_lba = backup_array_lba + GPT_ARRAY_SECTORS
    total_sectors = backup_header_lba + 1
    last_usable_lba = ESP_START_LBA + esp_sectors - 1

    disk_guid = uuid.uuid4()
    part_guid = uuid.uuid4()

    # Partition entry array (both copies identical).
    entry = bytearray(128)
    entry[0:16] = guid_bytes(ESP_TYPE_GUID)
    entry[16:32] = guid_bytes(part_guid)
    struct.pack_into("<Q", entry, 32, ESP_START_LBA)
    struct.pack_into("<Q", entry, 40, ESP_START_LBA + esp_sectors - 1)
    struct.pack_into("<Q", entry, 48, 0)       # Attributes
    entry[56:56 + len("AROS ESP".encode("utf-16-le"))] = "AROS ESP".encode("utf-16-le")
    array = bytearray(GPT_ARRAY_SECTORS * SECTOR)
    array[0:128] = entry
    array_crc = crc32(bytes(array))

    def gpt_header(my_lba, alt_lba, part_array_lba):
        h = bytearray(SECTOR)
        h[0:8] = b"EFI PART"
        struct.pack_into("<I", h, 8, 0x00010000)   # Revision
        struct.pack_into("<I", h, 12, 92)          # HeaderSize
        struct.pack_into("<I", h, 16, 0)           # HeaderCRC32 (filled below)
        struct.pack_into("<I", h, 20, 0)           # Reserved
        struct.pack_into("<Q", h, 24, my_lba)
        struct.pack_into("<Q", h, 32, alt_lba)
        struct.pack_into("<Q", h, 40, FIRST_USABLE_LBA)
        struct.pack_into("<Q", h, 48, last_usable_lba)
        h[56:72] = guid_bytes(disk_guid)
        struct.pack_into("<Q", h, 72, part_array_lba)
        struct.pack_into("<I", h, 80, 128)         # NumberOfPartitionEntries
        struct.pack_into("<I", h, 84, 128)         # SizeOfPartitionEntry
        struct.pack_into("<I", h, 88, array_crc)
        struct.pack_into("<I", h, 16, crc32(bytes(h[0:92])))
        return bytes(h)

    primary_header = gpt_header(GPT_HEADER_LBA, backup_header_lba, GPT_ARRAY_LBA)
    backup_header = gpt_header(backup_header_lba, GPT_HEADER_LBA, backup_array_lba)

    # Protective MBR.
    mbr = bytearray(SECTOR)
    disk_sectors_field = min(total_sectors - 1, 0xFFFFFFFF)
    struct.pack_into("<B", mbr, 446 + 0, 0x00)          # status
    mbr[446 + 1:446 + 4] = b"\x00\x02\x00"              # CHS start (dummy)
    mbr[446 + 4] = 0xEE                                 # type: GPT protective
    mbr[446 + 5:446 + 8] = b"\xFF\xFF\xFF"               # CHS end (dummy)
    struct.pack_into("<I", mbr, 446 + 8, 1)             # starting LBA
    struct.pack_into("<I", mbr, 446 + 12, disk_sectors_field)
    mbr[510] = 0x55
    mbr[511] = 0xAA

    img = bytearray(total_sectors * SECTOR)
    img[0:SECTOR] = mbr
    img[SECTOR:SECTOR * 2] = primary_header
    img[SECTOR * 2:SECTOR * 2 + len(array)] = array
    img[ESP_START_LBA * SECTOR:ESP_START_LBA * SECTOR + len(esp_image)] = esp_image
    img[backup_array_lba * SECTOR:backup_array_lba * SECTOR + len(array)] = array
    img[backup_header_lba * SECTOR:backup_header_lba * SECTOR + SECTOR] = backup_header
    return bytes(img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size-mb", type=int, default=300,
                     help="ESP size in MiB (default 300 -- comfortably "
                          "clears FAT32's 65525-cluster minimum)")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    disk = os.path.join(root, "disk")
    build_dir = os.path.join(root, "build")
    kernel_path = os.path.join(disk, "AROS", "KERNEL.BIN")
    boot_path = os.path.join(disk, "EFI", "BOOT", "BOOTX64.EFI")
    startup_path = os.path.join(disk, "startup.nsh")

    for p in (kernel_path, boot_path):
        if not os.path.isfile(p):
            sys.exit(f"{p} not found -- run scripts/build.sh first.")

    with open(boot_path, "rb") as f:
        boot_data = f.read()
    with open(kernel_path, "rb") as f:
        kernel_data = f.read()

    entries = [
        (["EFI", "BOOT", "BOOTX64.EFI"], boot_data),
        (["AROS", "KERNEL.BIN"], kernel_data),
    ]
    if os.path.isfile(startup_path):
        with open(startup_path, "rb") as f:
            entries.append((["STARTUP.NSH"], f.read()))

    esp_sectors = (args.size_mb * 1024 * 1024) // SECTOR
    print(f"Building FAT32 ESP: {args.size_mb} MiB ({esp_sectors} sectors)...")
    esp_image = build_fat32(esp_sectors, entries)

    print("Wrapping in a GPT partition table...")
    disk_image = build_gpt_image(esp_image, esp_sectors)

    os.makedirs(build_dir, exist_ok=True)
    out_path = os.path.join(build_dir, "aros.img")
    with open(out_path, "wb") as f:
        f.write(disk_image)

    print(f"Done: {out_path} ({len(disk_image) / (1024*1024):.1f} MiB)")
    print("  Test in QEMU:  qemu-system-x86_64 ... -drive file=build/aros.img,format=raw,if=virtio")
    print("  Write to USB:  dd if=build/aros.img of=/dev/sdX bs=4M status=progress   (or Rufus 'DD mode' on Windows)")


if __name__ == "__main__":
    main()
