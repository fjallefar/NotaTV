#!/usr/bin/env python3
"""Check the translations in src/shell/assets/lang/*.po (and each app package's src/apps/<id>/lang/*.po) against the source code.

  python3 scripts/i18n-check.py            check all languages
  python3 scripts/i18n-check.py --pot      also write src/shell/assets/lang/notatv.pot (template for a new language)

Finds every translatable text: T("...") / N_("...") in C (src/shell, src/moon), T("...") in the mpv OSD
script, data-t/data-th/data-tp in the phone page, and the English messages of the helper scripts.
App packages are checked on their own: every text in src/apps/<id>/*.c must be in that app's lang/<code>.po
(an app brings its own translations). Reports texts missing from a .po file, entries no longer used, and translations whose printf
placeholders (%s, %d, %1$s ...) do not match the English text. Exit code 1 if anything is missing or wrong.

The texts in the code are English (the Danish goes in lang/da.po): a text with æ, ø or å in the code, or an app
with texts but no lang/da.po, is an error - so an app can never ship Danish-only again (TV 2 Play 1.0.0 did).
The release workflows run this check and stop on errors."""
import glob, html, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, 'src/shell/assets/lang')
OVERLAY = os.path.join(ROOT, 'external/board')


def c_unescape(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


LIT = r'"((?:[^"\\\n]|\\.)*)"'


def c_ids(files, ids):
    for f in files:
        code = re.sub(r'/\*.*?\*/', '', open(f, encoding='utf-8').read(), flags=re.S)
        for m in re.finditer(r'(?<![A-Za-z0-9_])(?:T|N_)\(\s*' + LIT + r'\s*\)', code):
            ids.setdefault(c_unescape(m.group(1)), os.path.relpath(f, ROOT))
    return ids


def source_ids():
    ids = {}
    lit = LIT
    c_ids(glob.glob(os.path.join(ROOT, 'src/shell/*.c')) + glob.glob(os.path.join(ROOT, 'src/moon/*.c')), ids)
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


def check(ids, lang_dir, label):
    bad = False
    for po in sorted(glob.glob(os.path.join(lang_dir, '*.po'))):
        lang = label + os.path.basename(po)[:-3]
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
    return bad


def not_english(ids, label):
    bad = [i for i in ids if re.search('[æøåÆØÅ]', i)]
    for i in bad: print(f'  {label}: Danish text in the code (write it in English, Danish in lang/da.po): {ids[i]}: {i!r}')
    return bool(bad)


# App ids are English (the folder, manifest id, nota_app id, cfg app_<id>, data folder and App Store catalog all
# use it, and it cannot be changed later without moving boxes over - see RENAMED_APPS in src/shell/apps.c).
# A new app must be added here on purpose, after checking that the id is an English word or a name (not Danish).
APP_IDS = {'peezy', 'drtv', 'electricity', 'flimmer', 'games', 'iptv', 'jellyfin', 'media', 'music', 'plex', 'radio', 'seerr',
           'tesla', 'tv2', 'twitch', 'weather', 'youtube'}


def check_app_id(d):
    folder = os.path.basename(d)
    bad = False
    mid = json.load(open(os.path.join(d, 'manifest.json'), encoding='utf-8')).get('id')
    if mid != folder:
        print(f'app {folder}: manifest id {mid!r} is not the folder name'); bad = True
    if folder not in APP_IDS:
        print(f'app {folder}: unknown app id - ids must be English (or a name); add it to APP_IDS in scripts/i18n-check.py'); bad = True
    src = ''.join(open(f, encoding='utf-8', errors='replace').read() for f in glob.glob(os.path.join(d, '*.c')))
    m = re.search(r'NOTA_API,\s*"([^"]*)"', src)
    if not m or m.group(1) != folder:
        print(f'app {folder}: nota_app id {m.group(1) if m else None!r} is not the folder name'); bad = True
    return bad


def main():
    ids = source_ids()
    bad = check(ids, LANG_DIR, '')
    bad |= not_english(ids, 'NotaTV')
    for app in sorted(glob.glob(os.path.join(ROOT, 'src/apps/*/manifest.json'))):
        d = os.path.dirname(app)
        bad |= check_app_id(d)
        ids = c_ids(glob.glob(os.path.join(d, '*.c')), {})
        ch = json.load(open(app, encoding='utf-8')).get('changes') or {}  # what is new (shown in the App Store)
        for t in [ch.get('summary', '')] + list(ch.get('points', [])):
            if t: ids.setdefault(t, os.path.relpath(app, ROOT))
        name = 'app ' + os.path.basename(d)
        bad |= not_english(ids, name)
        if ids and not os.path.exists(os.path.join(d, 'lang', 'da.po')):
            print(f'{name}: {len(ids)} texts but no lang/da.po')
            bad = True
        bad |= check(ids, os.path.join(d, 'lang'), name + ': ')
    if '--pot' in sys.argv:
        with open(os.path.join(LANG_DIR, 'notatv.pot'), 'w', encoding='utf-8') as f:
            f.write('# NotaTV translation template. Copy to <code>.po, fill in msgstr and add the language to\n'
                    '# i18n_langs in src/shell/i18n.c (and src/moon/i18n.c).\n'
                    'msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n\n')
            for i in sorted(ids, key=str.lower):
                f.write(f'#: {ids[i]}\nmsgid {po_quote(i)}\nmsgstr ""\n\n')
    sys.exit(1 if bad else 0)


main()
