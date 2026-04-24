# Artosyn U-Boot USB HID Flash Protocol

Reversed from the U-Boot binary for the BetaFPV P1 / Artosyn AR9311 platform.
Binary analyzed in Ghidra (static addresses; runtime base = 0x20000000).

---

## Overview

When the device receives the serial reboot command, it boots into U-Boot and enters
`artosyn_hid_upgrade`, which presents a USB HID gadget:

```
idVendor=0x1d6b   idProduct=0x0100
Product:      "USB download gadget"
Manufacturer: "U-Boot"
```

The host sends firmware to the device via HID OUT reports. The device streams status
back via HID IN reports. After receiving the complete image the device verifies the
RSA-2048 + SHA-256 signature and flashes to NAND.

---

## Reboot Into Flash Mode

Send 10 bytes to `/dev/ttyACM0` at 1152000 baud:

```
AA 05 00 1D 00 00 CC EB AA 00
```

```bash
printf '\xAA\x05\x00\x1D\x00\x00\xCC\xEB\xAA\x00' \
    | socat - /dev/ttyACM0,b1152000,raw,echo=0
```

The device reboots and the HID gadget appears within ~2 seconds.
You can also manually trigger flash mode from the U-Boot prompt:

```
=> artosyn_hid_upgrade 0
```

---

## Packet Format (OTRA Protocol)

All communication uses a two-layer framing scheme called "OTRA" internally.

### Layer 1 — Outer OTRA Header (36 bytes, offsets 0x00–0x23)

| Offset | Size | Value | Description |
|--------|------|-------|-------------|
| 0x00 | 8 | `FF 55 FF AA 41 52 54 4F` | Magic ("ARTO" reversed + preamble) |
| 0x08 | 8 | `00 00 00 00 00 00 00 00` | Reserved / zeros |
| 0x10 | 4 | `inner_size` (u32 LE) | = `0x0C + payload_len` |
| 0x14 | 4 | `total_chunks` (u32 LE) | Total packet count for this transfer |
| 0x18 | 4 | `chunk_index` (u32 LE) | Index of this packet (0-based) |
| 0x1C | 1 | `0x01` | sub_proto (must be exactly 1) |
| 0x1D | 7 | `00 …` | Reserved / zeros |

### Layer 2 — Inner Header (12 bytes, offsets 0x24–0x2F)

| Offset | Size | Value | Description |
|--------|------|-------|-------------|
| 0x24 | 2 | `0x0000` (u16 LE) | Port index (0 = upgrade handler) |
| 0x26 | 2 | `0x000C` (u16 LE) | Inner header length (always 12) |
| 0x28 | 4 | `payload_len` (u32 LE) | Payload byte count for this packet |
| 0x2C | 4 | `0x00000000` | Padding |

### Payload (variable, offset 0x30+)

Raw bytes for the specific command or data chunk.

**Total overhead: 0x30 = 48 bytes.**

### MTU / Maximum Payload Per Packet

**Confirmed empirically: MTU = 64 bytes.**

