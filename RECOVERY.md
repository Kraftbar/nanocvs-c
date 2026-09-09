# Source recovery — 2026-09-09

This repository preserves the C source from LOQ's
`/home/nybo/services/nanocvs-c` and its PHP browser from
`/var/www/html/cvs.php` (stored here as `web/cvs.php`). Application source is
unchanged. The old SQLite history database is intentionally not imported.
Compiled binaries, object files and temporary smoke-test outputs are excluded.

`config.def.h` preserves the existing host's default roots. `make` copies it
to ignored `config.h` if absent. Edit `config.h` for another installation and
perform a clean build after changing it. `config.json` is retained historical
configuration; the program does not use it to choose tracked roots.

The existing `smoke.sh` contains fixed LOQ paths and its test directory differs
from the shipped tracked roots. Do not use it as a portable test runner.
Recovery validation uses a separate disposable build, scratch tracked directory
and fresh database; it does not scan or restore the live server's files.

The PHP browser's binary/database paths are currently fixed to the LOQ
installation. `web/cvs.php` is the versioned source; deploying that file to
`/var/www/html/cvs.php` is a separate operation. Merely cloning this repository
does not replace the running binary, web page or history database.
