# exFAT test volume

`exfat_test.vhd` is a real, Windows-formatted exFAT volume used to test
`kernel/exfat.inc` against Microsoft's actual on-disk format (not a
hand-rolled one). It's git-ignored (64MB binary) -- regenerate it with:

```
# 1. Create + format (needs an elevated PowerShell -- diskpart requires
#    admin and cannot be approved non-interactively):
diskpart /s testdata\make_exfat_vhd.txt

# 2. Write the test file (drive letter from step 1, usually Z: unless
#    already taken -- check with Get-Volume):
powershell -File testdata\write_testfile.ps1 -DriveLetter Z

# 3. Detach so QEMU can open the file exclusively:
diskpart /s testdata\detach_exfat_vhd.txt
```

`scripts/run-qemu.sh` attaches it as the NVMe-backed disk
(`-device nvme,drive=nvmedisk`), which `storage_read_sectors` prefers
over AHCI when both are present.

## Known quirks worth remembering

- **LBA 0 is an MBR, not the exFAT VBR.** `diskpart`'s `create partition
  primary` creates an actual partitioned disk; the real VBR lives at the
  first partition's starting LBA. `exfat_mount` handles this by falling
  back to MBR parsing when the exFAT signature isn't at LBA 0.
- **Windows auto-creates `System Volume Information`** the first time a
  volume is mounted/browsed. It'll show up as the first (sometimes only)
  entry in the root directory -- don't assume your own test file is entry
  #0.
- **Detaching without a flush silently loses writes.** A plain
  `[IO.File]::WriteAllText` + `diskpart detach vdisk` can leave the write
  sitting in Windows' cache, never reaching the VHD backing file.
  `write_testfile.ps1` explicitly flushes the stream and then calls
  `Write-VolumeCache` before you detach.
- **Freshly-written, unfragmented files use NoFatChain.** Their FAT
  entries are genuinely left at 0 (not an error) -- the Stream Extension
  entry's `GeneralSecondaryFlags` bit 1 says so, and the reader must walk
  `cluster, cluster+1, cluster+2, ...` directly instead of consulting the
  FAT in that case.
