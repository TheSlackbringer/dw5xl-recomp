"""042: producer identity follows compiled code, independent of Windows object timestamps and UI files."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class VuProducer(unittest.TestCase):
    def setUp(self):
        self.scratch=tempfile.TemporaryDirectory(prefix='vu-producer-')
        self.addCleanup(self.scratch.cleanup)
        self.root=Path(self.scratch.name)

    def identity(self,folder,payload,timestamp,*,big=False):
        folder.mkdir(parents=True,exist_ok=True)
        header=(struct.pack('<HHHHI',0,65535,2,0x8664,timestamp)+bytes(44) if big else
                struct.pack('<HHIIIHH',0x8664,1,timestamp,0,0,0,0))
        obj=folder/'generator.cpp.obj';obj.write_bytes(header+payload)
        listing=folder/'objects.txt';listing.write_text(obj.as_posix()+'\n')
        out=folder/'producer.h'
        run=subprocess.run([os.environ.get('PS2X_TEST_CMAKE','cmake'),'-DOBJECT_LIST='+str(listing),
            '-DOUT='+str(out),'-P',str(ROOT/'app/cmake/vu_producer.cmake')],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        return out.read_bytes()

    def test_coff_timestamp_and_build_path_do_not_change_identity(self):
        self.assertEqual(self.identity(self.root/'first',b'same generated instructions',123),
                         self.identity(self.root/'moved',b'same generated instructions',456))

    def test_bigobj_timestamp_is_metadata_too(self):
        self.assertEqual(self.identity(self.root/'first',b'same generated instructions',123,big=True),
                         self.identity(self.root/'moved',b'same generated instructions',456,big=True))

    def test_code_changes_invalidate_but_unrelated_ui_does_not(self):
        before=self.identity(self.root/'build',b'instructions from generator and headers',123)
        (self.root/'wizard_ui.cpp').write_text('changed UI only')
        (self.root/'BUILD_ID').write_text('new release')
        self.assertEqual(before,self.identity(self.root/'build',b'instructions from generator and headers',456))
        self.assertNotEqual(before,self.identity(self.root/'build',b'changed generator instructions or options',456))
