#!/usr/bin/env python3
"""
Artosyn U-Boot USB HID firmware flasher.

Protocol (reversed from U-Boot binary):

  HID report size = MTU = 64 bytes (state->mtu at state+0xde, confirmed empirically).
  Each device.write() sends exactly as many bytes as given (hidapi does NOT pad).

  Outer OTRA header (36 bytes, 0x00-0x23):
    0x00  8  Magic: FF 55 FF AA 41 52 54 4F
    0x08  8  zeros
    0x10  4  inner_size: actual inner bytes in THIS packet (must be ≤ MTU=64)
    0x14  4  total_chunks (u32 LE)
    0x18  4  chunk_index (u32 LE, 0-based)
    0x1C  1  sub_proto = 0x01
    0x1D  7  zeros

  Inner header (12 bytes, 0x24-0x2F, FIRST CHUNK ONLY):
    0x24  2  port_index = 0x0000 (u16 LE)
    0x26  2  header_len = 0x000C (u16 LE)
    0x28  4  payload_size = total payload for this inner packet (u32 LE)
    0x2C  4  zeros (padding)

  Firmware payload starts at 0x30 (first chunk) or 0x24 (continuation chunks).

  Per-chunk capacity:
    First chunk:        MTU(64) - outer(36) - inner_hdr(12) = 16 bytes of firmware
    Continuation chunk: MTU(64) - outer(36)                  = 28 bytes of firmware

  Start command (single outer chunk, 56 bytes total — NOT padded to 64):
    inner_size = 20 (= 12 + 8)
    total_chunks = 1, chunk_index = 0
    payload: b"upgd" + fw_total_size_u32_LE  (8 bytes)

  Data session (one giant multi-chunk session for the whole firmware):
    total_chunks = ceil((12 + fw_size) / 28)
    chunk 0: outer(inner_size=28) + inner_hdr(payload_size=fw_size) + fw[0:16]
    chunk i: outer(inner_size=28) + fw[16+(i-1)*28 : 16+i*28]   (zero-padded to 64)

  For continuation chunks, the device handler receives x1 = actual_length - 36 = 28
  (confirmed from assembly at 0x441b4: sub w1, w23, #0x24).

Signature: the device enforces RSA-2048 + SHA-256 verification (3 hardcoded keys).
Only valid signed ARTO images (magic 0x4152544F) will be accepted after upload.
"""

import math
import sys
import time
import struct
import argparse
import hid

REBOOT_CMD = b'\xAA\x05\x00\x1D\x00\x00\xCC\xEB\xAA\x00'
SERIAL_DEV = '/dev/ttyACM0'
SERIAL_BAUD = 1152000

HID_VID = 0x1d6b
HID_PID = 0x0100

OTRA_MAGIC = bytes([0xFF, 0x55, 0xFF, 0xAA, 0x41, 0x52, 0x54, 0x4F])

MTU        = 64   # HID report size = state->mtu (confirmed: device prints "size out of mtu 64")
OUTER_HDR  = 36   # bytes 0x00-0x23
INNER_HDR  = 12   # bytes 0x24-0x2F (first chunk only)
FIRST_PLD  = MTU - OUTER_HDR - INNER_HDR   # 16 bytes of firmware in first chunk
CONT_PLD   = MTU - OUTER_HDR               # 28 bytes of firmware in continuation chunks


def _make_outer_hdr(inner_size: int, total_chunks: int, chunk_index: int) -> bytearray:
    hdr = bytearray(OUTER_HDR)
    hdr[0:8] = OTRA_MAGIC
    struct.pack_into('<I', hdr, 0x10, inner_size)
    struct.pack_into('<I', hdr, 0x14, total_chunks)
    struct.pack_into('<I', hdr, 0x18, chunk_index)
    hdr[0x1C] = 0x01  # sub_proto
    return hdr


def build_start_cmd(fw_size: int) -> bytes:
    """56-byte start command packet (NOT padded to 64)."""
    cmd = b'upgd' + struct.pack('<I', fw_size)   # 8 bytes
    inner_size = INNER_HDR + len(cmd)            # 20
    pkt = bytearray(OUTER_HDR + INNER_HDR + len(cmd))
    pkt[0:OUTER_HDR] = _make_outer_hdr(inner_size, total_chunks=1, chunk_index=0)
    struct.pack_into('<H', pkt, 0x24, 0)          # port = 0
    struct.pack_into('<H', pkt, 0x26, INNER_HDR)  # header_len = 12
    struct.pack_into('<I', pkt, 0x28, len(cmd))   # payload_size = 8
    pkt[0x30:] = cmd
    return bytes(pkt)


