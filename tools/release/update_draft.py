#!/usr/bin/env python3
"""Update the numbered builder draft; publishing remains an owner action.

Keep updating beta.N while it is a draft. Once published (including as a
prerelease), leave it alone and create beta.N+1 on the next successful build.
Drafts are full releases, so AppImage updaters (which follow the latest full
release) see them once published; marking one as a prerelease is the owner's choice.
The AppImage's .zsync names the AppImage, so it is renamed and rewritten with it.
The archive's BUILD_ID remains the source revision so updates still rebuild.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

import zsync_file

NOTES_DIR = Path(__file__).with_name('notes')
SERIES = 'v0.1.0-beta'
RETIRED_SERIES = ('v0.1.0-alpha',)  # an unpublished draft of these becomes the next draft of SERIES
# Package file names per platform: prefix + version + suffix (issue 044: the AppImage names the project, not Linux),
# and whether AppImage updaters need a .zsync next to it.
PACKAGES = (('DW5XL-', '-windows-x64.zip', False), ('DW5XL-RECOMP-', '-x86_64.AppImage', True))


class PublishedDuringUpdate(Exception):
    pass


class GitHub:
    def __init__(self, repo):
        self.repo = repo

    def api(self, path, *, method='GET', payload=None, paginate=False):
        command = ['gh', 'api', '--method', method]
        if paginate:
            command += ['--paginate', '--slurp']
        command.append(f'repos/{self.repo}/{path}')
        if payload is not None:
            command += ['--input', '-']
        result = subprocess.run(command, input=json.dumps(payload) if payload is not None else None,
                                capture_output=True, text=True, check=True)
        return json.loads(result.stdout) if result.stdout.strip() else None

    def releases(self):
        return [release for page in self.api('releases?per_page=100', paginate=True) for release in page]

    def require_draft(self, release_id):
        release = self.api(f'releases/{release_id}')
        if not release['draft']:
            raise PublishedDuringUpdate()
        return release

    def edit(self, release_id, payload):
        self.require_draft(release_id)
        # Never set draft=true on an existing release: publication belongs to the owner.
        return self.api(f'releases/{release_id}', method='PATCH', payload=payload)

    def create(self, payload):
        return self.api('releases', method='POST', payload={**payload, 'draft': True, 'prerelease': False})

    def upload(self, release_id, files):
        release = self.require_draft(release_id)
        subprocess.run(['gh', 'release', 'upload', release['tag_name'], *map(str, files),
                        '--repo', self.repo, '--clobber'], check=True)

    def delete_asset(self, release_id, asset_id):
        self.require_draft(release_id)
        self.api(f'releases/assets/{asset_id}', method='DELETE')


def select_release(releases, series=SERIES):
    pattern = re.compile(re.escape(series) + r'\.([1-9][0-9]*)\Z')
    numbered = [(int(match[1]), release) for release in releases
                if (match := pattern.fullmatch(release['tag_name']))]
    if numbered:
        number, latest = max(numbered, key=lambda item: item[0])
        if latest['draft']:
            return latest['tag_name'], latest
        number += 1
    else:
        number = 1
    # Migrate the old rolling draft or a draft of a retired series, preserving its identity and owner comments.
    retired = re.compile('(' + '|'.join(map(re.escape, RETIRED_SERIES)) + r')\.[1-9][0-9]*\Z')
    legacy = next((release for release in releases if release['draft'] and
                   (release['tag_name'] == 'windows-next' or retired.fullmatch(release['tag_name']))), None)
    return f'{series}.{number}', legacy


def package_kind(source):
    kind = next((kind for kind in PACKAGES if source.name.startswith(kind[0]) and source.name.endswith(kind[1])), None)
    if kind is None:
        raise ValueError(f'unknown builder package: {source.name}')
    return kind


def update_file(source):
    """The checked .zsync of an AppImage, or None for a package that has none; ValueError if it is missing or wrong."""
    if not package_kind(source)[2]:
        return None
    zsync = Path(str(source) + '.zsync')
    if not zsync.is_file():
        raise ValueError(f'{source.name}: {zsync.name} is missing; AppImage updaters need it')
    zsync_file.check(zsync, source)
    return zsync


def package_for_version(source, version):
    prefix, suffix, _ = package_kind(source)
    zsync = update_file(source)
    target = source.with_name(f'{prefix}{version}{suffix}')
    if target != source:
        source.rename(target)
    checksum = Path(str(target) + '.sha256')
    with target.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    checksum.write_text(f'{digest}  {target.name}\n')
    return (target, checksum) if zsync is None else (target, checksum, zsync_file.rename(zsync, target))


def release_body(version, commit, run_id, subject, platforms):
    metadata = (f'Built by GitHub Actions from {commit} (run {run_id}) after every test job passed.\n\n'
                f'{platforms} builder packages: launcher, compiler, sources and tools. '
                'The first start builds the game from your own disc.\n\n'
                f'Latest commit: {subject.splitlines()[0] if subject else ""}\n\n'
                'Draft packages are updated by CI until the owner publishes this version.')
    notes = NOTES_DIR / f'{version}.md'
    return notes.read_text().rstrip() + '\n\n---\n\n' + metadata if notes.is_file() else metadata


def update_draft(github, source, commit, run_id, subject, series=SERIES):
    sources = [source] if isinstance(source, Path) else list(source)
    platforms = 'Windows and Linux' if len(sources) > 1 else 'Windows'
    for package in sources:
        update_file(package)  # refuse before the draft changes
    for _ in range(3):
        version, release = select_release(github.releases(), series)
        payload = {
            'tag_name': version,
            'target_commitish': commit,
            'name': f'DW5XL {version}',
            'body': release_body(version, commit, run_id, subject, platforms),
        }
        try:
            if release is None:
                release = github.create(payload)
            else:
                release = github.edit(release['id'], payload)
            packages = [package_for_version(source, version) for source in sources]
            sources = [package[0] for package in packages]  # A publication retry keeps the same bytes.
            files = [path for package in packages for path in package]
            github.upload(release['id'], files)
            keep = {path.name for path in files}
            for asset in github.require_draft(release['id']).get('assets', []):
                if asset['name'].startswith('DW5XL-') and asset['name'] not in keep:
                    github.delete_asset(release['id'], asset['id'])
            print(f'Updated draft {version} from {commit}')
            return version
        except PublishedDuringUpdate:
            print('Owner published the selected draft; selecting the next version.')
    raise RuntimeError('Release changed repeatedly during the upload; rerun the draft job.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dist', type=Path, default=Path('dist'))
    parser.add_argument('--repo', default=os.environ.get('GH_REPO'))
    parser.add_argument('--commit', default=os.environ.get('GITHUB_SHA'))
    parser.add_argument('--run-id', default=os.environ.get('GITHUB_RUN_ID', 'local'))
    parser.add_argument('--series', default=SERIES)
    args = parser.parse_args()
    windows, linux = (list(args.dist.glob(f'{prefix}*{suffix}')) for prefix, suffix, _ in PACKAGES)
    if not args.repo or not args.commit or len(windows) != 1 or len(linux) > 1:
        parser.error('repo, commit, exactly one Windows zip and at most one Linux AppImage are required')
    update_draft(GitHub(args.repo), windows + linux, args.commit, args.run_id,
                 os.environ.get('COMMIT_MESSAGE', ''), args.series)


if __name__ == '__main__':
    main()
