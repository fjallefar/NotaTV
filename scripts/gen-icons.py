#!/usr/bin/env python3
"""Draw NotaTV's app icons (our own drawings - no logo files from the services) as SVG and render them to PNG
with rsvg-convert. Output: src/shell/assets/icons/<app id>.png (496x280 = the home tile at 2x).
Run again after changing a design: scripts/gen-icons.py"""
import math, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src/shell/assets/icons")
W, H = 496, 280
CX, CY = W / 2, H / 2

def star(cx, cy, r1, r2, n=5, rot=-90):
    pts = []
    for i in range(n * 2):
        r = r1 if i % 2 == 0 else r2
        a = math.radians(rot + i * 180 / n)
        pts.append(f"{cx + r * math.cos(a):.1f},{cy + r * math.sin(a):.1f}")
    return " ".join(pts)

def gear(cx, cy, r_out, r_in, teeth=8, tooth=0.42):
    # teeth as trapezoids around a ring; returned as one path
    pts = []
    step = 2 * math.pi / teeth
    for i in range(teeth):
        a0 = i * step
        for a, r in ((a0 - step * tooth / 2 - step * 0.08, r_in), (a0 - step * tooth / 2, r_out),
                     (a0 + step * tooth / 2, r_out), (a0 + step * tooth / 2 + step * 0.08, r_in)):
            pts.append(f"{cx + r * math.cos(a):.1f},{cy + r * math.sin(a):.1f}")
    return "M" + " L".join(pts) + " Z"

def icon(c1, c2, glyph, glow="#ffffff"):
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">
<defs>
 <linearGradient id="bg" x1="0" y1="0" x2="0.35" y2="1"><stop offset="0" stop-color="{c1}"/><stop offset="1" stop-color="{c2}"/></linearGradient>
 <radialGradient id="hl" cx="0.22" cy="-0.1" r="0.95"><stop offset="0" stop-color="#fff" stop-opacity="0.30"/><stop offset="0.6" stop-color="#fff" stop-opacity="0.04"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
 <radialGradient id="gl" cx="0.5" cy="0.52" r="0.42"><stop offset="0" stop-color="{glow}" stop-opacity="0.22"/><stop offset="1" stop-color="{glow}" stop-opacity="0"/></radialGradient>
 <filter id="sh" x="-30%" y="-30%" width="160%" height="160%"><feDropShadow dx="0" dy="7" stdDeviation="9" flood-color="#000" flood-opacity="0.32"/></filter>
