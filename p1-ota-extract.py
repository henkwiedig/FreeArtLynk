#!/usr/bin/env python3
"""
AR FPV OTA Firmware Extractor

Extracts partition images from AR FPV OTA firmware packages.
Based on reverse-engineered do_upgrade_ex() and verify_image_signature() functions.
"""

import struct
import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

# Try to import LZO - handle different package variants
try:
    import lzo
    HAS_LZO = True
except ImportError:
    HAS_LZO = False
    print("Warning: python-lzo not installed. Install with: pip install python-lzo")


def lzo_decompress(data: bytes, output_size: int) -> bytes:
    """Decompress LZO1X data"""
    if not HAS_LZO:
        raise ImportError("LZO library not available")
    
    # python-lzo package uses decompress_block for raw LZO1X data
    return lzo.decompress(data, False, output_size)


# Magic numbers
OTA_MAGIC = 0x4152544F  # "ARTO" - AR OTA magic

# Sizes
BASE_HEADER_SIZE = 0x100  # 256 bytes base header
HASH_SIG_SIZE = 0x120     # 32 bytes hash + 256 bytes RSA signature
PARTITION_ENTRY_SIZE = 0x34  # 52 bytes per partition
SEGMENT_ENTRY_SIZE = 0x20    # 32 bytes per segment


@dataclass
class OTAHeader:
    """OTA firmware header (0x100 bytes base)"""
    magic: int              # 0x00: Magic number (0x4152544F)
    hdr_version: int        # 0x04: Header version (byte)
    compressed: int         # 0x05: Compressed flag (byte)
    flash_type: int         # 0x06: Flash type (0=nor, 1=mmc, 2=nand)
    part_status: int        # 0x07: Partition status (byte)
    header_ext_size: int    # 0x08: Header extension size (2 bytes)
    hash_size: int          # 0x0A: Hash size (2 bytes, typically 0x20)
    sig_size: int           # 0x0C: Signature size (2 bytes, typically 0x100)
    sig_realsize: int       # 0x0E: Real signature size (2 bytes)
    img_size: int           # 0x10: Total image size (8 bytes)
    rom_size: int           # 0x18: ROM data size (4 bytes)
    loader_size: int        # 0x1C: Loader data size (4 bytes)
    num_partitions: int     # 0x20: Number of partitions (2 bytes)
    num_segments: int       # 0x22: Number of segments (2 bytes)
    obj_version: int        # 0x24: Object version (4 bytes)
    dep_version: int        # 0x28: Dependency version (4 bytes)
    customer_version: str   # 0x2C: Customer version string (52 bytes max)
    part_flag: bytes        # 0x40: Partition flag bytes (64 bytes)
    sdk_version: str        # 0x80: SDK version string

    @classmethod
    def from_bytes(cls, data: bytes) -> "OTAHeader":
        if len(data) < BASE_HEADER_SIZE:
            raise ValueError(f"Header data too short: {len(data)} < {BASE_HEADER_SIZE}")

        magic = struct.unpack("<I", data[0x00:0x04])[0]
        hdr_version = data[0x04]
        compressed = data[0x05]
        flash_type = data[0x06]
        part_status = data[0x07]
        header_ext_size = struct.unpack("<H", data[0x08:0x0A])[0]
        hash_size = struct.unpack("<H", data[0x0A:0x0C])[0]
        sig_size = struct.unpack("<H", data[0x0C:0x0E])[0]
        sig_realsize = struct.unpack("<H", data[0x0E:0x10])[0]
        img_size = struct.unpack("<Q", data[0x10:0x18])[0]
        rom_size = struct.unpack("<I", data[0x18:0x1C])[0]
        loader_size = struct.unpack("<I", data[0x1C:0x20])[0]
        num_partitions = struct.unpack("<H", data[0x20:0x22])[0]
        num_segments = struct.unpack("<H", data[0x22:0x24])[0]
        obj_version = struct.unpack("<I", data[0x24:0x28])[0]
        dep_version = struct.unpack("<I", data[0x28:0x2C])[0]
        
        # Customer version at offset 0x2C (null-terminated string, up to 52 bytes)
        customer_version_raw = data[0x2C:0x60]
        customer_version = customer_version_raw.split(b'\x00')[0].decode('utf-8', errors='replace')
        
        # Partition flag bytes at offset 0x40 (64 bytes)
        part_flag = data[0x40:0x80]
        
        # SDK version at offset 0x80 (null-terminated string)
        sdk_version_raw = data[0x80:0x100]
        sdk_version = sdk_version_raw.split(b'\x00')[0].decode('utf-8', errors='replace')

        return cls(
            magic=magic,
            hdr_version=hdr_version,
            compressed=compressed,
            flash_type=flash_type,
            part_status=part_status,
            header_ext_size=header_ext_size,
            hash_size=hash_size,
            sig_size=sig_size,
            sig_realsize=sig_realsize,
            img_size=img_size,
            rom_size=rom_size,
            loader_size=loader_size,
            num_partitions=num_partitions,
            num_segments=num_segments,
            obj_version=obj_version,
            dep_version=dep_version,
            customer_version=customer_version,
            part_flag=part_flag,
            sdk_version=sdk_version,
        )

    def print_info(self):
        """Print header information (mirrors verify_image_signature output)"""
        flash_types = {0: "nor", 1: "mmc", 2: "nand"}
        part_statuses = {0: "no change", 1: "changed"}
        
        print("\n====================upgrade bin infomation====================\n")
        print(f"magic:            0x{self.magic:x}")
        print(f"hdr_version:      0x{self.hdr_version:x}")
        print(f"compressed:       0x{self.compressed:x}")
        print(f"flashtype:        0x{self.flash_type:x} ({flash_types.get(self.flash_type, 'unknown')})")
        print(f"part_status:      0x{self.part_status:x}")
        print(f"header_ext size:  0x{self.header_ext_size:x}")
        print(f"hash size:        0x{self.hash_size:x}")
        print(f"sig size:         0x{self.sig_size:x}")
        print(f"sig_realsize:     0x{self.sig_realsize:x}")
        print(f"img size:         0x{self.img_size:x}")
        print(f"rom_size:         0x{self.rom_size:x}")
        print(f"loader_size:      0x{self.loader_size:x}")
        print(f"partitions:       0x{self.num_partitions:x}")
        print(f"segments:         0x{self.num_segments:x}")
        print(f"object_version:   0x{self.obj_version:x}")
        print(f"depend_version:   0x{self.dep_version:x}")
        print(f"customer_version: {self.customer_version}")
        print(f"part_flag:        ", end="")
        for i, byte in enumerate(self.part_flag):
            if i != 0 and i % 8 == 0:
                print("\n                  ", end="")
            print(f"0x{byte:02x} ", end="")
        print()
        print(f"sdk_version:      {self.sdk_version}")
        
        # Print size validation (from disassembly)
        print(f"\nSize Validation:")
        rom_valid = self.rom_size == 0 or self.rom_size < 0x10001
        loader_valid = (self.loader_size == 0 or self.loader_size < 0x40001 or 
                       self.flash_type not in [0, 1])  # nor(0) or mmc(1)
        print(f"  ROM size valid:    {rom_valid} (must be 0 or < 0x10001)")
        print(f"  Loader size valid: {loader_valid} (must be 0 or < 0x40001 for nor/mmc)")