def iter_data_chunks(fw_data: bytes):
    """Yield exactly MTU=64 byte HID OUT packets for the full firmware."""
    fw_size = len(fw_data)
    total_chunks = math.ceil((INNER_HDR + fw_size) / CONT_PLD)

    # First chunk: outer header + inner header + first 16 bytes of firmware
    first_pld = fw_data[:FIRST_PLD]
    pkt = bytearray(MTU)
    pkt[0:OUTER_HDR] = _make_outer_hdr(
        inner_size=INNER_HDR + len(first_pld),
        total_chunks=total_chunks,
        chunk_index=0,
    )
    struct.pack_into('<H', pkt, 0x24, 0)          # port = 0
    struct.pack_into('<H', pkt, 0x26, INNER_HDR)  # header_len = 12
    struct.pack_into('<I', pkt, 0x28, fw_size)    # payload_size = full firmware size
    pkt[0x30:0x30 + len(first_pld)] = first_pld
    yield bytes(pkt)

    # Continuation chunks: outer header + up to 28 bytes of firmware (zero-padded to 64)
    pos = FIRST_PLD
    chunk_idx = 1
    while pos < fw_size:
        chunk = fw_data[pos:pos + CONT_PLD]
        pkt = bytearray(MTU)
        pkt[0:OUTER_HDR] = _make_outer_hdr(CONT_PLD, total_chunks, chunk_idx)
        pkt[OUTER_HDR:OUTER_HDR + len(chunk)] = chunk
        yield bytes(pkt)
        pos += len(chunk)
        chunk_idx += 1


def send_hid(dev: hid.device, packet: bytes):
    dev.write(bytes([0x00]) + packet)


def read_hid(dev: hid.device, timeout_ms: int = 100) -> bytes | None:
    data = dev.read(MTU, timeout_ms)
    if data:
        return bytes(data)
    return None


def wait_for_device(timeout_s: int = 30) -> hid.device:
    print(f"Waiting up to {timeout_s}s for USB HID {HID_VID:04x}:{HID_PID:04x}...")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for _ in hid.enumerate(HID_VID, HID_PID):
            dev = hid.device()
            try:
                dev.open(HID_VID, HID_PID)
                print(f"  Found: [{dev.get_manufacturer_string()}] {dev.get_product_string()}")
                dev.set_nonblocking(1)
                return dev
            except Exception:
                pass
        time.sleep(0.5)
    raise TimeoutError(f"Device {HID_VID:04x}:{HID_PID:04x} did not appear within {timeout_s}s")


def send_reboot_command(serial_dev: str):
    try:
        import serial
        print(f"Sending reboot command via {serial_dev} @ {SERIAL_BAUD} baud...")
        with serial.Serial(serial_dev, SERIAL_BAUD, timeout=1) as s:
            s.write(REBOOT_CMD)
        print("  Done.")
    except ImportError:
        import subprocess
        cmd = (
            f"printf '\\xAA\\x05\\x00\\x1D\\x00\\x00\\xCC\\xEB\\xAA\\x00' "
            f"| socat - {serial_dev},b{SERIAL_BAUD},raw,echo=0"
        )
        print(f"  pyserial not found, using socat: {cmd}")
        subprocess.run(cmd, shell=True, check=True)
        print("  Done.")
    except Exception as e:
        print(f"  Warning: {e} — assuming device is already in HID flash mode.")


def verify_arto_magic(data: bytes) -> bool:
    return len(data) >= 4 and struct.unpack_from('<I', data, 0)[0] == 0x4152544F


