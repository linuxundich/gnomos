# Icon für Gnomos

Stand: 2026-10-01 · **Entwurf C1 (Lautsprecher auf einer Klangscheibe) freigegeben und umgesetzt.**

Das endgültige Icon erzeugt `build-aux/icons/generate_icons.py` (SVG, symbolisches SVG und PNGs in 48/64/128/256 px), die Vorschau `build-aux/icons/make_preview.sh` → `docs/icon-preview.png`. Die Entwürfe der vier Runden liegen zum Vergleich in `docs/icon-drafts/`.

Runde 1 (A Lautsprecher, B Grundriss, C Controller) liegt in `docs/icon-drafts/runde1/`. Der Nutzer hat A gewählt, aber die zwei getrennt stehenden Geräte störten ihn. Runde 2 (`docs/icon-drafts/runde2/`) variiert A deshalb als ein zusammenhängendes Objekt. Dort störte, dass die WLAN-Bögen lose über den Geräten stehen, deshalb macht Runde 3 (`docs/icon-drafts/runde3/`) die Wellen zum Teil des Lautsprechers. Daraus hat der Nutzer B3 gewählt, Runde 4 variiert es, und aus Runde 4 wurde C1.

Die Entwürfe liegen in `docs/icon-drafts/`, die Vorschau erzeugt `docs/icon-drafts/make_preview.sh` → `docs/icon-drafts/preview.png` (128/64/32 px, symbolisch 16 px, hell und dunkel, daneben zwei GNOME-Referenzicons in 64 px).

## Warum ein neues Icon

