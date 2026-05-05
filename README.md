# Mini Virtual File System (miniVSFS)

A custom filesystem implemented from scratch in C, built on top of raw binary disk images. Implements superblock, inode table, bitmap allocation, directory entries, and CRC32 checksums — following a real-world filesystem design.

## What It Does

- Creates formatted disk images (`mkfs_builder`) with a fully structured layout: superblock → inode bitmap → data bitmap → inode table → data region
- Adds files into an existing disk image (`mkfs_adder`) by writing to inodes and data blocks
- Lists the contents of the root directory from a disk image (`ls_minivsfs`)

## Filesystem Layout

```
Block 0       — Superblock (magic, version, geometry, CRC32 checksum)
Block 1       — Inode bitmap
Block 2       — Data block bitmap
Block 3..N    — Inode table (128 bytes per inode, packed)
Block N+1..end — Data region (4096-byte blocks)
```

## On-Disk Structures

| Structure | Size | Key Fields |
|-----------|------|------------|
| `superblock_t` | 116 bytes | magic `0x4D565346`, block size, inode count, region offsets, CRC32 |
| `inode_t` | 128 bytes | mode, size, timestamps, 12 direct block pointers, CRC32 |
| `dirent64_t` | 64 bytes | inode number, type, 58-byte name, XOR checksum |

## Tech Stack

- **Language** — C (C17 standard)
- **Build** — GCC with `-Wall -Wextra`
- **Platform** — Linux
- **Concepts** — Filesystem design, low-level I/O, bitmap allocation, CRC32 integrity checking

## Build & Run

```bash
# Build all three tools
gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c    -o mkfs_adder
gcc -O2 -std=c17 -Wall -Wextra ls_minivsfs.c   -o ls_minivsfs

# Create a 4MB disk image with 256 inodes
./mkfs_builder --image out.img --size-kib 4096 --inodes 256

# Add a file into the image
./mkfs_adder --image out.img --file file_9.txt

# List root directory contents
./ls_minivsfs out.img
```

## Test Files

The repository includes four sample text files (`file_9.txt`, `file_13.txt`, `file_20.txt`, `file_34.txt`) used to test file insertion and directory listing.

## Course

Operating Systems, BRACU
