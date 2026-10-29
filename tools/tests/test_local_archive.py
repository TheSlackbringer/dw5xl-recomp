"""042: stable local archive paths invalidate by content and refresh affected header/library timestamps."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[2]


class LocalArchive(unittest.TestCase):
    def setUp(self):
        self.scratch=tempfile.TemporaryDirectory(prefix='local-archive-')
        self.addCleanup(self.scratch.cleanup)
        self.root=Path(self.scratch.name)
        self.source=self.root/'source';self.source.mkdir()
        self.build=self.root/'build';self.archive=self.root/'same-name.zip'
        self.write_archive('first header')
        helper=(ROOT/'external/PS2Recomp/cmake/LocalArchive.cmake').as_posix()
        # An old checkout's fallback reproduces its exact current ExternalProject extraction policy.
        (self.source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(ArchiveUpdate NONE)\n'
            'include(ExternalProject)\n'+f'if(EXISTS "{helper}")\ninclude("{helper}")\n'+
            f'ps2x_local_archive_options("{self.archive.as_posix()}" archive_options)\nelse()\n'+
            'set(archive_options DOWNLOAD_EXTRACT_TIMESTAMP TRUE)\nendif()\n'+
            f'ExternalProject_Add(archive URL "{self.archive.as_posix()}" SOURCE_DIR "{self.build.as_posix()}/unpacked" '+
            f'PREFIX "{self.build.as_posix()}/prefix" ${{archive_options}} CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND "")\n')

    def write_archive(self,text):
        with zipfile.ZipFile(self.archive,'w') as archive:
            entry=zipfile.ZipInfo('header.txt',date_time=(2000,1,1,0,0,0))
            archive.writestr(entry,text)

    def configure_build(self):
        cmake=os.environ.get('PS2X_TEST_CMAKE','cmake')
        for args in ([cmake,'-S',str(self.source),'-B',str(self.build),'-G','Ninja'],
                     [cmake,'--build',str(self.build)]):
            run=subprocess.run(args,capture_output=True,text=True,timeout=60)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        return self.build/'unpacked/header.txt'

    def test_changed_archive_same_path_and_mtime_updates_contents(self):
        header=self.configure_build();self.assertEqual(header.read_text(),'first header')
        old=self.archive.stat()
        self.write_archive('second header');os.utime(self.archive,ns=(old.st_atime_ns,old.st_mtime_ns))
        self.assertEqual(self.archive.stat().st_mtime_ns,old.st_mtime_ns)
        self.configure_build()
        self.assertEqual(header.read_text(),'second header','same-path archive replacement must invalidate extraction')

    def test_extracted_headers_are_newer_than_archive_dates(self):
        header=self.configure_build()
        self.assertGreater(header.stat().st_mtime,946684800,
                           'preserved old archive dates can keep existing C++ objects falsely up to date')

    def test_unchanged_archive_preserves_extracted_output(self):
        header=self.configure_build();before=header.stat().st_mtime_ns
        self.configure_build()
        self.assertEqual(header.stat().st_mtime_ns,before,'unchanged SDK must not extract/rebuild unnecessarily')