def flash(firmware_path: str, serial_dev: str, skip_reboot: bool):
    print(f"Reading firmware: {firmware_path}")
    with open(firmware_path, 'rb') as f:
        fw_data = f.read()
    fw_size = len(fw_data)
    print(f"  Size: {fw_size} bytes")

    if not verify_arto_magic(fw_data):
        print("WARNING: No ARTO magic. Device enforces RSA-2048+SHA-256 — unsigned images fail.")
        if input("Continue anyway? [y/N] ").strip().lower() != 'y':
            sys.exit(1)
    else:
        print("  ARTO magic OK.")

    if not skip_reboot:
        send_reboot_command(serial_dev)
        time.sleep(2)

    dev = wait_for_device(timeout_s=30)

    try:
        # Step 1: Start command
        print("\nStep 1: Send start command...")
        send_hid(dev, build_start_cmd(fw_size))
        time.sleep(0.2)
        resp = read_hid(dev, timeout_ms=500)
        if resp:
            print(f"  Response: {resp.hex()}")

        # Step 2: Send firmware as one multi-chunk data session
        total_chunks = math.ceil((INNER_HDR + fw_size) / CONT_PLD)
        print(f"\nStep 2: Send {fw_size} bytes in {total_chunks} HID OUT chunks "
              f"({FIRST_PLD} bytes first, {CONT_PLD} bytes each continuation)...")

        t0 = time.time()
        bytes_sent = 0
        report_every = max(1, total_chunks // 200)   # ~200 progress prints

        for i, pkt in enumerate(iter_data_chunks(fw_data)):
            send_hid(dev, pkt)

            if i == 0:
                bytes_sent += FIRST_PLD
            else:
                bytes_sent += min(CONT_PLD, fw_size - FIRST_PLD - (i - 1) * CONT_PLD)

            if i % report_every == 0 or i == total_chunks - 1:
                pct = bytes_sent * 100 // fw_size
                elapsed = time.time() - t0
                rate = bytes_sent / elapsed / 1024 if elapsed > 0 else 0
                print(f"\r  {bytes_sent}/{fw_size} bytes ({pct}%) "
                      f"chunk {i+1}/{total_chunks} {rate:.1f} KB/s   ",
                      end='', flush=True)

            # Check for device status reports occasionally (non-blocking)
            if i % 1000 == 999:
                resp = read_hid(dev, timeout_ms=0)
                if resp and any(resp):
                    print(f"\n  Device status at chunk {i+1}: {resp.hex()}")

        print()
        elapsed = time.time() - t0
        print(f"  Transfer complete in {elapsed:.1f}s "
              f"({fw_size / elapsed / 1024:.1f} KB/s net)")

        # Step 3: Wait for completion.
        # The device sends HID IN reports (same OTRA format, reversed) with ASCII status
        # strings starting at byte 0x30 of each chunk. For a 64-byte payload (0x40) the
        # response spans 3 × 64-byte HID packets; "upgrade_complete" (16 chars) fits
        # exactly in bytes [0x30..0x3f] of chunk 0. Progress strings:
        #   "Upgrade_status: %d percent %d"
        #   "upgrade_complete" / "upgrade_failed: ret = %d"
        # The device often prints "Hid send timeout!" to serial when the NAND write keeps
        # the CPU busy and the host isn't polling fast enough — use non-blocking reads so
        # we drain every packet the device manages to queue.
        # Switch to blocking mode so read() actually waits for HID IN data.
        # set_nonblocking(1) was called during device open; in non-blocking mode
        # timeout_ms is silently ignored and reads return [] immediately.
        dev.set_nonblocking(0)

        print("\nStep 3: Waiting for flash completion (NAND write takes ~2–3 min)...")
        deadline = time.time() + 300  # 5 minutes max
        flash_complete = False
        last_dot = time.time()
        assembled = bytearray()
        while time.time() < deadline:
            try:
                resp = read_hid(dev, timeout_ms=20)
            except OSError:
                print("\n  Device disconnected — flash complete, device rebooted.")
                flash_complete = True
                break
            if resp and any(resp):
                raw = bytes(resp)
                # Extract ASCII payload: skip 36-byte outer header; first chunk also has
                # 12-byte inner header so string starts at 0x30, continuations at 0x24.
                chunk_idx = struct.unpack_from('<I', raw, 0x18)[0] if len(raw) >= 0x1c else 0
                payload_off = 0x30 if chunk_idx == 0 else 0x24
                text_bytes = raw[payload_off:] if len(raw) > payload_off else b''
                text = text_bytes.rstrip(b'\x00').decode('ascii', errors='replace')
                # chunk 0 = new message; reset buffer so we don't match stale data
                if chunk_idx == 0:
                    assembled = bytearray(text_bytes)
                else:
                    assembled.extend(text_bytes)
                if text.strip():
                    print(f"\n  Device [{chunk_idx}]: {text.strip()!r}")
                # Check assembled buffer across chunks for completion keywords
                if b'upgrade_complete' in assembled:
                    print("  Flash complete signal received!")
                    flash_complete = True
                    break
                if b'upgrade_failed' in assembled:
                    print("  Flash FAILED signal received!")
                    flash_complete = True
                    break
            else:
                assembled.clear()
                if time.time() - last_dot >= 2:
                    print(".", end='', flush=True)
                    last_dot = time.time()

        print()
        if flash_complete:
            print("Done. Device will reboot automatically (or send reboot command).")
        else:
            print("Timeout waiting for completion — check serial console.")

    finally:
        dev.close()


def main():
    parser = argparse.ArgumentParser(
        description='Artosyn U-Boot USB HID flasher for BetaFPV P1 (SKY unit)'
    )
    parser.add_argument('firmware', help='Firmware image (.img, ARTO format)')
    parser.add_argument('--serial', default=SERIAL_DEV,
                        help=f'Serial port for reboot command (default: {SERIAL_DEV})')
    parser.add_argument('--skip-reboot', action='store_true',
                        help='Device already in HID flash mode, skip serial reboot')
    args = parser.parse_args()
    flash(args.firmware, args.serial, args.skip_reboot)


if __name__ == '__main__':
    main()
