#!/usr/bin/env python3
"""Pure reference checks for Pico->Uno UART framing and throttle transform."""
def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ (0x07 if c & 0x80 else 0)) & 0xff
    return c

def command(ch3, armed):
    if not armed or ch3 <= 448:
        return 0
    return 48 + (min(ch3, 1680) - 448) * 1999 // (1680 - 448)

def packet(seq, ch3, status):
    d = [0xa5, 0x5a, 1, seq & 0xff, ch3 & 0xff, ch3 >> 8, status]
    return bytes(d + [crc8(d)])

assert command(368, True) == 0
assert command(448, True) == 0
assert command(449, True) == 49
assert command(1680, True) == 2047
assert command(1904, True) == 2047
assert command(1680, False) == 0
assert len(packet(13, 1080, 3)) == 8
assert crc8(packet(13, 1080, 3)) == 0
b = bytearray(packet(13, 1080, 3)); b[4] ^= 0x01
assert crc8(b) != 0
print("protocol / mapping tests passed")
