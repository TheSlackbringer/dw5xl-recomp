"""Release lifecycle checks with fake GitHub state; no game data or network."""
import copy
import hashlib
from pathlib import Path
import tempfile
import unittest

from test_zsync_file import write_zsync
from update_draft import GitHub, PublishedDuringUpdate, package_for_version, select_release, update_draft
import zsync_file


def release(tag, draft, number=1):
    return {'id': number, 'tag_name': tag, 'draft': draft, 'assets': []}


class FakeGitHub:
    def __init__(self, releases):
        self.state = copy.deepcopy(releases)
        self.uploads = []
        self.deleted = []
        self.publish_on_edit = False

    def releases(self):
        return copy.deepcopy(self.state)

    def require_draft(self, release_id):
        current = next(item for item in self.state if item['id'] == release_id)
        if not current['draft']:
            raise PublishedDuringUpdate()
        return copy.deepcopy(current)

    def edit(self, release_id, payload):
        if self.publish_on_edit:
            self.publish_on_edit = False
            next(item for item in self.state if item['id'] == release_id)['draft'] = False
        self.require_draft(release_id)
        current = next(item for item in self.state if item['id'] == release_id)
        current.update(payload)
        return copy.deepcopy(current)

    def create(self, payload):
        current = {'id': max((item['id'] for item in self.state), default=0) + 1,
                   **payload, 'draft': True, 'prerelease': False, 'assets': []}
        self.state.append(current)
        return copy.deepcopy(current)

    def upload(self, release_id, files):
        self.require_draft(release_id)
        self.uploads.append((release_id, [path.name for path in files]))

    def delete_asset(self, release_id, asset_id):
        self.require_draft(release_id)
        self.deleted.append((release_id, asset_id))


class DraftLifecycleTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.source = Path(self.scratch.name) / 'DW5XL-123abcd-windows-x64.zip'
        self.source.write_bytes(b'synthetic builder package')

    def update(self, github):
        return update_draft(github, self.source, '123abcd', '42', 'Release fix\nsecond line')

    def linux_package(self):
        linux = Path(self.scratch.name) / 'DW5XL-RECOMP-123abcd-x86_64.AppImage'
        linux.write_bytes(b'synthetic Linux builder')
        write_zsync(Path(str(linux) + '.zsync'), linux.name, linux.read_bytes())
        return linux

    def test_first_build_creates_beta_one(self):
        github = FakeGitHub([])
        self.assertEqual(self.update(github), 'v0.1.0-beta.1')
        self.assertTrue(github.state[0]['draft'])
        self.assertFalse(github.state[0]['prerelease'])
        self.assertEqual(github.state[0]['target_commitish'], '123abcd')

    def test_unpublished_draft_keeps_its_number(self):
        github = FakeGitHub([release('v0.1.0-beta.1', True)])
        self.assertEqual(self.update(github), 'v0.1.0-beta.1')
        self.assertEqual(len(github.state), 1)

    def test_published_prerelease_is_untouched_and_next_draft_created(self):
        published = {**release('v0.1.0-beta.1', False), 'prerelease': True}
        github = FakeGitHub([published])
        self.assertEqual(self.update(github), 'v0.1.0-beta.2')
        self.assertEqual(github.state[0], published)
        self.assertEqual(github.uploads[0][0], 2)

    def test_old_published_windows_tag_does_not_block(self):
        published = release('windows-next', False)
        github = FakeGitHub([published])
        self.assertEqual(self.update(github), 'v0.1.0-beta.1')
        self.assertEqual(github.state[0], published)

    def test_existing_rolling_draft_is_migrated(self):
        github = FakeGitHub([release('windows-next', True, 17)])
        self.assertEqual(self.update(github), 'v0.1.0-beta.1')
        self.assertEqual(len(github.state), 1)
        self.assertEqual(github.state[0]['id'], 17)

    def test_version_order_is_numeric(self):
        items = [release('v0.1.0-beta.9', False), release('v0.1.0-beta.10', False, 2)]
        self.assertEqual(select_release(items), ('v0.1.0-beta.11', None))

    def test_owner_publishing_before_update_selects_next_number(self):
        github = FakeGitHub([release('v0.1.0-beta.1', True)])
        github.publish_on_edit = True
        self.assertEqual(self.update(github), 'v0.1.0-beta.2')
        self.assertFalse(github.state[0]['draft'])
        self.assertEqual(github.uploads[0][0], 2)

    def test_only_old_package_assets_are_removed(self):
        current = release('v0.1.0-beta.1', True)
        current['assets'] = [{'id': 10, 'name': 'DW5XL-old-windows-x64.zip'},
                             {'id': 11, 'name': 'DW5XL-v0.1.0-beta.1-windows-x64.zip'},
                             {'id': 12, 'name': 'owner-notes.txt'}]
        github = FakeGitHub([current])
        self.update(github)
        self.assertEqual(github.deleted, [(1, 10)])

    def test_numbered_archive_has_matching_checksum(self):
        self.update(FakeGitHub([]))
        archive = Path(self.scratch.name) / 'DW5XL-v0.1.0-beta.1-windows-x64.zip'
        digest, name = Path(str(archive) + '.sha256').read_text().split()
        self.assertEqual(digest, hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertEqual(name, archive.name)

    def test_real_client_refuses_to_edit_published_release(self):
        github = GitHub('test/repo')
        calls = []
        def api(path, **kwargs):
            calls.append((path, kwargs))
            return release('v0.1.0-beta.1', False)
        github.api = api
        with self.assertRaises(PublishedDuringUpdate):
            github.edit(1, {'name': 'changed'})
        self.assertEqual(calls, [('releases/1', {})])

    def test_windows_and_linux_packages_share_one_numbered_draft(self):
        linux = self.linux_package()
        github = FakeGitHub([])
        version = update_draft(github, [self.source, linux], '123abcd', '42', 'Linux builder')
        self.assertEqual(version, 'v0.1.0-beta.1')
        expected = {'DW5XL-v0.1.0-beta.1-windows-x64.zip',
                    'DW5XL-RECOMP-v0.1.0-beta.1-x86_64.AppImage'}
        self.assertEqual(set(github.uploads[0][1]), expected | {name + '.sha256' for name in expected} |
                         {'DW5XL-RECOMP-v0.1.0-beta.1-x86_64.AppImage.zsync'})
        for name in expected:
            archive = Path(self.scratch.name) / name
            digest, recorded_name = Path(str(archive) + '.sha256').read_text().split()
            self.assertEqual(digest, hashlib.sha256(archive.read_bytes()).hexdigest())
            self.assertEqual(recorded_name, name)

    def test_publication_advances_both_platforms_together(self):
        linux = self.linux_package()
        published = release('v0.1.0-beta.1', True)
        github = FakeGitHub([published])
        github.publish_on_edit = True
        version = update_draft(github, [self.source, linux], '123abcd', '42', 'Linux builder')
        self.assertEqual(version, 'v0.1.0-beta.2')
        self.assertTrue(all('beta.2' in name for name in github.uploads[0][1]))
        self.assertEqual(len(github.uploads[0][1]), 5)
        zsync_file.check(Path(self.scratch.name) / 'DW5XL-RECOMP-v0.1.0-beta.2-x86_64.AppImage.zsync',
                         Path(self.scratch.name) / 'DW5XL-RECOMP-v0.1.0-beta.2-x86_64.AppImage')

    def test_draft_renames_the_zsync_with_the_appimage(self):
        linux = self.linux_package()
        built_zsync = Path(str(linux) + '.zsync')
        checksums = built_zsync.read_bytes().split(b'\n\n', 1)[1]
        update_draft(FakeGitHub([]), [self.source, linux], '123abcd', '42', 'Linux builder')
        renamed = Path(self.scratch.name) / 'DW5XL-RECOMP-v0.1.0-beta.1-x86_64.AppImage'
        zsync = Path(str(renamed) + '.zsync')
        self.assertFalse(built_zsync.exists())
        header = zsync_file.read_header(zsync)
        self.assertEqual((header['Filename'], header['URL']), (renamed.name, renamed.name))
        zsync_file.check(zsync, renamed)
        self.assertEqual(zsync.read_bytes().split(b'\n\n', 1)[1], checksums)

    def test_linux_appimage_without_zsync_is_refused_before_any_change(self):
        linux = Path(self.scratch.name) / 'DW5XL-RECOMP-123abcd-x86_64.AppImage'
        linux.write_bytes(b'synthetic Linux builder')
        github = FakeGitHub([release('v0.1.0-beta.5', False)])
        with self.assertRaisesRegex(ValueError, r'\.zsync'):
            update_draft(github, [self.source, linux], '123abcd', '42', 'Linux builder')
        self.assertEqual((len(github.state), github.uploads), (1, []))
        self.assertTrue(linux.exists() and self.source.exists())

    def test_zsync_of_other_bytes_is_refused_before_any_change(self):
        linux = self.linux_package()
        linux.write_bytes(b'rebuilt Linux builder')
        github = FakeGitHub([])
        with self.assertRaises(ValueError):
            update_draft(github, [self.source, linux], '123abcd', '42', 'Linux builder')
        self.assertEqual((github.state, github.uploads), ([], []))

    def test_real_release_history_continues_after_beta_five(self):
        # The published releases on 2026-10-30 (issue 044): the next draft follows beta.5 instead of reusing alpha.4.
        items = [{**release(f'v0.1.0-beta.{n}', False, n), 'prerelease': n == 1} for n in range(1, 6)]
        items += [{**release(tag, False, number), 'prerelease': True}
                  for number, tag in ((6, 'v0.1.0-alpha.3'), (7, 'v0.1.0-alpha.2'), (8, 'windows-test-1'))]
        self.assertEqual(select_release(items), ('v0.1.0-beta.6', None))

    def test_leftover_alpha_draft_becomes_the_next_beta(self):
        github = FakeGitHub([release('v0.1.0-beta.5', False), release('v0.1.0-alpha.4', True, 9)])
        self.assertEqual(self.update(github), 'v0.1.0-beta.6')
        self.assertEqual(len(github.state), 2)
        self.assertEqual(github.state[1]['tag_name'], 'v0.1.0-beta.6')
        self.assertEqual(github.uploads[0][0], 9)

    def test_real_client_creates_drafts_as_full_releases(self):
        github = GitHub('test/repo')
        calls = []
        github.api = lambda path, **kwargs: calls.append((path, kwargs))
        github.create({'tag_name': 'v0.1.0-beta.6'})
        payload = {'tag_name': 'v0.1.0-beta.6', 'draft': True, 'prerelease': False}
        self.assertEqual(calls, [('releases', {'method': 'POST', 'payload': payload})])

    def test_old_linux_package_name_is_refused(self):
        old = Path(self.scratch.name) / 'DW5XL-123abcd-linux-x86_64.AppImage'
        old.write_bytes(b'synthetic Linux builder')
        with self.assertRaises(ValueError):
            package_for_version(old, 'v0.1.0-beta.1')


if __name__ == '__main__':
    unittest.main()