@dataclass
class PartitionEntry:
    """Partition table entry (0x34 = 52 bytes)"""
    name: str               # 0x00: Partition name (null-terminated string)
    flash_offset: int       # 0x20: Flash offset (8 bytes)
    size: int               # 0x28: Partition size (8 bytes)
    flags: int              # 0x30: Flags (4 bytes)

    @classmethod
    def from_bytes(cls, data: bytes) -> "PartitionEntry":
        if len(data) < PARTITION_ENTRY_SIZE:
            raise ValueError(f"Partition entry too short: {len(data)}")

        # Name is at the beginning, null-terminated (up to 0x20 bytes)
        name_raw = data[0x00:0x20]
        name = name_raw.split(b'\x00')[0].decode('utf-8', errors='replace')
        
        flash_offset = struct.unpack("<Q", data[0x20:0x28])[0]
        size = struct.unpack("<Q", data[0x28:0x30])[0]
        flags = struct.unpack("<I", data[0x30:0x34])[0]

        return cls(name=name, flash_offset=flash_offset, size=size, flags=flags)


@dataclass
class SegmentEntry:
    """Segment table entry (0x20 = 32 bytes)"""
    img_offset: int         # 0x00: Offset in image file (8 bytes)
    flash_offset: int       # 0x08: Target flash offset (8 bytes)
    compress_size: int      # 0x10: Compressed data size (8 bytes)
    decompress_size: int    # 0x18: Decompressed data size (8 bytes)

    @classmethod
    def from_bytes(cls, data: bytes) -> "SegmentEntry":
        if len(data) < SEGMENT_ENTRY_SIZE:
            raise ValueError(f"Segment entry too short: {len(data)}")

        img_offset = struct.unpack("<Q", data[0x00:0x08])[0]
        flash_offset = struct.unpack("<Q", data[0x08:0x10])[0]
        compress_size = struct.unpack("<Q", data[0x10:0x18])[0]
        decompress_size = struct.unpack("<Q", data[0x18:0x20])[0]

        return cls(
            img_offset=img_offset,
            flash_offset=flash_offset,
            compress_size=compress_size,
            decompress_size=decompress_size,
        )


