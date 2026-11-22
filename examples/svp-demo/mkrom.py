#!/usr/bin/env python3
"""mkrom.py -- pads a cartridge image to a power-of-two size (at least 256 KB), writes the end address at 0x1A4 and the
header checksum at 0x18E (16-bit sum of the big-endian words from 0x200 to the end).
  mkrom.py in.bin out.bin"""
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
n = 0x40000
while n < len(d): n *= 2
d += bytes(n - len(d))
d[0x1A4:0x1A8] = (n - 1).to_bytes(4, 'big')
s = sum(int.from_bytes(d[i:i + 2], 'big') for i in range(0x200, n, 2)) & 0xFFFF
d[0x18E:0x190] = s.to_bytes(2, 'big')
open(sys.argv[2], 'wb').write(d)
print('%s: %d bytes, checksum %04X' % (sys.argv[2], n, s))
