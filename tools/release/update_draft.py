#!/usr/bin/env python3
"""Update the numbered builder draft; publishing remains an owner action.

Keep updating alpha.N while it is a draft. Once published (including as a
prerelease), leave it alone and create alpha.N+1 on the next successful build.
The archive's BUILD_ID remains the source revision so updates still rebuild.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

NOTES_DIR = Path(__file__).with_name('notes')


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
        return self.api('releases', method='POST', payload={**payload, 'draft': True, 'prerelease': True})

    def upload(self, release_id, files):
        release = self.require_draft(release_id)
        subprocess.run(['gh', 'release', 'upload', release['tag_name'], *map(str, files),
                        '--repo', self.repo, '--clobber'], check=True)

    def delete_asset(self, release_id, asset_id):
        self.require_draft(release_id)
        self.api(f'releases/assets/{asset_id}', method='DELETE')


def select_release(releases, series='v0.1.0-alpha'):
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
    # Migrate the old rolling draft, preserving its identity and owner comments.
    legacy = next((release for release in releases
                   if release['tag_name'] == 'windows-next' and release['draft']), None)
    return f'{series}.{number}', legacy


def package_for_version(source, version):
    suffix = next((suffix for suffix in ('windows-x64.zip', 'linux-x86_64.AppImage')
                   if source.name.endswith('-' + suffix)), None)
    if suffix is None:
        raise ValueError(f'unknown builder package: {source.name}')
    target = source.with_name(f'DW5XL-{version}-{suffix}')
    if target != source:
        source.rename(target)
    checksum = Path(str(target) + '.sha256')
    with target.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    checksum.write_text(f'{digest}  {target.name}\n')
    return target, checksum


def release_body(version, commit, run_id, subject, platforms):
    metadata = (f'Built by GitHub Actions from {commit} (run {run_id}) after every test job passed.\n\n'
                f'{platforms} builder packages: launcher, compiler, sources and tools. '
                'The first start builds the game from your own disc.\n\n'
                f'Latest commit: {subject.splitlines()[0] if subject else ""}\n\n'
                'Draft packages are updated by CI until the owner publishes this version.')
    notes = NOTES_DIR / f'{version}.md'
    return notes.read_text().rstrip() + '\n\n---\n\n' + metadata if notes.is_file() else metadata


def update_draft(github, source, commit, run_id, subject, series='v0.1.0-alpha'):
    sources = [source] if isinstance(source, Path) else list(source)
    platforms = 'Windows and Linux' if len(sources) > 1 else 'Windows'
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
    parser.add_argument('--series', default='v0.1.0-alpha')
    args = parser.parse_args()
    windows = list(args.dist.glob('DW5XL-*-windows-x64.zip'))
    linux = list(args.dist.glob('DW5XL-*-linux-x86_64.AppImage'))
    if not args.repo or not args.commit or len(windows) != 1 or len(linux) > 1:
        parser.error('repo, commit, exactly one Windows zip and at most one Linux AppImage are required')
    update_draft(GitHub(args.repo), windows + linux, args.commit, args.run_id,
                 os.environ.get('COMMIT_MESSAGE', ''), args.series)


if __name__ == '__main__':
    main()
