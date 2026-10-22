#!/usr/bin/env python3
"""Wraps the desktop build in dist/chiral-ftp.app, a double-clickable macOS
application that runs without Homebrew installed.

    make PLATFORM=sdl app
    python3 tools/make-macos-app.py <binary> <version> [out.app]

The binary links SDL2 and libmicrohttpd from Homebrew, and those pull in a
dozen more (gnutls and its crypto); every one is copied into
Contents/Frameworks and relinked to load from there. SDL2 is sdl2-compat,
which loads SDL3 at run time from @loader_path/libSDL3.dylib -- no load
command names it, so it is added by hand.

Signed ad hoc: enough for Apple silicon to run it on this Mac. Another Mac
will want it notarised, or a right-click > Open the first time.

The settings live in ~/Library/Application Support/chiral-ftp when started
from the bundle (platform/sdl/system.c), never inside it.
"""
import os, plistlib, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUNDLE_ID = 'io.github.herbrax.chiral-ftp'
# Libraries the OS provides; anything else is bundled.
SYSTEM_PREFIXES = ('/usr/lib/', '/System/')


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def linked(path):
    """The dylibs a Mach-O file loads, as written in its load commands."""
    lines = run('otool', '-L', path).splitlines()[1:]
    deps = [l.strip().split(' (')[0] for l in lines]
    # A dylib lists its own install name first; that isn't a dependency.
    own = run('otool', '-D', path).splitlines()[1:]
    return [d for d in deps if d not in own]


def minos(path):
    """The lowest macOS the file says it runs on, as (major, minor)."""
    m = re.search(r'minos (\d+)\.(\d+)', run('otool', '-l', path))
    return (int(m.group(1)), int(m.group(2))) if m else (11, 0)


def collect(binary, extra):
    """Every non-system dylib reachable from the binary (and from extra),
    keyed by real path, each with the file name it will have in the bundle."""
    found = {}  # real path -> bundled name
    todo = [binary] + list(extra.values())
    for real, name in ((os.path.realpath(p), n) for n, p in extra.items()):
        found[real] = name
    while todo:
        for dep in linked(todo.pop()):
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            if dep.startswith('@'):
                sys.exit('unexpected relative load path %s; bundle it by hand' % dep)
            real = os.path.realpath(dep)
            if real not in found:
                found[real] = os.path.basename(dep)
                todo.append(real)
    return found


def relink(path, found, prefix):
    """Points each bundled dependency of path at its copy in Frameworks."""
    args = []
    for dep in linked(path):
        real = os.path.realpath(dep) if not dep.startswith('@') else None
        if real in found:
            args += ['-change', dep, prefix + found[real]]
    if args:
        run('install_name_tool', *args, path)


