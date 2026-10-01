#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Erzeugt das App-Icon und das symbolische Icon von Gnomos nach GNOME-HIG.

Ein Lautsprecher steht auf einer runden Scheibe, deren Ringe in flachen
Blautönen den Klang zeigen, der sich im Raum ausbreitet (Mehrraum, kabellos).
Raster 2 px, Farben aus der GNOME-Palette, Profil vorn 4 px. Die Entwürfe,
aus denen dieses Icon gewählt wurde, stehen in docs/icon.md (Entwurf C1).

Zusätzlich rendert es PNGs in 48/64/128/256 px (per rsvg-convert): Ohne
gdk-pixbuf-Loader für SVG zeigt GNOME Shell sonst nur eine leere Kachel, und
Flatpaks Icon-Prüfung beim Export scheitert.

    python3 build-aux/icons/generate_icons.py
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_ID = "de.linuxundich.Gnomos"
ICONS = ROOT / "data" / "icons" / "hicolor"

# GNOME-Palette (https://developer.gnome.org/hig/reference/palette.html)
BLUE1, BLUE2, BLUE3, BLUE5 = "#99c1f1", "#62a0ea", "#3584e4", "#1a5fb4"
LIGHT4, LIGHT5 = "#c0bfbc", "#9a9996"
DARK1, DARK2, DARK3, DARK4 = "#77767b", "#5e5c64", "#3d3846", "#241f31"
SYM = "#2e3436"  # Standardfarbe für symbolische Icons


def svg(body: str, size: int) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
            f'viewBox="0 0 {size} {size}">\n{body}</svg>\n')


APP = svg(f'''  <!-- Scheibe: Profil, dann die Ringe als flache Flächen auf der Oberseite -->
  <ellipse cx="64" cy="102" rx="58" ry="18" fill="{BLUE5}"/>
  <ellipse cx="64" cy="98" rx="58" ry="18" fill="{BLUE1}"/>
  <ellipse cx="64" cy="98" rx="44" ry="13.5" fill="{BLUE2}"/>
  <ellipse cx="64" cy="98" rx="30" ry="9" fill="{BLUE3}"/>
  <!-- Lautsprecher: Profil, Front, Hochtöner, Membran -->
  <rect x="38" y="24" width="52" height="76" rx="14" fill="{DARK3}"/>
  <rect x="38" y="20" width="52" height="76" rx="14" fill="{DARK2}"/>
  <circle cx="64" cy="38" r="6" fill="{DARK4}"/>
  <circle cx="64" cy="38" r="2.5" fill="{LIGHT5}"/>
  <circle cx="64" cy="70" r="17" fill="{DARK4}"/>
  <circle cx="64" cy="70" r="11" fill="{DARK1}"/>
  <circle cx="64" cy="70" r="5" fill="{LIGHT4}"/>
''', 128)

# Symbolisch: Lautsprecher mit ausgestanzter Membran, darunter die Scheibe,
# mit 1 px Abstand um den Lautsprecher freigestellt.
SYMBOLIC = svg(f'''  <mask id="plate"><rect width="16" height="16" fill="#fff"/><rect x="3.5" y="0" width="9" height="12" rx="2.5" fill="#000"/></mask>
  <mask id="cone"><rect width="16" height="16" fill="#fff"/><circle cx="8" cy="7" r="2" fill="#000"/><circle cx="8" cy="3" r="0.75" fill="#000"/></mask>
  <ellipse cx="8" cy="12" rx="7.5" ry="3.5" fill="{SYM}" mask="url(#plate)"/>
  <rect x="4.5" y="0.5" width="7" height="10.5" rx="1.5" fill="{SYM}" mask="url(#cone)"/>
''', 16)


def main() -> None:
    app = ICONS / "scalable" / "apps" / f"{APP_ID}.svg"
    sym = ICONS / "symbolic" / "apps" / f"{APP_ID}-symbolic.svg"
    for path, content in ((app, APP), (sym, SYMBOLIC)):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    for size in (48, 64, 128, 256):
        png = ICONS / f"{size}x{size}" / "apps" / f"{APP_ID}.png"
        png.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), str(app), "-o", str(png)],
                       check=True)
    print("Icons erzeugt in", ICONS.relative_to(ROOT))


if __name__ == "__main__":
    main()
