#!/usr/bin/env python3
"""Pack the built app packages for the App Store and write its catalog.

  python3 scripts/apps-catalog.py <apps-out> <dist> [--strip <strip program>] [--notes <file.md>]
                                  [--tools src/tools [--dl <Buildroot dl folder>]]

--tools also packs the tools (src/tools/<id>/manifest.json, see src/tools/README.md): an upstream zip pinned by its
SHA256 (taken from <dl>/<id>/ when it is there, otherwise downloaded) becomes <dist>/<id>-<version>-tool.tar.gz and is
listed under "tools" in every catalog. The App Store does not show tools; a box installs a newer one by itself.

--notes writes the text of the GitHub release "apps": every app with its version and what is new (its "changes"),
so the release page shows the same notes as the App Store. It is written outside <dist> (not uploaded as a file).

<apps-out> holds one folder per app (manifest.json, app.so, lang/), as built by src/apps/Makefile.
For every app this writes <dist>/<id>-<version>-api<N>.tar.gz, and one catalog per app API version,
<dist>/apps-api<N>.json, with version, file name, SHA256, size and what the App Store shows (name,
description, icon, colors, order, default, and "changes": what is new in this version) - also translated,
from the app's lang/<code>.po.

An optional icon.png in the package (at most 1024x1024 and 512 KB) is the app's own tile picture. It also goes in
<dist> as <id>-<version>-icon.png, with "icon_file" and "icon_sha256" in the catalog, so the App Store can show it
before the app is installed (the box checks the SHA256, see apppkg icon).

"changes" in manifest.json is a very short summary and a few points, shown in the App Store before an update:
  "changes": {"summary": "Faster start", "points": ["Opens in half the time", "Fixed ..."]}
Write it again (or remove it) whenever "version" is raised; the texts go in lang/<code>.po like the other texts.

The release workflow uploads <dist> to the GitHub release "apps" (a pre-release), where the boxes
fetch it: https://github.com/<repo>/releases/download/apps/apps-api<N>.json."""
import glob, gzip, hashlib, json, os, re, shutil, subprocess, sys, tarfile, tempfile

PKG_MTIME = 1767225600  # 2026-01-01: the time every file in a package gets (reproducible packages)


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


