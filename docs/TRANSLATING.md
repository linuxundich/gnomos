# Translating Gnomos

The interface is written in English and translated with gettext; German
is included in `po/de.po`. To add a language, add its code to
`po/LINGUAS`, create the `.po` file from `po/gnomos.pot`
(`msginit -i po/gnomos.pot -o po/xx.po -l xx`) and translate it. After
changing strings in the code, `ninja -C _build gnomos-pot` and
`ninja -C _build gnomos-update-po` refresh the template and the
translations. To try a translation without installing:
`GNOMOS_LOCALEDIR=_build/po _build/src/gnomos`.