def make_icon(out_icns):
    """Contents/Resources/chiral-ftp.icns from the brand's app icon SVG,
    drawn at 824 of 1024 like every other macOS icon (the grid leaves a
    margin for the shadow)."""
    svg = open(os.path.join(ROOT, 'assets/brand/chiral-ftp-icon.svg'), encoding='utf8').read()
    inner = re.sub(r'^\s*<svg[^>]*>|</svg>\s*$', '', svg)
    framed = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
              'viewBox="0 0 1024 1024" width="1024" height="1024">'
              '<g transform="translate(100 100) scale(%g)">%s</g></svg>' % (824 / 512, inner))
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'icon.svg')
        open(src, 'w', encoding='utf8').write(framed)
        master = os.path.join(tmp, 'icon.png')
        run('sips', '-s', 'format', 'png', src, '--out', master)
        iconset = os.path.join(tmp, 'chiral-ftp.iconset')
        os.mkdir(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = 'icon_%dx%d%s.png' % (size, size, '@2x' if scale == 2 else '')
                run('sips', '-z', str(px), str(px), master, '--out', os.path.join(iconset, name))
        run('iconutil', '-c', 'icns', iconset, '-o', out_icns)


def info_plist(version, minimum):
    return {
        'CFBundleName': 'chiral-ftp',
        'CFBundleDisplayName': 'chiral-ftp',
        'CFBundleIdentifier': BUNDLE_ID,
        'CFBundleExecutable': 'chiral-ftp',
        'CFBundleIconFile': 'chiral-ftp',
        'CFBundlePackageType': 'APPL',
        'CFBundleSignature': '????',
        'CFBundleShortVersionString': version,
        'CFBundleVersion': version,
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleDevelopmentRegion': 'en',
        'LSMinimumSystemVersion': '%d.%d' % minimum,
        'LSApplicationCategoryType': 'public.app-category.utilities',
        'NSHighResolutionCapable': True,
        'NSSupportsAutomaticGraphicsSwitching': True,
        'NSHumanReadableCopyright': 'chiral-ftp by Herbrax (Simo Hakim). GPLv3.',
        # What macOS shows when it asks; without these the prompts are blank.
        'NSLocalNetworkUsageDescription':
            'chiral-ftp connects to FTP and SFTP servers and consoles on your network, '
            'and serves its FTP server and web app to them.',
        'NSDocumentsFolderUsageDescription': 'chiral-ftp transfers files to and from your Documents folder.',
        'NSDesktopFolderUsageDescription': 'chiral-ftp transfers files to and from your Desktop.',
        'NSDownloadsFolderUsageDescription': 'chiral-ftp transfers files to and from your Downloads folder.',
        'NSRemovableVolumesUsageDescription': 'chiral-ftp transfers files to and from removable drives.',
        'NSNetworkVolumesUsageDescription': 'chiral-ftp transfers files to and from network volumes.',
    }


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: make-macos-app.py <binary> <version> [out.app]')
    binary, version = sys.argv[1], sys.argv[2]
    app = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, 'dist', 'chiral-ftp.app')

    contents = os.path.join(app, 'Contents')
    macos = os.path.join(contents, 'MacOS')
    frameworks = os.path.join(contents, 'Frameworks')
    resources = os.path.join(contents, 'Resources')
    shutil.rmtree(app, ignore_errors=True)
    for d in (macos, frameworks, resources):
        os.makedirs(d)

    # sdl2-compat's run-time dependency, found beside it.
    extra = {}
    sdl3 = run('brew', '--prefix', 'sdl3').strip() + '/lib/libSDL3.0.dylib'
    if any('sdl2-compat' in os.path.realpath(d) for d in linked(binary)):
        if not os.path.exists(sdl3):
            sys.exit('libSDL2 is sdl2-compat, which needs SDL3: brew install sdl3')
        extra['libSDL3.dylib'] = sdl3

    exe = os.path.join(macos, 'chiral-ftp')
    shutil.copy2(binary, exe)
    found = collect(binary, extra)

    minimum = minos(binary)
    for real, name in sorted(found.items(), key=lambda kv: kv[1]):
        dst = os.path.join(frameworks, name)
        shutil.copy2(real, dst)
        os.chmod(dst, 0o755)
        run('install_name_tool', '-id', '@rpath/' + name, dst)
        relink(dst, found, '@loader_path/')
        minimum = max(minimum, minos(real))
        print('  bundled %s' % name)
    relink(exe, found, '@executable_path/../Frameworks/')

    make_icon(os.path.join(resources, 'chiral-ftp.icns'))
    with open(os.path.join(contents, 'Info.plist'), 'wb') as f:
        plistlib.dump(info_plist(version, minimum), f)
    with open(os.path.join(contents, 'PkgInfo'), 'w') as f:
        f.write('APPL????')
    shutil.copy2(os.path.join(ROOT, 'LICENSE'), resources)
    # The text renderer's licence, which asks to travel with it (FreeType
    # is linked from Homebrew, like the libraries bundled above).
    freetype = run('brew', '--prefix', 'freetype').strip() + '/LICENSE.TXT'
    if os.path.exists(freetype):
        shutil.copy2(freetype, os.path.join(resources, 'FreeType-LICENSE.txt'))

    # install_name_tool broke every signature; Apple silicon refuses to load
    # unsigned code, so sign inside out: the libraries, then the bundle.
    for name in sorted(os.listdir(frameworks)):
        run('codesign', '--force', '--sign', '-', os.path.join(frameworks, name))
    run('codesign', '--force', '--sign', '-', '--identifier', BUNDLE_ID, app)
    run('codesign', '--verify', '--strict', app)

    # Nothing may still point at Homebrew.
    stray = [(f, d) for f in [exe] + [os.path.join(frameworks, n) for n in os.listdir(frameworks)]
             for d in linked(f) if not d.startswith(SYSTEM_PREFIXES) and not d.startswith('@')]
    if stray:
        sys.exit('still linked outside the bundle: %s' % stray)

    print('built %s (macOS %d.%d or later)' % (os.path.relpath(app, os.getcwd()), *minimum))


if __name__ == '__main__':
    main()
