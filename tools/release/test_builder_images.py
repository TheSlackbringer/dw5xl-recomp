#!/usr/bin/env python3
"""The release builder images (issues 041, 027 M11): both are pinned by digest, the windows-build job runs in the
same Windows image as tools/release/windows-builder-image, and the release jobs download nothing themselves.
build.yml is read as text (CI's test job has no YAML library)."""
import re, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
DIGEST = re.compile(r'^ghcr\.io/[a-z0-9-]+/dw5xl-release-(linux|windows)@sha256:[0-9a-f]{64}$')


def job_text(name):
    """The lines of one job of build.yml (from its key to the next job key)."""
    text = (REPO / '.github/workflows/build.yml').read_text()
    jobs = re.split(r'^  (?=[a-z][a-z-]*:\s*$)', text.split('\njobs:\n', 1)[1], flags=re.M)
    found = [j for j in jobs if j.startswith(name + ':')]
    if not found:
        raise AssertionError(f'no job {name} in build.yml')
    return found[0]


class BuilderImages(unittest.TestCase):
    def test_images_pinned_by_digest(self):
        for name in ('linux-builder-image', 'windows-builder-image'):
            self.assertRegex((HERE / name).read_text().strip(), DIGEST, name)

    def test_windows_job_runs_in_the_pinned_image(self):
        image = re.search(r'^\s+image:\s*(\S+)\s*$', job_text('windows-build'), re.M)
        self.assertIsNotNone(image, 'windows-build has no container image')
        self.assertEqual(image.group(1), (HERE / 'windows-builder-image').read_text().strip())

    def test_release_jobs_download_nothing(self):
        for job in ('windows-build',):   # linux-package: once its inputs come from its image (027 M11, owner's choice)
            text = job_text(job)
            for tool in ('curl ', 'wget ', 'urlretrieve', 'urllib', 'pacman '):
                self.assertNotIn(tool, text, job)


if __name__ == '__main__':
    unittest.main()