Das bisherige Icon (schwarzes Play-Dreieck aus der Decibels-Vorlage mit weißen Wellen) verstößt an mehreren Stellen gegen die [HIG für App-Icons](https://developer.gnome.org/hig/guidelines/app-icons.html):

- Es ist flach, ohne Draufsicht und Front-Profil.
- Die Farben stammen aus dem Sonos-Schriftzug (reines Schwarz/Weiß), nicht aus der GNOME-Palette.
- Die Form ist von Decibels übernommen. Daneben wirkt Gnomos wie eine Variante davon, nicht wie eine eigene App.
- Ein symbolisches Icon fehlt.

## Regeln (HIG), nach denen die Entwürfe gebaut sind

- Leinwand 128 × 128, 2-px-Raster, gemeinsame Grundlinie, keine extremen Seitenverhältnisse.
- Draufsicht und Front-Profil, das Profil 4 px hoch und dunkler als die Front.
- Farben aus der GNOME-Palette, flache Flächen ohne Verlauf, keine Schatten.
- Wenig Details, damit es auch bei 64 und 32 px funktioniert.
- Ein physisches Objekt als Metapher (die HIG nennt selbst „einen Lautsprecher für eine Musik-App“).
- Kein Sonos-Logo, keine Sonos-Gerätekopie.
- Symbolisches Icon 16 × 16 in `#2e3436` mit derselben Metapher.

## Recherche

- Mehrraum-Icons im Netz zeigen meist mehrere Lautsprecher oder ein Haus mit Schallwellen, verbunden durch WLAN-Bögen.
- In GNOME belegt sind schon der einzelne Lautsprecher (Decibels-Vorlage, gelbe Membran), das Play-Dreieck (Musik), das Radio mit Wellen (Shortwave) und der Bernstein (Amberol). Die Entwürfe grenzen sich davon ab.
- Sonos-typisch sind schlichte schwarze und weiße Lautsprecher und der Handcontroller CR100 mit Display und Scrollrad. Gnomos legt Wert auf diese erste Hardware-Generation.

## Entwurf A: Zwei Lautsprecher mit Funkwellen (Empfehlung)

Ein großer dunkler und ein kleiner heller Lautsprecher stehen auf derselben Grundlinie, über dem kleinen stehen blaue Funkwellen. Das zeigt Mehrraum (zwei Geräte), kabellos (Wellen) und Audio (Membranen), und es erinnert an die schwarzen und weißen Sonos-Boxen, ohne eine davon nachzubauen. Bleibt bis 32 px lesbar. Symbolisch: zwei Lautsprecher, ein Funkbogen.

## Entwurf B: Grundriss mit drei Räumen

Draufsicht auf einen Grundriss mit drei farbigen Räumen, in jedem pulsiert ein Lautsprecher in konzentrischen Ringen. Das ist die direkteste Darstellung von Multiroom und fällt im App-Raster auf. Schwächen: Bei 32 px wird es zum bunten Flickenteppich, und man liest eher „Smart Home“ als „Musik“.

## Entwurf C: Handcontroller

Ein heller Controller mit Display (Cover, Titel, Fortschrittsbalken) und einem Scrollrad mit blauer Play-Taste, eine Verbeugung vor dem CR100, den Gnomos ersetzt. Das trifft die Rolle der App (Fernbedienung) am genauesten. Schwächen: Ohne Sonos-Vorwissen liest man einen iPod, und Mehrraum und Funk fehlen.

## Runde 2: Varianten von A

Alle drei nutzen das helle Gehäuse `#5e5c64` mit dem Profil `#3d3846`, damit das Icon auch auf dunklem Grund steht. Die Wellen sind blau `#3584e4`.

- **A1 – ein Lautsprecher:** Ein einzelner Lautsprecher, aus dessen Oberseite die Funkwellen aufsteigen. Das ist am ruhigsten und am besten lesbar, Mehrraum zeigen nur noch die Wellen.
- **A2 – Gruppe:** Der helle Lautsprecher steht hinter dem dunklen, beide bilden einen gemeinsamen Umriss. Das ist näher an Runde 1, aber nicht mehr getrennt.
- **A3 – breiter Lautsprecher:** Ein Gehäuse mit zwei Membranen (Stereo), die Wellen mittig darüber. Es ist kompakt und breit, erinnert aber etwas an ein Kofferradio.

## Runde 3: Wellen als Teil des Lautsprechers

- **B1 – Front:** Der Hochtöner ist der Sender, die drei Funkbögen liegen hellblau auf der Front, darunter die Membran. Das liest man als „kabelloser Lautsprecher“ und es bleibt bis 32 px klar. Das Symbol ist eine Box mit ausgestanztem Bogen und Membran.
- **B2 – Ringe:** Von der Membran breiten sich Schallringe über die ganze Front aus und werden am Gehäuse abgeschnitten, nach außen dünner und dunkler. Das ist kompakt und kräftig, aber ohne Hochtöner nah an einer Zielscheibe.
- **B3 – Boden:** Der Lautsprecher steht in Wellenringen, die sich in Draufsicht über den Boden ausbreiten, so als würde der Klang den Raum füllen. Das ist die räumlichste Lösung und passt zu Mehrraum, nutzt aber die Breite für eine flache Form.

## Runde 4: Varianten von B3

- **C1 – Scheibe:** Der Lautsprecher steht auf einer runden Scheibe mit eigenem Profil, die Ringe sind flache Blautöne auf ihrer Oberseite. Das hält sich am strengsten an die HIG (Draufsicht und Profil, flache Flächen) und ist das kräftigste der drei Icons, wirkt aber eher wie ein Sockel als wie Wellen.
- **C2 – auslaufend:** Die Ringe werden nach außen dünner und heller, der Lautsprecher ist etwas schlanker. Das ist am nächsten an B3 und zeigt die Bewegung am deutlichsten.
- **C3 – hell:** Ein heller, runder Lautsprecher mit blauer Statusleuchte statt Hochtöner, in den Wellenringen von B3. Er ist freundlicher und erinnert an aktuelle WLAN-Lautsprecher. Auf hellem Grund trägt ihn nur das Profil.

Die symbolischen Icons sind in dieser Runde nur Platzhalter (abgeleitet von B3), sie werden nach der Auswahl fertig gezeichnet.

## Umsetzung

- `data/icons/hicolor/scalable/apps/de.linuxundich.Gnomos.svg` und `data/icons/hicolor/symbolic/apps/de.linuxundich.Gnomos-symbolic.svg`. Das symbolische Icon zeigt den Lautsprecher mit ausgestanzter Membran auf der Scheibe, die um den Lautsprecher 1 px freigestellt ist.
- Zusätzlich PNGs in 48/64/128/256 px, aus dem SVG gerendert, damit GNOME Shell und Flatpaks Icon-Prüfung auch ohne gdk-pixbuf-Loader für SVG auskommen. Das Flatpak-Manifest rendert deshalb kein eigenes PNG mehr und löscht das SVG nicht mehr.
- Änderungen am Icon nur in `generate_icons.py` vornehmen und das Skript neu laufen lassen.
