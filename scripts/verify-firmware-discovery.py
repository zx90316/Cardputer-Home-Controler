"""Verify the firmware's embedded Tapo discovery packet without credentials."""

from __future__ import annotations

import binascii
import re
import secrets
import socket
import struct
import time
from pathlib import Path


def main() -> None:
    source = Path("firmware/src/adapters/TapoAdapter.cpp").read_text(encoding="utf-8")
    match = re.search(r'R"JSON\((.*?)\)JSON"', source, re.DOTALL)
    if match is None:
        raise RuntimeError("firmware discovery payload was not found")
    payload = match.group(1).encode()
    header = struct.pack(
        ">BBHHBBII", 2, 0, 1, len(payload), 17, 0, secrets.randbits(32), 0x5A6B7C8D
    )
    query = bytearray(header + payload)
    query[12:16] = binascii.crc32(query).to_bytes(4, "big")

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    udp.settimeout(0.4)
    udp.bind(("", 0))
    udp.sendto(query, ("255.255.255.255", 20002))
    deadline = time.monotonic() + 5
    found: set[str] = set()
    while time.monotonic() < deadline:
        try:
            data, address = udp.recvfrom(4096)
        except TimeoutError:
            continue
        if b"L530" in data[16:]:
            found.add(address[0])
    udp.close()
    print(f"FIRMWARE_PACKET_L530E={len(found)}")


if __name__ == "__main__":
    main()
