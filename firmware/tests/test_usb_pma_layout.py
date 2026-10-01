#!/usr/bin/env python3
"""Verify the single-buffer-only build matches every application PMA allocation."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PacketMemoryLayout(unittest.TestCase):
    def test_single_buffer_configuration(self):
        config = (ROOT / 'include/stm32g4xx_hal_conf.h').read_text()
        self.assertRegex(config, r'#define\s+USE_USB_DOUBLE_BUFFER\s+0U\b')
        self.assertLess(config.index('#define USE_USB_DOUBLE_BUFFER'),
                        config.index('#include'))

    def test_cdc_packet_regions(self):
        source = (ROOT / 'src/usb_transport.c').read_text()
        allocations = re.findall(
            r'HAL_PCDEx_PMAConfig\(&pcd,\s*([^,]+),\s*([^,]+),\s*(0x[0-9A-Fa-f]+)U\)',
            source)
        expected = {'0x00U': 0x40, '0x80U': 0x80, 'CDC_OUT_EP': 0xc0,
                    'CDC_IN_EP': 0x100, 'CDC_CMD_EP': 0x140}
        self.assertEqual(len(allocations), len(expected))
        seen = set()
        occupied = set(range(0x40))  # Eight 8-byte buffer-table entries.
        for endpoint, kind, offset in allocations:
            self.assertEqual(kind.strip(), 'PCD_SNG_BUF')
            endpoint = endpoint.strip()
            self.assertNotIn(endpoint, seen)
            seen.add(endpoint)
            start = int(offset, 16)
            self.assertEqual(start, expected[endpoint])
            self.assertEqual(start % 2, 0)  # G4 PMA halfword alignment.
            # Conservatively reserve 64 bytes even for the 8-byte notification EP.
            region = set(range(start, start + 64))
            self.assertFalse(region & occupied)
            self.assertLessEqual(start + 64, 1024)
            occupied |= region
        self.assertEqual(seen, set(expected))


if __name__ == '__main__':
    unittest.main()