Despite the USB endpoint's wMaxPacketSize = 1024, the device's U-Boot HID stack stores
a separate `mtu` field at `state+0xde` (a `ushort`). It is set during USB gadget bind
to 64 (the HID report descriptor's wMaxPacketSize). The check at `FUN_00043fa0` is:

```c
if ((uint)*(ushort *)(state + 0xde) < inner_size)  // inner_size from outer header
    → "size out of mtu 64(N)!" error, packet dropped
```

Sending a 988-byte inner packet triggers `"size out of mtu 64(988)!"` — proving MTU=64.

Each HID OUT write must be **exactly 64 bytes** (zero-padded). The usable payload per
write after removing the 36-byte outer header:

```
continuation payload = 64 − 36 = 28 bytes of firmware per HID write
first-chunk payload  = 64 − 36 − 12 = 16 bytes of firmware (inner header present)
```

### HID Report ID

`hidapi` requires prepending a report ID byte. Prepend `0x00` (no report ID):

```python
device.write(bytes([0x00]) + packet)
```

---

## Flash Sequence

### Step 1 — Start Command

Send a single packet with port=0, payload = 8 bytes:

```
75 70 67 64                     "upgd" — command magic
<total_size: u32 LE>            full firmware image size in bytes
```

The device logs `"upgrade start cmd check!"`, resets its receive buffer
(base address 0x24900000, max ~183 MB), and stores the expected total size.

### Step 2 — Data Chunks

Split the raw firmware image into chunks of ≤ 976 bytes.
Send each as an OTRA packet with:

- `total_chunks` = total number of chunks
- `chunk_index` = 0, 1, 2, … (must be sequential)
- payload = firmware bytes for that chunk

The device copies each chunk into DRAM at `0x24900000 + received_offset`.

### Step 3 — Completion

When `received_offset >= total_size`, the device:

1. Logs `"upgrade receive complete(offset: N total_len: N)!"`
2. Calls the upgrade function which verifies the ARTO image signature
3. Writes to NAND and reboots

Status is reported back via HID IN (64-byte reads from the device).

---

## Chunking for Large Transfers

For any payload that exceeds 976 bytes (i.e. all data packets), the outer header
encodes the split across multiple HID writes:

```
total_chunks = ceil(payload_len / 976)
```

Each HID write carries one chunk. The device state machine tracks `chunk_index`
and reassembles at the inner handler. The inner header (port, header_len,
payload_size) is only present in the **first chunk** (`chunk_index == 0`);
subsequent chunks contain raw continuation bytes starting at offset 0x24.

For the start command (8-byte payload, fits in one packet), always use
`total_chunks=1, chunk_index=0`.

---

## Signature Verification

**The device enforces RSA-2048 + SHA-256 signature verification. Unsigned images
are rejected.**

Function `FUN_00006cf0` (binary 0x6cf0) — do_upgrade:

1. Checks firmware magic = `0x4152544F` ("ARTO")
2. Checks `hash_size == 0x20` (SHA-256)
3. Checks `sig_size == 0x100` (RSA-2048)
4. Calls `spl_verify_sw` (`FUN_000631f8`) with 3 hardcoded RSA public keys
5. Hash is at `image + 0x100` (32 bytes)
6. RSA signature is at `image + 0x120` (256 bytes)
7. Signed data starts at `image + 0x220`
8. Flash type must be `0x02` (NAND)

Only Artosyn-signed OTA images (e.g. `P1_SKY_v2.0.5_20260330_a22d901.img`) will
be accepted.

---

## Registered Port Handlers (Dispatch Table @ binary 0x7daf8)

| Port | Binary address | Description |
|------|---------------|-------------|
| 0    | 0x44898       | Upgrade handler (upgd command + data receive) |
| 1–4  | NULL          | Unregistered |
| 5    | 0x45BD8       | USB class driver |
| 6    | 0x45DA0       | USB endpoint driver |

Table stride: 0x18 (24 bytes per entry, first 8 bytes = function pointer).
Runtime addresses = binary address + 0x20000000.

---

## Key Functions (U-Boot binary, static addresses)

| Address | Name | Description |
|---------|------|-------------|
| 0x43fa0 | `artosyn_hid_upgrade` | HID OUT completion callback, OTRA outer framing |
| 0x43b10 | `inner_packet_dispatch` | Parses inner header, routes to port handler |
| 0x43a58 | `alloc_inner_packet` | Allocates and populates inner header |
| 0x44898 | `upgrade_port0_handler` | "upgd" command + firmware data receiver |
| 0x44810 | `upgrade_status_callback` | Sends HID IN status/progress reports |
| 0x44368 | `hid_in_send` | Sends HID IN report (chunks via MTU) |
| 0x06cf0 | `do_upgrade` | ARTO magic check, RSA verify, NAND flash |
| 0x631f8 | `spl_verify_sw` | RSA-2048 + SHA-256 signature verification |

---

## Serial Console Notes

- Serial is available at `/dev/ttyACM0` (normal boot) at 1152000 baud
- In U-Boot HID mode the serial console is still active; press Ctrl+C to abort
  `artosyn_hid_upgrade` and fall back to the U-Boot prompt
- "magic num error!" on serial = outer magic check failed (wrong packet format)
- "upgrade start cmd check!" = start command received
- "upgrade receive complete…" = all data received, verification starting
- "upgrade complete!" + "artousb upgrade finish..." = success