def write_pkg(pkg, out):
    # plain files and folders, relative paths, owned by root. The same files always give the same bytes (fixed
    # times in tar and gzip): a package built again under the same name keeps its SHA256, so a box holding the
    # catalog from before the upload still gets a matching file
    with open(out, 'wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as gz, \
            tarfile.open(fileobj=gz, mode='w', format=tarfile.GNU_FORMAT) as t:
        for root, dirs, files in os.walk(pkg):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(root, f)
                ti = t.gettarinfo(p, os.path.relpath(p, pkg))
                ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'
                ti.mtime = PKG_MTIME
                with open(p, 'rb') as fh: t.addfile(ti, fh)


def build_tools(tools_dir, dl_dir, dist, strip=None):
    """Tools (src/tools/<id>/manifest.json): a program from upstream, pinned by version + SHA256 of its zip, or of a
    .tar.xz with "files" ({member: name}: only those files are taken, e.g. Node's bin/node and LICENSE).
    The download is taken from Buildroot's download folder if it is there, otherwise downloaded, and always checked."""
    out = []
    for man in sorted(glob.glob(os.path.join(tools_dir, '*', 'manifest.json'))):
        m = json.load(open(man, encoding='utf-8'))
        tid, ver, binname, url, want = m['id'], m['version'], m['bin'], m['url'], m['sha256']
        assert m.get('kind') == 'tool' and tid == os.path.basename(os.path.dirname(man)), f'{man}: kind/id'
        assert re.fullmatch(r'[a-z0-9_-]{1,32}', tid) and re.fullmatch(r'[0-9]+(\.[0-9]+){0,3}', ver)
        assert re.fullmatch(r'[A-Za-z0-9._-]+', binname) and not binname.startswith('.') and re.fullmatch(r'[0-9a-f]{64}', want)
        assert url.startswith('https://'), f'{man}: url must be https'
        with tempfile.TemporaryDirectory() as tmp:
            zpath = os.path.join(dl_dir or '', tid, os.path.basename(url)) if dl_dir else ''
            if not (zpath and os.path.exists(zpath) and hashlib.sha256(open(zpath, 'rb').read()).hexdigest() == want):
                import urllib.request
                zpath = os.path.join(tmp, os.path.basename(url))
                with urllib.request.urlopen(url, timeout=300) as r, open(zpath, 'wb') as f: shutil.copyfileobj(r, f)
            got = hashlib.sha256(open(zpath, 'rb').read()).hexdigest()
            assert got == want, f'{tid}: SHA256 of {url} is {got}, the manifest says {want}'
            pkg = os.path.join(tmp, 'pkg')
            os.makedirs(pkg)
            if m.get('files'):  # a .tar.xz (Node.js): only the listed members, each to its own plain name
                import tarfile as tf
                with tf.open(zpath, 'r:xz') as t:
                    for member, dest in m['files'].items():
                        assert re.fullmatch(r'[A-Za-z0-9._-]+', dest) and not dest.startswith('.'), f'{tid}: bad name {dest}'
                        ti = t.getmember(member)
                        assert ti.isfile(), f'{tid}: {member} is not a plain file'
                        with t.extractfile(ti) as src, open(os.path.join(pkg, dest), 'wb') as f: shutil.copyfileobj(src, f)
                        os.chmod(os.path.join(pkg, dest), 0o755 if ti.mode & 0o111 else 0o644)
            else:
                import zipfile
                with zipfile.ZipFile(zpath) as z:
                    for zi in z.infolist():
                        name = zi.filename
                        assert not name.startswith('/') and '..' not in name.split('/'), f'{tid}: bad path {name}'
                        mode = (zi.external_attr >> 16) & 0o777
                        if (zi.external_attr >> 16) & 0o170000 == 0o120000: raise SystemExit(f'{tid}: symlink {name} in the zip')
                        z.extract(zi, pkg)
                        if not zi.is_dir(): os.chmod(os.path.join(pkg, name), 0o755 if mode & 0o111 else 0o644)
            b = os.path.join(pkg, binname)
            assert os.path.isfile(b), f'{tid}: {binname} is not in the download'
            os.chmod(b, 0o755)
            if m.get('strip') and strip:  # Node: 150 MB as released, about 110 MB without symbols
                subprocess.run([strip, '--strip-unneeded', b], check=True)
            with open(os.path.join(pkg, 'manifest.json'), 'w', encoding='utf-8') as f:
                json.dump({'id': tid, 'version': ver, 'kind': 'tool', 'bin': binname}, f, indent=1)
                f.write('\n')
            name = f'{tid}-{ver}-tool.tar.gz'
            write_pkg(pkg, os.path.join(dist, name))
        data = open(os.path.join(dist, name), 'rb').read()
        out.append({'id': tid, 'version': ver, 'bin': binname, 'file': name,
                    'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)})
        print(f'{name}  {len(data)} bytes (tool)')
    return out


def main():
    args = sys.argv[1:]
    strip = None
    if '--strip' in args:
        i = args.index('--strip'); strip = args[i + 1]; del args[i:i + 2]
    notes = None
    if '--notes' in args:
        i = args.index('--notes'); notes = args[i + 1]; del args[i:i + 2]
    tools_dir = dl_dir = None
    if '--tools' in args:
        i = args.index('--tools'); tools_dir = args[i + 1]; del args[i:i + 2]
    if '--dl' in args:
        i = args.index('--dl'); dl_dir = args[i + 1]; del args[i:i + 2]
    src, dist = args
    os.makedirs(dist, exist_ok=True)
    catalogs = {}
    for man in sorted(glob.glob(os.path.join(src, '*', 'manifest.json'))):
        d = os.path.dirname(man)
        m = json.load(open(man, encoding='utf-8'))
        app_id, ver, api = m['id'], m['version'], int(m['api'])
        assert app_id == os.path.basename(d), f'{d}: id must be the folder name'
        assert re.fullmatch(r'[a-z0-9_-]{1,32}', app_id) and re.fullmatch(r'[0-9]+(\.[0-9]+){0,3}', ver)
        missing = [h for h in m.get('helpers', []) if not os.path.exists(os.path.join(d, h))]
        if missing:  # e.g. cinema without its flutter-pi (made by src/apps/cinema/build): never publish half an app
            print(f'{app_id}: skipped, missing {", ".join(missing)}', file=sys.stderr)
            continue
        with tempfile.TemporaryDirectory() as tmp:
            pkg = os.path.join(tmp, 'pkg')
            shutil.copytree(d, pkg)
            if strip:
                subprocess.run([strip, '--strip-unneeded', os.path.join(pkg, 'app.so')], check=True)
                for helper in m.get('helpers', []):  # programs the app runs (e.g. tv2's wvserve)
                    subprocess.run([strip, '--strip-unneeded', os.path.join(pkg, helper)], check=True)
            name = f'{app_id}-{ver}-api{api}.tar.gz'
            out = os.path.join(dist, name)
            write_pkg(pkg, out)
            unpacked = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(pkg) for f in fs)
        data = open(out, 'rb').read()
        icon = os.path.join(d, 'icon.png')  # the app's own tile picture (optional), also shown before it is installed
        icon_entry = {}
        if os.path.exists(icon):
            raw = open(icon, 'rb').read()
            assert raw[:8] == b'\x89PNG\r\n\x1a\n' and raw[12:16] == b'IHDR', f'{icon}: must be a PNG'
            w, h = int.from_bytes(raw[16:20], 'big'), int.from_bytes(raw[20:24], 'big')
            assert len(raw) <= 512 * 1024 and 0 < w <= 1024 and 0 < h <= 1024, f'{icon}: at most 1024x1024 and 512 KB'
            iname = f'{app_id}-{ver}-icon.png'
            with open(os.path.join(dist, iname), 'wb') as f: f.write(raw)
            icon_entry = {'icon_file': iname, 'icon_sha256': hashlib.sha256(raw).hexdigest()}
        entry = {k: m[k] for k in ('id', 'version', 'name', 'desc', 'icon', 'c1', 'c2', 'fg', 'order', 'default', 'nonfree', 'changes') if k in m}
        ch = m.get('changes') or {}
        assert isinstance(ch, dict) and isinstance(ch.get('summary', ''), str) and isinstance(ch.get('points', []), list), \
            f'{d}: "changes" must be {{"summary": "...", "points": ["...", ...]}}'
        entry.update(file=name, sha256=hashlib.sha256(data).hexdigest(), size=len(data),
                     installed_size=unpacked, **icon_entry)  # installed_size: bytes on the box once unpacked (App Store)
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
    # tools are the same for every app API (they do not use tv.h); a tvshell without tools support ignores the key
    tools = build_tools(tools_dir, dl_dir, dist, strip) if tools_dir else []
    for api, apps in catalogs.items():
        apps.sort(key=lambda a: (a.get('order', 500), a['id']))
        with open(os.path.join(dist, f'apps-api{api}.json'), 'w', encoding='utf-8') as f:
            cat = {'api': api, 'apps': apps}
            if tools: cat['tools'] = tools
            json.dump(cat, f, ensure_ascii=False, indent=1)
            f.write('\n')
        print(f'apps-api{api}.json  {len(apps)} apps')
    if notes and catalogs:
        api = max(catalogs)  # the newest app API: what an up-to-date box sees
        out = ['Apps for the NotaTV App Store. Boxes download them from here - do not delete.', '',
               f'## The apps now (app API {api})', '']
        for a in catalogs[api]:
            ch = a.get('changes') or {}
            line = f"### {a['name']} {a['version']}"
            if a.get('nonfree'): line += ' (needs closed software - opt-in)'
            out.append(line)
            out.append(ch.get('summary') or a.get('desc', ''))
            out += [f'- {x}' for x in ch.get('points', [])]
            out.append('')
        with open(notes, 'w', encoding='utf-8') as f:
            f.write('\n'.join(out))
        print(f'{notes}  release notes for "apps"')


main()