class OTAExtractor:
    """OTA firmware extractor"""

    def __init__(self, input_path: str, output_dir: str, verbose: bool = False):
        self.input_path = Path(input_path)
        self.output_dir = Path(output_dir)
        self.verbose = verbose
        self.header: Optional[OTAHeader] = None
        self.partitions: List[PartitionEntry] = []
        self.segments: List[SegmentEntry] = []
        self.data_start = 0  # Start of segment data

    def extract(self) -> int:
        """
        Extract OTA firmware into partition images.
        
        Returns:
            0 on success, non-zero on failure
        """
        self.output_dir.mkdir(parents=True, exist_ok=True)

        try:
            with open(self.input_path, "rb") as fp:
                return self._extract_from_file(fp)
        except FileNotFoundError:
            print(f"Failed to open input image: {self.input_path}", file=sys.stderr)
            return -1
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            if self.verbose:
                import traceback
                traceback.print_exc()
            return -1

    def _extract_from_file(self, fp) -> int:
        """Extract from open file handle"""
        
        fp.seek(0)

        # Read base header
        header_data = fp.read(BASE_HEADER_SIZE)
        if len(header_data) < BASE_HEADER_SIZE:
            print("Failed to read header", file=sys.stderr)
            return -1

        self.header = OTAHeader.from_bytes(header_data)

        # Validate magic
        if self.header.magic != OTA_MAGIC:
            print(f"Invalid OTA magic: 0x{self.header.magic:X} (expected 0x{OTA_MAGIC:X})", 
                  file=sys.stderr)
            return -1

        # Validate hash and sig sizes
        if self.header.hash_size != 0x20:
            print(f"Warning: Unexpected hash size: 0x{self.header.hash_size:X} (expected 0x20)")
        if self.header.sig_size != 0x100:
            print(f"Warning: Unexpected sig size: 0x{self.header.sig_size:X} (expected 0x100)")

        self.header.print_info()

        # Calculate offsets (matching the C code from disassembly)
        # Layout: base_header | header_ext | hash+sig | rom | loader | partitions | segments | data
        header_ext_start = BASE_HEADER_SIZE
        hash_sig_start = header_ext_start + self.header.header_ext_size
        rom_start = hash_sig_start + 0x120  # 0x20 hash + 0x100 signature
        loader_start = rom_start + self.header.rom_size
        partition_table_start = loader_start + self.header.loader_size
        segment_table_start = partition_table_start + (self.header.num_partitions * 0x34)
        self.data_start = segment_table_start + (self.header.num_segments * 0x20)

        if self.verbose:
            print(f"\nCalculated Offsets (matching firmware):")
            print(f"  Base header:      0x{0:X}")
            print(f"  Header ext:       0x{header_ext_start:X} (size=0x{self.header.header_ext_size:X})")
            print(f"  Hash+Sig:         0x{hash_sig_start:X} (hash_size=0x20, sig_size=0x100)")
            print(f"  ROM data:         0x{rom_start:X} (size=0x{self.header.rom_size:X})")
            print(f"  Loader data:      0x{loader_start:X} (size=0x{self.header.loader_size:X})")
            print(f"  Partition table:  0x{partition_table_start:X} ({self.header.num_partitions}×0x34)")
            print(f"  Segment table:    0x{segment_table_start:X} ({self.header.num_segments}×0x20)")
            print(f"  Segment data:     0x{self.data_start:X}")

        # Print hash and signature
        self._print_signature_info(fp, hash_sig_start)

        # Read partition table
        fp.seek(partition_table_start)
        print("\nUpgrade image partition table:")
        for i in range(self.header.num_partitions):
            entry_data = fp.read(0x34)
            partition = PartitionEntry.from_bytes(entry_data)
            self.partitions.append(partition)
            print(f"part {i}, name {partition.name:12s}, length 0x{partition.size:08x}, "
                  f"flash_offset 0x{partition.flash_offset:08x}, flags 0x{partition.flags:x}")

        # Read segment table
        fp.seek(segment_table_start)
        print("\nUpgrade image segments:")
        for i in range(self.header.num_segments):
            entry_data = fp.read(0x20)
            segment = SegmentEntry.from_bytes(entry_data)
            self.segments.append(segment)
            print(f"segment {i:03d}, img offset=0x{segment.img_offset:08x}, "
                  f"flash offset=0x{segment.flash_offset:08x}, "
                  f"compress size=0x{segment.compress_size:08x}, "
                  f"decompress size=0x{segment.decompress_size:08x}")

        print("\n====================upgrade bin infomation done====================\n")

        # Validate segments
        if not self._validate_segments():
            print("Warning: Segment validation failed, extraction may be incomplete")

        # Extract ROM data if present
        if self.header.rom_size > 0:
            fp.seek(rom_start)
            rom_data = fp.read(self.header.rom_size)
            rom_path = self.output_dir / "rom.bin"
            with open(rom_path, "wb") as out:
                out.write(rom_data)
            print(f"Extracted: rom.bin ({self.header.rom_size} bytes)")

        # Extract loader if present
        if self.header.loader_size > 0:
            fp.seek(loader_start)
            loader_data = fp.read(self.header.loader_size)
            loader_path = self.output_dir / "loader.bin"
            with open(loader_path, "wb") as out:
                out.write(loader_data)
            print(f"Extracted: loader.bin ({self.header.loader_size} bytes)")

        # Extract and decompress partitions
        return self._extract_partitions(fp)

    def _print_signature_info(self, fp, hash_sig_start: int):
        """Print signature information like the firmware does"""
        fp.seek(hash_sig_start)
        hash_data = fp.read(0x20)
        sig_data = fp.read(0x100)
        
        print(f"\nSignature Information:")
        print(f"  Hash (SHA256):     {hash_data.hex()}")
        print(f"  RSA Signature:     {sig_data[:32].hex()}...")
        print(f"  (Full signature: {len(sig_data)} bytes)")

    def _validate_segments(self) -> bool:
        """Validate segment sizes and mapping"""
        if not self.segments:
            return True
            
        # Group segments by partition
        segment_groups = {}
        for i, seg in enumerate(self.segments):
            # Find which partition this segment belongs to
            partition_idx = -1
            for idx, p in enumerate(self.partitions):
                if p.flash_offset <= seg.flash_offset < p.flash_offset + p.size:
                    partition_idx = idx
                    break
            
            if partition_idx == -1:
                print(f"Warning: Segment {i} at flash offset 0x{seg.flash_offset:X} doesn't belong to any partition")
                continue
            
            if partition_idx not in segment_groups:
                segment_groups[partition_idx] = []
            segment_groups[partition_idx].append(seg)
        
        # Validate total decompressed size doesn't exceed partition size
        errors = 0
        for part_idx, segs in segment_groups.items():
            total_decompressed = sum(seg.decompress_size for seg in segs)
            partition = self.partitions[part_idx]
            
            if total_decompressed > partition.size:
                print(f"Error: Partition {partition.name} total decompressed size "
                      f"(0x{total_decompressed:X}) exceeds partition size (0x{partition.size:X})")
                errors += 1
        
        # Print segment to partition mapping if verbose
        if self.verbose and segment_groups:
            print("\nSegment to Partition Mapping:")
            for seg_idx, seg in enumerate(self.segments):
                for p_idx, p in enumerate(self.partitions):
                    if p.flash_offset <= seg.flash_offset < p.flash_offset + p.size:
                        print(f"  Segment {seg_idx:3d} -> Partition {p_idx:2d} ({p.name})")
                        break
        
        return errors == 0

    def _extract_partitions(self, fp) -> int:
        """Extract partition images from segments"""
        
        # Group segments by partition based on flash_offset
        partition_data = {p.name: bytearray() for p in self.partitions}
        
        for seg_idx, seg in enumerate(self.segments):
            # Find which partition this segment belongs to based on flash_offset
            current_partition = None
            for p in self.partitions:
                if p.flash_offset <= seg.flash_offset < p.flash_offset + p.size:
                    current_partition = p
                    break
            
            if current_partition is None:
                print(f"Warning: Segment {seg_idx} at flash offset 0x{seg.flash_offset:X} "
                      f"doesn't belong to any partition")
                continue

            # Read compressed segment data
            # img_offset is absolute from the start of OTA header
            fp.seek(seg.img_offset)
            compressed_data = fp.read(seg.compress_size)
            
            if len(compressed_data) != seg.compress_size:
                print(f"Warning: Failed to read segment {seg_idx} data (got {len(compressed_data)}, "
                      f"expected {seg.compress_size})")
                continue

            # Decompress if needed
            if self.header.compressed:
                try:
                    decompressed = lzo_decompress(compressed_data, seg.decompress_size)
                    if len(decompressed) != seg.decompress_size:
                        print(f"Warning: Segment {seg_idx} decompressed size mismatch "
                              f"(got {len(decompressed)}, expected {seg.decompress_size})")
                except Exception as e:
                    print(f"Warning: LZO decompression failed for segment {seg_idx}: {e}")
                    # Save raw compressed data with marker
                    decompressed = compressed_data
            else:
                decompressed = compressed_data

            partition_data[current_partition.name].extend(decompressed)
            
            if self.verbose:
                print(f"  Segment {seg_idx} -> {current_partition.name}: "
                      f"{len(decompressed)} bytes decompressed")

        # Write partition images
        print("\nExtracting partition images:")
        for p in self.partitions:
            if p.name not in partition_data or len(partition_data[p.name]) == 0:
                print(f"  Skipping {p.name}: no data")
                continue
            
            # Sanitize filename
            safe_name = p.name.replace("/", "_").replace("\\", "_")
            out_path = self.output_dir / f"{safe_name}.img"
            
            data = bytes(partition_data[p.name])
            with open(out_path, "wb") as out:
                out.write(data)
            
            print(f"  {safe_name}.img: {len(data)} bytes (expected: {p.size})")

        return 0

    def info_only(self) -> int:
        """Just print header info without extracting"""
        try:
            with open(self.input_path, "rb") as fp:
                fp.seek(0)

                header_data = fp.read(BASE_HEADER_SIZE)
                self.header = OTAHeader.from_bytes(header_data)
                
                if self.header.magic != OTA_MAGIC:
                    print(f"Invalid OTA magic: 0x{self.header.magic:X}", file=sys.stderr)
                    return -1

                self.header.print_info()
                
                # Read and show partition table
                header_ext_start = BASE_HEADER_SIZE
                hash_sig_start = header_ext_start + self.header.header_ext_size
                rom_start = hash_sig_start + HASH_SIG_SIZE
                loader_start = rom_start + self.header.rom_size
                partition_table_start = loader_start + self.header.loader_size
                
                fp.seek(partition_table_start)
                print("\nPartition table:")
                for i in range(self.header.num_partitions):
                    entry_data = fp.read(PARTITION_ENTRY_SIZE)
                    partition = PartitionEntry.from_bytes(entry_data)
                    print(f"  part {i}: {partition.name:12s} size=0x{partition.size:08x} "
                          f"flash_off=0x{partition.flash_offset:08x}")
                
                return 0
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            return -1


def main():
    parser = argparse.ArgumentParser(
        description="AR FPV OTA Firmware Extractor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s firmware.ota output/
  %(prog)s firmware.ota               # extracts to firmware_extracted/
  %(prog)s --info-only firmware.ota   # just print header info
  %(prog)s -v firmware.ota output/    # verbose output
        """
    )
    parser.add_argument("input", help="Input OTA firmware image")
    parser.add_argument("output", nargs="?", help="Output directory (default: <input>_extracted)")
    parser.add_argument("--info-only", "-i", action="store_true",
                        help="Only print header info, don't extract")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Verbose output")

    args = parser.parse_args()

    input_path = args.input
    output_dir = args.output
    if not output_dir and not args.info_only:
        output_dir = Path(input_path).stem + "_extracted"

    extractor = OTAExtractor(input_path, output_dir or "", args.verbose)

    if args.info_only:
        result = extractor.info_only()
    else:
        result = extractor.extract()

    return 0 if result == 0 else 1


if __name__ == "__main__":
    sys.exit(main())