#!/usr/bin/env python3
"""Writes latest_version.json: the newest download for each platform, for the
website. Run by .github/workflows/latest-version.yml.

    python3 tools/latest-version.py <out.json>

Each platform takes its file from the newest release that has one, so one
that missed a release keeps its older download, and one nobody has built yet
is null. Drafts and prereleases don't count: a release being built is a
prerelease until a person publishes it (tools/ci-attach-release.sh).

The time stays the same while nothing else changes, so that the workflow has
nothing to commit. GITHUB_REPOSITORY picks the repository; GITHUB_TOKEN, when
set, is used for GitHub's API.
"""
import datetime, fnmatch, json, os, sys, urllib.request

REPOSITORY = os.environ.get('GITHUB_REPOSITORY', 'Herbrax/Chiral-FTP')

# The names are the download page's (data-asset). In order of preference: the
# first pattern any file of a release matches is that release's answer.
PLATFORMS = {
    'ps5': ['chiral-ftp-ps5-*.zip'],
    'switch': ['chiral-ftp.nro'],
    'switch2': [],  # no such build
    'android': ['*.apk'],
    'mac': ['*.dmg', '*.app.zip'],  # Apple silicon: there is no Intel build
    'windows-x64': ['*-windows-x64.exe'],
    # Windows on ARM runs the x64 program until there is an ARM one.
    'windows-arm64': ['*-windows-arm64.exe', '*-windows-x64.exe'],
    'linux-x86_64': ['*-x86_64.AppImage'],
    'linux-aarch64': ['*-aarch64.AppImage'],
}


def api(path):
    headers = {'Accept': 'application/vnd.github+json', 'User-Agent': 'chiral-ftp-latest-version'}
    if os.environ.get('GITHUB_TOKEN'):
        headers['Authorization'] = 'Bearer ' + os.environ['GITHUB_TOKEN']
    request = urllib.request.Request('https://api.github.com' + path, headers=headers)
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def published_releases():
    """Newest first."""
    releases, page = [], 1
    while True:
        batch = api('/repos/%s/releases?per_page=100&page=%d' % (REPOSITORY, page))
        releases += batch
        if len(batch) < 100:
            break
        page += 1
    releases = [r for r in releases if not r['draft'] and not r['prerelease']]
    return sorted(releases, key=lambda r: r['published_at'], reverse=True)


def newest(releases, patterns):
    for release in releases:
        for pattern in patterns:
            for asset in release['assets']:
                if fnmatch.fnmatchcase(asset['name'], pattern):
                    return {
                        'version': release['tag_name'].lstrip('v'),
                        'tag': release['tag_name'],
                        'published_at': release['published_at'],
                        'file': asset['name'],
                        'size': asset['size'],
                        'url': asset['browser_download_url'],
                    }
    return None


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: latest-version.py <out.json>')
    path = sys.argv[1]
    releases = published_releases()
    latest = releases[0]['tag_name'] if releases else None
    out = {
        'generated_at': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'latest_version': latest.lstrip('v') if latest else None,
        'latest_tag': latest,
        'platforms': {name: newest(releases, patterns) for name, patterns in PLATFORMS.items()},
    }
    try:
        with open(path, encoding='utf8') as f:
            before = json.load(f)
        if {k: v for k, v in before.items() if k != 'generated_at'} == \
           {k: v for k, v in out.items() if k != 'generated_at'}:
            print('%s: nothing has changed' % path)
            return
    except (OSError, ValueError):
        pass  # the first time, or not JSON: write it
    with open(path, 'w', encoding='utf8', newline='\n') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print('%s: written, latest %s' % (path, out['latest_version']))


if __name__ == '__main__':
    main()
