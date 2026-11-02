#!/usr/bin/env python3
"""Pack the built app packages for the App Store and write its catalog.

  python3 scripts/apps-catalog.py <apps-out> <dist> [--strip <strip program>]

<apps-out> holds one folder per app (manifest.json, app.so, lang/), as built by src/apps/Makefile.
For every app this writes <dist>/<id>-<version>-api<N>.tar.gz, and one catalog per app API version,
<dist>/apps-api<N>.json, with version, file name, SHA256, size and what the App Store shows (name,
description, icon, colors, order, default, and "changes": what is new in this version) - also translated,
from the app's lang/<code>.po.

"changes" in manifest.json is a very short summary and a few points, shown in the App Store before an update:
  "changes": {"summary": "Faster start", "points": ["Opens in half the time", "Fixed ..."]}
Write it again (or remove it) whenever "version" is raised; the texts go in lang/<code>.po like the other texts.

The release workflow uploads <dist> to the GitHub release "apps" (a pre-release), where the boxes
fetch it: https://github.com/<repo>/releases/download/apps/apps-api<N>.json."""
import glob, hashlib, json, os, re, shutil, subprocess, sys, tarfile, tempfile


def read_po(path):
    out, cur, key = {}, None, None
    def unq(s):
        return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('msgid '):
            if cur and cur.get('msgid'): out[cur['msgid']] = cur.get('msgstr', '')
            cur = {'msgid': unq(line[7:-1])}; key = 'msgid'
        elif line.startswith('msgstr '):
            cur['msgstr'] = unq(line[8:-1]); key = 'msgstr'
        elif line.startswith('"') and cur is not None:
            cur[key] += unq(line[1:-1])
    if cur and cur.get('msgid'): out[cur['msgid']] = cur.get('msgstr', '')
    return out


def main():
    args = sys.argv[1:]
    strip = None
    if '--strip' in args:
        i = args.index('--strip'); strip = args[i + 1]; del args[i:i + 2]
    src, dist = args
    os.makedirs(dist, exist_ok=True)
    catalogs = {}
    for man in sorted(glob.glob(os.path.join(src, '*', 'manifest.json'))):
        d = os.path.dirname(man)
        m = json.load(open(man, encoding='utf-8'))
        app_id, ver, api = m['id'], m['version'], int(m['api'])
        assert app_id == os.path.basename(d), f'{d}: id must be the folder name'
        assert re.fullmatch(r'[a-z0-9_-]{1,32}', app_id) and re.fullmatch(r'[0-9]+(\.[0-9]+){0,3}', ver)
        with tempfile.TemporaryDirectory() as tmp:
            pkg = os.path.join(tmp, 'pkg')
            shutil.copytree(d, pkg)
            if strip:
                subprocess.run([strip, '--strip-unneeded', os.path.join(pkg, 'app.so')], check=True)
            name = f'{app_id}-{ver}-api{api}.tar.gz'
            out = os.path.join(dist, name)
            with tarfile.open(out, 'w:gz') as t:  # plain files and folders, relative paths, owned by root
                for root, dirs, files in os.walk(pkg):
                    dirs.sort()
                    for f in sorted(files):
                        p = os.path.join(root, f)
                        ti = t.gettarinfo(p, os.path.relpath(p, pkg))
                        ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'
                        with open(p, 'rb') as fh: t.addfile(ti, fh)
        data = open(out, 'rb').read()
        entry = {k: m[k] for k in ('id', 'version', 'name', 'desc', 'icon', 'c1', 'c2', 'fg', 'order', 'default', 'changes') if k in m}
        ch = m.get('changes') or {}
        assert isinstance(ch, dict) and isinstance(ch.get('summary', ''), str) and isinstance(ch.get('points', []), list), \
            f'{d}: "changes" must be {{"summary": "...", "points": ["...", ...]}}'
        entry.update(file=name, sha256=hashlib.sha256(data).hexdigest(), size=len(data))
        tr = {}
        for po in sorted(glob.glob(os.path.join(d, 'lang', '*.po'))):
            t = read_po(po)
            lang = os.path.basename(po)[:-3]
            tr[lang] = {k: t[m[k]] for k in ('name', 'desc') if t.get(m.get(k, ''))}
            if ch:
                tch = {'summary': t.get(ch.get('summary', ''), '') or ch.get('summary', ''),
                       'points': [t.get(x, '') or x for x in ch.get('points', [])]}
                if tch != {'summary': ch.get('summary', ''), 'points': ch.get('points', [])}: tr[lang]['changes'] = tch
            if not tr[lang]: del tr[lang]
        if tr: entry['i18n'] = tr
        catalogs.setdefault(api, []).append(entry)
        print(f'{name}  {len(data)} bytes')
    for api, apps in catalogs.items():
        apps.sort(key=lambda a: (a.get('order', 500), a['id']))
        with open(os.path.join(dist, f'apps-api{api}.json'), 'w', encoding='utf-8') as f:
            json.dump({'api': api, 'apps': apps}, f, ensure_ascii=False, indent=1)
            f.write('\n')
        print(f'apps-api{api}.json  {len(apps)} apps')


main()
