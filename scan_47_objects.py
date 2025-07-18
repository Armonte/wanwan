#!/usr/bin/env python3

import struct

print('FM2K Character Select: 47 Object Analysis')
print('=' * 50)

# Base addresses for first 50 slots
base_addr = 0x4701E0
object_size = 382

print('Object Type Distribution:')

# We'll use MCP to read the first 4 bytes (type) of each slot
# For now, let's prepare the scan addresses
print('Scanning object types in first 50 slots...')
for slot in range(50):
    addr = base_addr + (slot * object_size)
    print(f'Slot {slot:2d}: 0x{addr:08X}')