</defs>
<rect width="{W}" height="{H}" fill="url(#bg)"/>
<rect width="{W}" height="{H}" fill="url(#gl)"/>
<rect width="{W}" height="{H}" fill="url(#hl)"/>
<g filter="url(#sh)">{glyph}</g>
</svg>"""

ICONS = {
    # Live TV: a screen with a red "live" dot
    "iptv": icon("#3B82F6", "#1E3A8A", """
      <rect x="166" y="76" width="164" height="110" rx="22" fill="none" stroke="#fff" stroke-width="15"/>
      <path d="M212 212 H284" stroke="#fff" stroke-width="14" stroke-linecap="round"/>
      <circle cx="322" cy="84" r="20" fill="#FF453A" stroke="#fff" stroke-width="7"/>"""),
    # Plex: a bold orange chevron on charcoal
    "plex": icon("#3A3A40", "#141417", """
      <defs><linearGradient id="or" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FFC233"/><stop offset="1" stop-color="#E5A00D"/></linearGradient></defs>
      <path d="M206 60 H262 L318 140 L262 220 H206 L262 140 Z" fill="url(#or)" stroke="url(#or)" stroke-width="6" stroke-linejoin="round"/>""", glow="#FFB020"),
    # Jellyfin: a rounded triangle with a smaller one inside, on its purple-blue
    "jellyfin": icon("#A855F7", "#0284C7", """
      <path d="M248 62 L330 206 H166 Z" fill="none" stroke="#fff" stroke-width="16" stroke-linejoin="round"/>
      <path d="M248 132 L274 178 H222 Z" fill="#fff" stroke="#fff" stroke-width="8" stroke-linejoin="round"/>"""),
    # YouTube: a white rounded screen with a red play triangle
    "youtube": icon("#FF4438", "#B91C1C", """
      <rect x="172" y="84" width="152" height="112" rx="34" fill="#fff"/>
      <path d="M232 116 L276 140 L232 164 Z" fill="#E11D26" stroke="#E11D26" stroke-width="6" stroke-linejoin="round"/>"""),
    # Twitch: a chat bubble with a play triangle (live streams + chat)
    "twitch": icon("#A970FF", "#5B21B6", """
      <path d="M188 72 H308 Q328 72 328 92 V162 Q328 182 308 182 H252 L214 214 V182 H188 Q168 182 168 162 V92 Q168 72 188 72 Z" fill="#fff"/>
      <path d="M232 104 L274 127 L232 150 Z" fill="#7C3AED" stroke="#7C3AED" stroke-width="6" stroke-linejoin="round"/>"""),
    # Seerr: a star with a plus (ask for a movie)
    "seerr": icon("#818CF8", "#3730A3", f"""
      <polygon points="{star(236, 146, 78, 34)}" fill="#fff" stroke="#fff" stroke-width="8" stroke-linejoin="round"/>
      <circle cx="318" cy="88" r="28" fill="#fff"/>
      <path d="M318 74 V102 M304 88 H332" stroke="#4F46E5" stroke-width="8" stroke-linecap="round"/>"""),
    # Music: a double note and sound bars
    "music": icon("#FF4D6D", "#9F1239", """
      <path d="M206 186 V92 L282 76 V170" fill="none" stroke="#fff" stroke-width="13" stroke-linejoin="round"/>
      <path d="M206 92 L282 76 V104 L206 120 Z" fill="#fff"/>
      <ellipse cx="190" cy="188" rx="24" ry="19" fill="#fff"/>
      <ellipse cx="266" cy="172" rx="24" ry="19" fill="#fff"/>
      <path d="M318 120 V176 M338 100 V196 M358 128 V168" stroke="#fff" stroke-width="11" stroke-linecap="round" opacity="0.85"/>"""),
    # Games: a game pad
    "games": icon("#34D399", "#047857", """
      <path d="M196 96 H300 Q348 96 356 150 Q362 196 334 200 Q316 202 300 178 H196 Q180 202 162 200 Q134 196 140 150 Q148 96 196 96 Z" fill="#fff"/>
      <path d="M196 124 V160 M178 142 H214" stroke="#059669" stroke-width="11" stroke-linecap="round"/>
      <circle cx="292" cy="132" r="9" fill="#059669"/><circle cx="314" cy="152" r="9" fill="#059669"/>"""),
    # Media: a folder with a play triangle
    "media": icon("#A78BFA", "#5B21B6", """
      <path d="M170 92 Q170 76 186 76 H226 L244 94 H310 Q326 94 326 110 V188 Q326 204 310 204 H186 Q170 204 170 188 Z" fill="#fff"/>
      <path d="M236 124 L276 147 L236 170 Z" fill="#6D28D9" stroke="#6D28D9" stroke-width="6" stroke-linejoin="round"/>"""),
    # Tesla (TeslaMate): a battery being charged
    "tesla": icon("#3F4451", "#111318", """
      <rect x="160" y="96" width="166" height="88" rx="18" fill="none" stroke="#fff" stroke-width="12"/>
      <rect x="332" y="122" width="14" height="36" rx="5" fill="#fff"/>
      <rect x="176" y="112" width="96" height="56" rx="8" fill="#34D399"/>
      <path d="M252 84 L222 146 H250 L238 198 L282 128 H254 L268 84 Z" fill="#fff" stroke="#2B2F38" stroke-width="5" stroke-linejoin="round"/>""", glow="#34D399"),
    # App Store: four app squares, one of them a plus
    "appstore": icon("#2DD4BF", "#0F766E", """
      <rect x="182" y="74" width="58" height="58" rx="16" fill="#fff"/>
      <rect x="256" y="74" width="58" height="58" rx="16" fill="#fff"/>
      <rect x="182" y="148" width="58" height="58" rx="16" fill="#fff"/>
      <path d="M285 152 V202 M260 177 H310" stroke="#fff" stroke-width="13" stroke-linecap="round"/>"""),
    # Settings: a gear
    "settings": icon("#7B8494", "#3B4252", f"""
      <path d="{gear(248, 140, 78, 60, 9)}" fill="#fff" stroke="#fff" stroke-width="6" stroke-linejoin="round"/>
      <circle cx="248" cy="140" r="56" fill="#fff"/>
      <circle cx="248" cy="140" r="24" fill="#545C6B"/>"""),
}

def main():
    os.makedirs(OUT, exist_ok=True)
    only = set(sys.argv[1:])
    for name, svg in ICONS.items():
        if only and name not in only: continue
        png = os.path.join(OUT, name + ".png")
        subprocess.run(["rsvg-convert", "-w", str(W), "-h", str(H), "-o", png], input=svg.encode(), check=True)
        print(png)

if __name__ == "__main__":
    main()
