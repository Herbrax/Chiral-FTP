#!/usr/bin/env python3
"""Checks that every platform implements both contracts.

Three functions went missing when the tree was unified -- early_init,
startup_notice and report_startup_services existed for two platforms and
not the third -- and nothing noticed until a link failed. This notices,
without needing that platform's toolchain.

    python3 tools/check-platforms.py
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# One name per line of the contract we expect to find defined.
BACKEND = ['init', 'shutdown', 'pump', 'present', 'frame_begin', 'frame_end',
           'ticks_ms', 'touch', 'early_init', 'has_native_keyboard',
           'keyboard_failed', 'native_text_prompt', 'set_text_input',
           'can_quit', 'startup_notice', 'report_startup_services',
           'about_file_manager', 'about_thanks', 'window_size', 'pointer_hand', 'open_url', 'open_file', 'copy_text', 'choose_folder', 'in_foreground']
SYSTEM = ['system_data_dir_candidates', 'system_storage_roots',
          'system_free_space', 'system_local_ip', 'system_notify',
          'system_timegm', 'system_language', 'system_renderable_languages',
          'system_script_fonts', 'system_ftpd_default_root', 'system_ftpd_full_root', 'system_networks', 'system_broadcasts',
          'system_device_name', 'system_device_kind']
TRAITS = ['CHIRAL_FILE_MODE', 'CHIRAL_RENDER_W', 'CHIRAL_RENDER_H',
          'CHIRAL_WEB_MHD_OWNS_LISTENER', 'CHIRAL_WEB_MHD_NEEDS_SOCKETPAIR',
          'CHIRAL_BIG_ALLOC_VIA_MMAP', 'CHIRAL_PLATFORM_ID', 'CHIRAL_BUTTONS_PLAYSTATION',
          'CHIRAL_BUTTONS_KEYBOARD', 'CHIRAL_DEVICE_COMPUTER', 'CHIRAL_HAS_SFTPD', 'CHIRAL_UI_DESKTOP',
          'CHIRAL_UI_MOBILE', 'CHIRAL_PLATFORM_NAME']

def defined(path, name):
    try:
        text = open(path, encoding='utf8').read()
    except OSError:
        return False
    # a definition, not a call: the name followed by ( at the start of a
    # line, or after a return type on the same line
    return re.search(r'(^|[\s*])' + re.escape(name) + r'\s*\([^;]*\)\s*\{',
                     text, re.M) is not None

def has_macro(path, name):
    try:
        text = open(path, encoding='utf8').read()
    except OSError:
        return False
    return re.search(r'^\s*#\s*define\s+' + re.escape(name) + r'\b', text, re.M) is not None

bad = 0
platforms = sorted(d for d in os.listdir(os.path.join(ROOT, 'platform'))
                   if os.path.isdir(os.path.join(ROOT, 'platform', d)))
for p in platforms:
    base = os.path.join(ROOT, 'platform', p)
    missing = []
    missing += ['backend.cpp: ' + f for f in BACKEND
                if not defined(os.path.join(base, 'backend.cpp'), f)]
    missing += ['system.c: ' + f for f in SYSTEM
                if not defined(os.path.join(base, 'system.c'), f)]
    missing += ['platform_config.h: ' + t for t in TRAITS
                if not has_macro(os.path.join(base, 'platform_config.h'), t)]
    for f in ('platform.mk', 'backend.cpp', 'system.c', 'platform_config.h'):
        if not os.path.exists(os.path.join(base, f)):
            missing.append('missing file: ' + f)
    if missing:
        bad += 1
        print('%s:' % p)
        for m in missing:
            print('   ' + m)
    else:
        print('%s: complete' % p)

# And the rule the whole arrangement rests on.
leaked = []
for sub in ('engine', 'web', 'app'):
    for dirpath, _, names in os.walk(os.path.join(ROOT, sub)):
        for n in names:
            if not n.endswith(('.c', '.h', '.cpp')):
                continue
            path = os.path.join(dirpath, n)
            text = open(path, encoding='utf8').read()
            for m in re.finditer(r'__(SWITCH|PROSPERO|SCE)__', text):
                leaked.append('%s: %s' % (os.path.relpath(path, ROOT), m.group(0)))
if leaked:
    bad += 1
    print('\nplatform checks leaked into shared code:')
    for l in leaked:
        print('   ' + l)
else:
    print('\nshared code names no platform')

sys.exit(1 if bad else 0)
