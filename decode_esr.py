esr = 0x8600000E
ec = (esr >> 26) & 0x3F
iss = esr & 0xFFFFFF
print(f"EC: 0x{ec:02X}")
print(f"ISS: 0x{iss:06X}")
