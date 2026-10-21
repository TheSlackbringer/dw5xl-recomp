"""Release lifecycle checks with fake GitHub state; no game data or network."""
import copy
import hashlib
from pathlib import Path
import tempfile
import unittest

from update_draft import GitHub, PublishedDuringUpdate, select_release, update_draft


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
                   **payload, 'draft': True, 'prerelease': True, 'assets': []}
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

    def test_first_build_creates_alpha_one(self):
        github = FakeGitHub([])
        self.assertEqual(self.update(github), 'v0.1.0-alpha.1')
        self.assertTrue(github.state[0]['draft'])
        self.assertTrue(github.state[0]['prerelease'])
        self.assertEqual(github.state[0]['target_commitish'], '123abcd')

    def test_unpublished_draft_keeps_its_number(self):
        github = FakeGitHub([release('v0.1.0-alpha.1', True)])
        self.assertEqual(self.update(github), 'v0.1.0-alpha.1')
        self.assertEqual(len(github.state), 1)

    def test_published_prerelease_is_untouched_and_next_draft_created(self):
        published = {**release('v0.1.0-alpha.1', False), 'prerelease': True}
        github = FakeGitHub([published])
        self.assertEqual(self.update(github), 'v0.1.0-alpha.2')
        self.assertEqual(github.state[0], published)
        self.assertEqual(github.uploads[0][0], 2)

    def test_old_published_windows_tag_does_not_block(self):
        published = release('windows-next', False)
        github = FakeGitHub([published])
        self.assertEqual(self.update(github), 'v0.1.0-alpha.1')
        self.assertEqual(github.state[0], published)

    def test_existing_rolling_draft_is_migrated(self):
        github = FakeGitHub([release('windows-next', True, 17)])
        self.assertEqual(self.update(github), 'v0.1.0-alpha.1')
        self.assertEqual(len(github.state), 1)
        self.assertEqual(github.state[0]['id'], 17)

    def test_version_order_is_numeric(self):
        items = [release('v0.1.0-alpha.9', False), release('v0.1.0-alpha.10', False, 2)]
        self.assertEqual(select_release(items), ('v0.1.0-alpha.11', None))

    def test_owner_publishing_before_update_selects_next_number(self):
        github = FakeGitHub([release('v0.1.0-alpha.1', True)])
        github.publish_on_edit = True
        self.assertEqual(self.update(github), 'v0.1.0-alpha.2')
        self.assertFalse(github.state[0]['draft'])
        self.assertEqual(github.uploads[0][0], 2)

    def test_only_old_package_assets_are_removed(self):
        current = release('v0.1.0-alpha.1', True)
        current['assets'] = [{'id': 10, 'name': 'DW5XL-old-windows-x64.zip'},
                             {'id': 11, 'name': 'DW5XL-v0.1.0-alpha.1-windows-x64.zip'},
                             {'id': 12, 'name': 'owner-notes.txt'}]
        github = FakeGitHub([current])
        self.update(github)
        self.assertEqual(github.deleted, [(1, 10)])

    def test_numbered_archive_has_matching_checksum(self):
        self.update(FakeGitHub([]))
        archive = Path(self.scratch.name) / 'DW5XL-v0.1.0-alpha.1-windows-x64.zip'
        digest, name = Path(str(archive) + '.sha256').read_text().split()
        self.assertEqual(digest, hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertEqual(name, archive.name)

    def test_real_client_refuses_to_edit_published_release(self):
        github = GitHub('test/repo')
        calls = []
        def api(path, **kwargs):
            calls.append((path, kwargs))
            return release('v0.1.0-alpha.1', False)
        github.api = api
        with self.assertRaises(PublishedDuringUpdate):
            github.edit(1, {'name': 'changed'})
        self.assertEqual(calls, [('releases/1', {})])


if __name__ == '__main__':
    unittest.main()
