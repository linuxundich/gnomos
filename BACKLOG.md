# Backlog

Open items. Shipped changes are in [CHANGELOG.md](CHANGELOG.md).

## Bugs

- [ ] **Album tile briefly shows the placeholder cover.** In a freshly logged-in
      English session (Flatpak 0.28.5, `LANGUAGE=en`), filtering the albums
      for "bread" showed the generated placeholder ("B" on purple) for about
      3 s before the real cover – in two takes in a row, even after the library
      had been loaded once. A German session shortly before showed the real
      cover immediately. Not investigated yet; suspects: cover cache or fetch
      after a new login, or a language-dependent cache key. Noticed
      2026-10-05 while recording the English demo video.
