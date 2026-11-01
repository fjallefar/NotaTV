#!/usr/bin/env python3
"""Genererer src/shell/tzlist.h: lande (engelske og danske navne fra iso-codes) -> tidszoner (fra tzdata zone.tab).
Kør igen når tzdata opdateres:  python3 scripts/gen-tzlist.py <zone.tab> > src/shell/tzlist.h"""
import gettext, json, sys
zt = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/zoneinfo/zone.tab'
iso = {c['alpha_2']: c['name'] for c in json.load(open('/usr/share/iso-codes/json/iso_3166-1.json'))['3166-1']}
isoc = {c['alpha_2']: c.get('common_name', c['name']) for c in json.load(open('/usr/share/iso-codes/json/iso_3166-1.json'))['3166-1']}
tr = gettext.translation('iso_3166-1', '/usr/share/locale', ['da'])
zones = {}
for line in open(zt, encoding='utf-8'):
    if line.startswith('#') or not line.strip(): continue
    f = line.rstrip('\n').split('\t')
    zones.setdefault(f[0], []).append((f[2], f[3] if len(f) > 3 else ''))
DA_FIX = {'KR': 'Sydkorea', 'KP': 'Nordkorea', 'TR': 'Tyrkiet', 'SY': 'Syrien'}  # missing from iso-codes' short Danish names
def da(code):  # short name (common_name) if it exists, otherwise the official one
    return DA_FIX.get(code) or tr.gettext(isoc.get(code, iso.get(code, code)))
order = 'abcdefghijklmnopqrstuvwxyzæøå'
def key(s):
    s = s.lower()
    return [order.index(ch) if ch in order else ord(ch) + 100 for ch in s]
def en(code): return isoc.get(code, code)
cs = sorted(zones, key=lambda c: key(da(c)))
cs_en = sorted(zones, key=lambda c: en(c).lower())
def q(s): return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
print('/* Genereret af scripts/gen-tzlist.py - ret ikke i hånden. Landenavne: iso-codes (LGPL-2.1), zoner: tzdata (public domain) */')
print('typedef struct { const char *zone, *note; } TzZone;')
print('typedef struct { const char *code, *name_en, *name_da; const TzZone *z; int n; } TzCountry;')
for c in cs:
    print(f'static const TzZone tzz_{c}[] = {{' + ', '.join('{%s, %s}' % (q(z), q(n)) for z, n in zones[c]) + '};')
print('static const TzCountry tz_countries[] = {')
for c in cs:
    print(f'    {{"{c}", {q(en(c))}, {q(da(c))}, tzz_{c}, {len(zones[c])}}},')
print('};')
print('/* visningsrækkefølge (indeks i tz_countries) efter engelsk og dansk navn */')
print('static const short tz_order_en[] = {' + ', '.join(str(cs.index(c)) for c in cs_en) + '};')
print('static const short tz_order_da[] = {' + ', '.join(str(i) for i in range(len(cs))) + '};')
print('#define TZ_N_COUNTRIES (int)(sizeof tz_countries / sizeof tz_countries[0])')
