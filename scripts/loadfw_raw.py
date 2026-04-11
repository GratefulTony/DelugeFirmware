#!/usr/bin/env python3
"""Send firmware to Deluge via raw MIDI with ACK-based flow control.

Each sysex firmware segment is sent in small chunks (paced writes to avoid
USB transfer drops on Linux 6.x kernels), then the script waits for the
Deluge to ACK before sending the next segment.
"""

import os
import sys
import time
import binascii
import select


def pack_8_to_7_bits(src, dstsize):
    packets = (len(src) + 6) // 7
    dst = bytearray(dstsize)
    for i in range(packets):
        ipos = 7 * i
        opos = 8 * i
        for j in range(7):
            if ipos + j < len(src):
                temp = src[ipos + j] & 0x7F
                dst[opos + 1 + j] = temp
                if src[ipos + j] & 0x80:
                    dst[opos] |= 1 << j
    return dst


def make_segment(segment_number, segment_data, handshake):
    data = bytearray(601)
    data[0] = 0xF0
    data[1:5] = bytes([0x00, 0x21, 0x7B, 0x01])
    data[5] = 3
    data[6] = 1
    data[7:12] = pack_8_to_7_bits(handshake.to_bytes(4, byteorder="little"), 5)
    data[12] = segment_number & 0x7F
    data[13] = (segment_number >> 7) & 0x7F
    data[14:-1] = pack_8_to_7_bits(segment_data, 586)
    data[-1] = 0xF7
    return data


def make_load_command(checksum, length, handshake):
    data = bytearray(22)
    data[0] = 0xF0
    data[1:5] = bytes([0x00, 0x21, 0x7B, 0x01])
    data[5] = 3
    data[6] = 2
    data[7:21] = pack_8_to_7_bits(
        handshake.to_bytes(4, byteorder="little")
        + length.to_bytes(4, byteorder="little")
        + checksum.to_bytes(4, byteorder="little"),
        14,
    )
    data[21] = 0xF7
    return data


def send_paced(fd, data, chunk_size=48, chunk_delay=0.004):
    """Write sysex in small chunks to avoid USB transfer drops."""
    for i in range(0, len(data), chunk_size):
        chunk = bytes(data[i : i + chunk_size])
        while True:
            try:
                os.write(fd, chunk)
                break
            except BlockingIOError:
                time.sleep(0.001)
        if i + chunk_size < len(data):
            time.sleep(chunk_delay)


def send_fast(fd, data, chunk_size=48, chunk_delay=0.001):
    """Write sysex in small chunks with minimal delay."""
    for i in range(0, len(data), chunk_size):
        chunk = bytes(data[i : i + chunk_size])
        while True:
            try:
                os.write(fd, chunk)
                break
            except BlockingIOError:
                time.sleep(0.001)
        if i + chunk_size < len(data):
            time.sleep(chunk_delay)


def wait_for_ack(fd, timeout=2.0):
    """Wait for ACK sysex from Deluge. Returns True if ACK received."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        remaining = deadline - time.time()
        r, _, _ = select.select([fd], [], [], min(remaining, 0.1))
        if r:
            try:
                buf += os.read(fd, 256)
            except BlockingIOError:
                pass
            # Look for ACK: F0 00 21 7B 01 03 01 xx xx F7
            if b"\xf7" in buf:
                return True
        else:
            continue
    return False


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <binary> [device]")
        print("  device: raw MIDI device (default: /dev/snd/midiC2D0)")
        sys.exit(1)

    binary_path = sys.argv[1]
    device = sys.argv[2] if len(sys.argv) > 2 else "/dev/snd/midiC2D0"

    with open(".deluge_hex_key") as f:
        handshake = int(f.read().strip(), 16)

    with open(binary_path, "rb") as f:
        binary = f.read()

    checksum = binascii.crc32(binary)
    total_segments = (len(binary) + 511) // 512

    print(f"Firmware: {binary_path} ({len(binary)} bytes, {total_segments} segments)")
    print(f"Device: {device}")

    fd = os.open(device, os.O_RDWR | os.O_NONBLOCK)

    # Drain any pending input
    try:
        while os.read(fd, 1024):
            pass
    except BlockingIOError:
        pass

    t0 = time.time()
    ack_mode = True

    for i in range(total_segments):
        segment = binary[i * 512 : (i + 1) * 512]
        msg = make_segment(i, segment, handshake)

        if ack_mode:
            send_fast(fd, msg)
        else:
            send_paced(fd, msg)

        if ack_mode:
            if not wait_for_ack(fd, timeout=2.0):
                if i == 0:
                    # First packet failed ACK — firmware doesn't support ACK, fall back to paced
                    print(
                        "\nNo ACK from firmware, falling back to timed mode (4ms delay)"
                    )
                    ack_mode = False
                    time.sleep(0.004)
                else:
                    # Retry
                    print(f"\nACK timeout at segment {i}, retrying...")
                    send_paced(fd, msg)
                    if not wait_for_ack(fd, timeout=2.0):
                        print(f"\nFailed at segment {i}")
                        os.close(fd)
                        sys.exit(1)
        else:
            time.sleep(0.004)

        pct = (i + 1) * 100 // total_segments
        bar = "#" * (pct // 2) + "-" * (50 - pct // 2)
        elapsed = time.time() - t0
        rate = (i + 1) / elapsed if elapsed > 0 else 0
        eta = (total_segments - i - 1) / rate if rate > 0 else 0
        print(
            f"\r[{bar}] {i + 1}/{total_segments} ({pct}%) {eta:.0f}s remaining",
            end="",
            flush=True,
        )

    print()

    load_msg = make_load_command(checksum, len(binary), handshake)
    send_paced(fd, load_msg)
    elapsed = time.time() - t0
    print(f"Load command sent in {elapsed:.1f}s. Deluge should reboot.")

    os.close(fd)


if __name__ == "__main__":
    main()
