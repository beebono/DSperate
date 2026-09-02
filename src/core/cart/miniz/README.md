Vendored from https://github.com/richgel999/miniz -- miniz 3.1.2, commit
77d0dce8627735138c51770d1799a1ef48f2117d.

Only the DEFLATE *decompression* core is compiled: miniz_tinfl.c. The other
files here are headers it needs, because miniz.h unconditionally includes the
whole family; miniz_tdef.c (the compressor) and miniz_zip.c (miniz's own ZIP
reader) are not vendored and never linked, so the archive code in ../zip.cpp
is ours and bounded to what a DS ROM archive needs.

miniz_export.h is not upstream -- upstream generates it from its own CMake
build. See the note in it.

Built with MINIZ_NO_DEFLATE_APIS, MINIZ_NO_ARCHIVE_APIS, MINIZ_NO_STDIO and
MINIZ_NO_TIME (set in ../../CMakeLists.txt).

MIT licensed (see LICENSE), compatible with this project's GPL-3.0-or-later.
Do not hand-edit: re-copy from a newer upstream tag and update the commit
recorded above.
