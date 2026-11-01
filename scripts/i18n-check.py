#!/usr/bin/env python3
"""Check the translations in src/shell/assets/lang/*.po against the source code.

  python3 scripts/i18n-check.py            check all languages
  python3 scripts/i18n-check.py --pot      also write src/shell/assets/lang/notatv.pot (template for a new language)

Finds every translatable text: T("...") / N_("...") in C (src/shell, src/moon), T("...") in the mpv OSD
script, data-t/data-th/data-tp in the phone page, and the English messages of the helper scripts.
Reports texts missing from a .po file, entries no longer used, and translations whose printf
placeholders (%s, %d, %1$s ...) do not match the English text. Exit code 1 if anything is missing or wrong."""
import glob, html, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, 'src/shell/assets/lang')
OVERLAY = os.path.join(ROOT, 'external/board')


def c_unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def source_ids():
    ids = {}
    lit = r'"((?:[^"\\\n]|\\.)*)"'
    for f in glob.glob(os.path.join(ROOT, 'src/shell/*.c')) + glob.glob(os.path.join(ROOT, 'src/moon/*.c')):
        code = re.sub(r'/\*.*?\*/', '', open(f, encoding='utf-8').read(), flags=re.S)
        for m in re.finditer(r'(?<![A-Za-z0-9_])(?:T|N_)\(\s*' + lit + r'\s*\)', code):
            ids.setdefault(c_unescape(m.group(1)), os.path.relpath(f, ROOT))
    for f in glob.glob(os.path.join(OVERLAY, '*/rootfs-overlay/usr/share/notatv/mpv/*.lua')) + \
             glob.glob(os.path.join(OVERLAY, '*/overlay/usr/share/notatv/mpv/*.lua')):
        for m in re.finditer(r'(?<![A-Za-z0-9_])T\(' + lit + r'\)', open(f, encoding='utf-8').read()):
            ids.setdefault(c_unescape(m.group(1)), os.path.relpath(f, ROOT))
    for f in glob.glob(os.path.join(OVERLAY, '*/rootfs-overlay/usr/share/notatv/www/index.html')):
        s = open(f, encoding='utf-8').read()
        rel = os.path.relpath(f, ROOT)
        for m in re.finditer(r'<(\w+)[^>]*\sdata-t(?:\s[^>]*)?>(.*?)</\1>', s):
            ids.setdefault(html.unescape(m.group(2)), rel)
        for m in re.finditer(r'<(\w+)[^>]*\sdata-th(?:\s[^>]*)?>(.*?)</\1>', s, re.S):
            ids.setdefault(m.group(2), rel)
        for m in re.finditer(r'<input[^>]*placeholder="([^"]*)"[^>]*data-tp', s):
            ids.setdefault(html.unescape(m.group(1)), rel)
        for m in re.finditer(r"(?<![A-Za-z0-9_])T\('((?:[^'\\]|\\.)*)'\)", s):
            ids.setdefault(m.group(1).replace("\\'", "'"), rel)
    # helper scripts: messages shown on the TV / phone page
    scripts = glob.glob(os.path.join(OVERLAY, '*/rootfs-overlay/usr/libexec/notatv/*')) + \
              glob.glob(os.path.join(OVERLAY, '*/overlay/usr/libexec/notatv/*')) + \
              glob.glob(os.path.join(OVERLAY, '*/rootfs-overlay/usr/share/notatv/www/cgi-bin/*'))
    for f in scripts:
        rel = os.path.relpath(f, ROOT)
        for line in open(f, encoding='utf-8', errors='replace'):
            for m in re.finditer(r'(?:\bmsg|\bprogress|\becho|\bdie|\breply\s+"[^"]*")\s+"([A-Z%][^"$]*)"', line):
                t = m.group(1)
                if '\\037' in t or t in ('OK',) or t.startswith('NY'):
                    continue
                ids.setdefault(t, rel)
            for m in re.finditer(r'· ([A-Z][^"$:]*)', line):  # dele af en sammensat statuslinje
                ids.setdefault(m.group(1).strip(), rel)
            for m in re.finditer(r"printf '([A-Z%][^']*?)\\037", line):
                ids.setdefault(m.group(1).replace('%%', '%'), rel)
    return ids


def read_po(path):
    out, cur, key, fuzzy = {}, None, None, False
    def flush():
        nonlocal cur, fuzzy
        if cur and cur.get('msgid') and not fuzzy:
            out[cur['msgid']] = cur.get('msgstr', '')
        cur, fuzzy = None, False
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('#,') and 'fuzzy' in line:
            flush(); fuzzy = True
        elif line.startswith('msgid '):
            fz = fuzzy; flush(); fuzzy = fz
            cur = {'msgid': c_unescape(line[7:-1])}; key = 'msgid'
        elif line.startswith('msgstr '):
            cur['msgstr'] = c_unescape(line[8:-1]); key = 'msgstr'
        elif line.startswith('"') and cur is not None:
            cur[key] += c_unescape(line[1:-1])
    flush()
    return out


def specs(s):
    return sorted(re.findall(r'%(?:\d+\$)?[-+ #0]*\d*(?:\.\d+)?[lh]*[sdufxXcg%]', s))


def po_quote(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t') + '"'


def main():
    ids = source_ids()
    bad = False
    for po in sorted(glob.glob(os.path.join(LANG_DIR, '*.po'))):
        lang = os.path.basename(po)[:-3]
        tr = read_po(po)
        missing = [i for i in ids if not tr.get(i)]
        unused = [i for i in tr if i not in ids]
        wrong = [i for i in ids if tr.get(i) and specs(i) != specs(tr[i]) and
                 sorted(re.sub(r'\d+\$', '', x) for x in specs(i)) != sorted(re.sub(r'\d+\$', '', x) for x in specs(tr[i]))]
        print(f'{lang}: {len(ids)} texts, {len(missing)} missing, {len(wrong)} placeholder mismatches, {len(unused)} unused')
        for i in missing: print(f'  missing   {ids[i]}: {i!r}')
        for i in wrong: print(f'  mismatch  {i!r} -> {tr[i]!r}')
        for i in unused: print(f'  unused    {i!r}')
        bad |= bool(missing or wrong)
    if '--pot' in sys.argv:
        with open(os.path.join(LANG_DIR, 'notatv.pot'), 'w', encoding='utf-8') as f:
            f.write('# NotaTV translation template. Copy to <code>.po, fill in msgstr and add the language to\n'
                    '# i18n_langs in src/shell/i18n.c (and src/moon/i18n.c).\n'
                    'msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n\n')
            for i in sorted(ids, key=str.lower):
                f.write(f'#: {ids[i]}\nmsgid {po_quote(i)}\nmsgstr ""\n\n')
    sys.exit(1 if bad else 0)


main()
