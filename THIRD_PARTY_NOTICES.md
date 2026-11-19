# Third-party notices

The private C++ engine embeds checksum-pinned DwarFS 0.15.7. Its exact release
source archive and the local extractor patches are shipped in
`/usr/share/dwarfsctl/sources/`; copyright and
license texts are in `/usr/share/licenses/dwarfsctl/`. See
`CONTRIBUTING.md` for the source and redistribution boundary. The download URL
and integrity pin are recorded in `packaging/build-reader.sh`. The patches add
an opt-in extractor option to preserve selected empty directories and reject
unsafe stored filename components before extraction writes begin.

DwarFS common, reader, decompressor and extractor libraries are MIT-licensed.
Writer/rewrite and relevant tool code are GPL-3.0-or-later. The combined
application is GPL-3.0-or-later. Bundled upstream components include ricepp, FSST
and frozen/Thrift; retain their per-file notices and licenses in the source
archive. Release bundles include corresponding application source and scripts.

Header dependencies include Boost, nlohmann JSON, range-v3, parallel-hashmap,
utf8cpp and toml++. Their distribution notices are collected in the package.
Qt 6 and KDE Frameworks 6 are shared distribution dependencies with their
respective LGPL/GPL/MIT terms. Compression libraries, OpenSSL, libarchive,
libfuse and coreutils are also supplied by the distribution, with its copyright
and source packages. The app does not ship external DwarFS executables.

DwarFS is a separate project by Marcus Holland-Moritz and contributors. This
unofficial KDE integration is not affiliated with or endorsed by DwarFS.
