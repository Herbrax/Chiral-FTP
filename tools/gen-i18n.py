#!/usr/bin/env python3
"""Builds gen/i18n.c from i18n/translations.json, and checks the app's strings.

  tools/gen-i18n.py i18n/translations.json gen/i18n.c app/*.cpp engine/*.c ...
  tools/gen-i18n.py --missing i18n/translations.json app/*.cpp ...   (list what's missing)

The build fails when:
  - a tr("..."), trf("...") or N_("...") string in the sources has no entry,
  - an entry lacks a language, or
  - a translation's placeholders ({0}, {1}... or printf specifiers) differ
    from the English.

Arabic is stored ready to draw: the UI's text renderer neither joins Arabic
letters nor runs text right to left, so each translation becomes the
contextual letter forms (Unicode Arabic Presentation Forms-B) in visual
order.
"""

import json
import re
import sys

LANGS = ["en", "es", "fr", "it", "ja", "ko", "zh", "ru", "ar"]

# ---------- sources ----------

LITERAL = r'"((?:[^"\\]|\\.)*)"'
CALL = re.compile(r'\b(?:tr|trf|N_)\(\s*' + LITERAL)


def c_unescape(s):
    return bytes(s, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


def source_strings(paths):
    found = {}
    for p in paths:
        text = open(p, encoding="utf-8").read()
        for m in CALL.finditer(text):
            s = c_unescape(m.group(1))
            found.setdefault(s, f"{p}:{text.count(chr(10), 0, m.start()) + 1}")
    return found


# ---------- checks ----------

PLACEHOLDER = re.compile(r"\{\d\}|%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z|h)?[diouxXfFeEgGcsp%]")


def placeholders(s):
    marks = PLACEHOLDER.findall(s)
    braces = sorted(m for m in marks if m.startswith("{"))
    printf = [m for m in marks if m.startswith("%")]  # order matters for printf
    return braces, printf


# ---------- Arabic ----------

# isolated, final, initial, medial (None: the letter doesn't have that form)
FORMS = {
    0x0621: (0xFE80, None, None, None),
    0x0622: (0xFE81, 0xFE82, None, None),
    0x0623: (0xFE83, 0xFE84, None, None),
    0x0624: (0xFE85, 0xFE86, None, None),
    0x0625: (0xFE87, 0xFE88, None, None),
    0x0626: (0xFE89, 0xFE8A, 0xFE8B, 0xFE8C),
    0x0627: (0xFE8D, 0xFE8E, None, None),
    0x0628: (0xFE8F, 0xFE90, 0xFE91, 0xFE92),
    0x0629: (0xFE93, 0xFE94, None, None),
    0x062A: (0xFE95, 0xFE96, 0xFE97, 0xFE98),
    0x062B: (0xFE99, 0xFE9A, 0xFE9B, 0xFE9C),
    0x062C: (0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0),
    0x062D: (0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4),
    0x062E: (0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8),
    0x062F: (0xFEA9, 0xFEAA, None, None),
    0x0630: (0xFEAB, 0xFEAC, None, None),
    0x0631: (0xFEAD, 0xFEAE, None, None),
    0x0632: (0xFEAF, 0xFEB0, None, None),
    0x0633: (0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4),
    0x0634: (0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8),
    0x0635: (0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC),
    0x0636: (0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0),
    0x0637: (0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4),
    0x0638: (0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8),
    0x0639: (0xFEC9, 0xFECA, 0xFECB, 0xFECC),
    0x063A: (0xFECD, 0xFECE, 0xFECF, 0xFED0),
    0x0641: (0xFED1, 0xFED2, 0xFED3, 0xFED4),
    0x0642: (0xFED5, 0xFED6, 0xFED7, 0xFED8),
    0x0643: (0xFED9, 0xFEDA, 0xFEDB, 0xFEDC),
    0x0644: (0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0),
    0x0645: (0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4),
    0x0646: (0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8),
    0x0647: (0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC),
    0x0648: (0xFEED, 0xFEEE, None, None),
    0x0649: (0xFEEF, 0xFEF0, None, None),
    0x064A: (0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4),
}
TATWEEL = 0x0640
# lam + alef variants: (isolated, final)
LAM_ALEF = {0x0622: (0xFEF5, 0xFEF6), 0x0623: (0xFEF7, 0xFEF8), 0x0625: (0xFEF9, 0xFEFA), 0x0627: (0xFEFB, 0xFEFC)}


def transparent(c):
    return 0x064B <= c <= 0x065F or c == 0x0670


def joins_next(c):  # can connect to the letter after it
    return c == TATWEEL or (c in FORMS and FORMS[c][2] is not None)


def joins_prev(c):  # can connect to the letter before it
    return c == TATWEEL or (c in FORMS and FORMS[c][1] is not None)


def shape(text):
    cps = [ord(ch) for ch in text]
    out = []
    i = 0
    n = len(cps)

    def neighbour(start, step):
        j = start
        while 0 <= j < n and transparent(cps[j]):
            j += step
        return cps[j] if 0 <= j < n else None

    while i < n:
        c = cps[i]
        if c not in FORMS:
            out.append(c)
            i += 1
            continue
        prev = neighbour(i - 1, -1)
        connected_before = prev is not None and joins_next(prev) and joins_prev(c)
        nxt_i = i + 1
        while nxt_i < n and transparent(cps[nxt_i]):
            nxt_i += 1
        nxt = cps[nxt_i] if nxt_i < n else None
        if c == 0x0644 and nxt in LAM_ALEF:  # lam-alef ligature
            out.append(LAM_ALEF[nxt][1 if connected_before else 0])
            out.extend(cps[i + 1:nxt_i])
            i = nxt_i + 1
            continue
        connected_after = nxt is not None and joins_next(c) and joins_prev(nxt)
        iso, fin, ini, med = FORMS[c]
        if connected_before and connected_after:
            form = med
        elif connected_after:
            form = ini
        elif connected_before:
            form = fin
        else:
            form = iso
        out.append(form or iso)
        i += 1
    return "".join(chr(c) for c in out)


def is_rtl(ch):
    c = ord(ch)
    return 0x0600 <= c <= 0x06FF or 0xFB50 <= c <= 0xFDFF or 0xFE70 <= c <= 0xFEFF


def is_ltr(ch):
    return ch.isalnum() and not is_rtl(ch) or ch in "{}%"


MIRROR = {"(": ")", ")": "(", "[": "]", "]": "[", "<": ">", ">": "<", "«": "»", "»": "«"}


def visual(text):
    """Right-to-left paragraph to left-to-right drawing order: runs of Latin,
    digits and placeholders keep their order, everything else is reversed."""
    kinds = []
    for ch in text:
        kinds.append("R" if is_rtl(ch) else "L" if is_ltr(ch) else "N")
    # Neutrals between two L characters belong to that L run; others are R.
    resolved = kinds[:]
    for i, k in enumerate(kinds):
        if k != "N":
            continue
        left = next((kinds[j] for j in range(i - 1, -1, -1) if kinds[j] != "N"), "R")
        right = next((kinds[j] for j in range(i + 1, len(kinds)) if kinds[j] != "N"), "R")
        resolved[i] = "L" if left == "L" and right == "L" else "R"
    runs = []
    for ch, k in zip(text, resolved):
        if runs and runs[-1][0] == k:
            runs[-1][1].append(ch)
        else:
            runs.append([k, [ch]])
    out = []
    for k, chars in reversed(runs):
        out.append("".join(chars) if k == "L" else "".join(MIRROR.get(c, c) for c in reversed(chars)))
    return "".join(out)


def arabic_for_display(text):
    return visual(shape(text))


# ---------- output ----------


def c_string(s):
    out = []
    for b in s.encode("utf-8"):
        if b in (0x22, 0x5C):
            out.append("\\" + chr(b))
        elif 32 <= b < 127:
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def main():
    args = sys.argv[1:]
    missing_mode = args and args[0] == "--missing"
    if missing_mode:
        args = args[1:]
    catalog_path = args[0]
    out_path = None if missing_mode else args[1]
    sources = args[1:] if missing_mode else args[2:]

    catalog = json.load(open(catalog_path, encoding="utf-8"))
    names = catalog["languages"]
    entries = catalog["strings"]
    errors = []
    if list(names) != LANGS:
        errors.append("languages must be, in order: " + ", ".join(LANGS))
    by_en = {}
    for e in entries:
        en = e.get("en")
        if not en:
            errors.append("an entry has no English: %r" % e)
            continue
        if en in by_en:
            errors.append("duplicate entry: %r" % en)
        by_en[en] = e
        braces, printf = placeholders(en)
        for lang in LANGS[1:]:
            t = e.get(lang)
            if not t:
                errors.append("%r has no %s translation" % (en, lang))
                continue
            tb, tp = placeholders(t)
            if tb != braces or tp != printf:
                errors.append("%r in %s: placeholders %s differ from %s" % (en, lang, tb + tp, braces + printf))

    used = source_strings(sources)
    missing = [s for s in used if s not in by_en]
    if missing_mode:
        for s in sorted(missing):
            print(json.dumps(s, ensure_ascii=False))
        print("%d missing, %d unused" % (len(missing), len([s for s in by_en if s not in used])), file=sys.stderr)
        return
    for s in missing:
        errors.append("no translation entry for %r (%s)" % (s, used[s]))
    if errors:
        sys.exit("i18n: " + "\n      ".join(errors))

    lines = ["/* Generated by tools/gen-i18n.py from i18n/translations.json. Do not edit. */", ""]
    lines.append("const int chiral_i18n_languages = %d;" % len(LANGS))
    lines.append("const char *const chiral_i18n_codes[] = {%s};" % ", ".join(c_string(l) for l in LANGS))
    shown_names = [arabic_for_display(names[l]) if l == "ar" else names[l] for l in LANGS]
    lines.append("const char *const chiral_i18n_names[] = {%s};" % ", ".join(c_string(n) for n in shown_names))
    lines.append("const int chiral_i18n_strings = %d;" % len(entries))
    for i, e in enumerate(entries):
        cells = []
        for lang in LANGS:
            t = e[lang]
            cells.append(c_string(arabic_for_display(t) if lang == "ar" else t))
        lines.append("static const char *const row%d[] = {%s};" % (i, ", ".join(cells)))
    lines.append("const char *const *const chiral_i18n_rows[] = {%s};" %
                 ", ".join("row%d" % i for i in range(len(entries))))
    open(out_path, "w", encoding="utf-8").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
