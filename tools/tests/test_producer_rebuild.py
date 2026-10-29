"""042: full translator hashes must survive identical clean Windows rebuilds."""
import os
from pathlib import Path
import struct
import unittest


class ProducerRebuild(unittest.TestCase):
    def test_windows_producer_uses_reproducible_image_metadata(self):
        exe=Path(os.environ.get('PS2X_TEST_RECOMPILER','build-recomp/ps2xRecomp/ps2_recomp'))
        data=exe.read_bytes()
        if not data.startswith(b'MZ'):
            self.skipTest('PE metadata guard applies to Windows; full rebuilt-byte comparison is separate')
        pe=struct.unpack_from('<I',data,0x3c)[0]
        self.assertEqual(data[pe:pe+4],b'PE\0\0')
        timestamp=struct.unpack_from('<I',data,pe+8)[0]
        # MinGW can zero the timestamp. MSVC's /Brepro marks a content-derived timestamp with debug type 16.
        reproducible=False
        optional=pe+24
        directories=optional+(112 if struct.unpack_from('<H',data,optional)[0]==0x20b else 96)
        rva,size=struct.unpack_from('<II',data,directories+6*8)
        sections=optional+struct.unpack_from('<H',data,pe+20)[0]
        for i in range(struct.unpack_from('<H',data,pe+6)[0]):
            section=sections+i*40
            va,raw_size,raw=struct.unpack_from('<III',data,section+12)
            if rva and va<=rva and rva+size<=va+raw_size:
                start=raw+rva-va
                reproducible=any(struct.unpack_from('<I',data,at+12)[0]==16 for at in range(start,start+size,28))
        self.assertTrue(timestamp==0 or reproducible,
                        'a wall-clock PE timestamp makes unchanged translators invalidate generation after every CI rebuild')
