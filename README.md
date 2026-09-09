# nanocvs-c

Native C implementation of a small **Fossil-inspired** local change tracker.

It keeps a single SQLite repository, stores content-addressed file artifacts, and records immutable file change events across tracked roots.

## For LLMs

If you are an LLM or agent using this tool, treat it as:
- a small Fossil-inspired local history tracker, not a full Git replacement
- a single-repo SQLite index of scanned file states and immutable change events
- a low-level primitive for `scan`, `log`, `cat`, `restore`, `revert`, and `diff`

Operational model:
- `scan` records file changes under compiled tracked roots
- `log` shows change-event ids; those ids are the main revision handles
- `REV^` means the before-side of a change event
- `restore` and `diff` operate on stored content from that event history

Do not assume:
- branches, merges, commits, remotes, staging, or Git-compatible semantics
- that `config.json` is authoritative; tracked roots are compiled from `config.h`

## Philosophy

Following the **suckless** and **SQLite/Fossil** spirit:
- **Minimalist:** Small C codebase, minimal dependencies (system SQLite).
- **Simple:** Focus on a few powerful primitives rather than complex workflows.
- **Robust:** Content-addressed storage, atomic restores, and immutable history.
- **Unix-like:** Tools should be composable; prefer simple text/JSON outputs over built-in "magic" commands.

## Current status

Working native C prototype with usable commands for:
- `roots`, `init`, `status`, `scan`, `log`, `week`, `cat`, `restore`, `revert`, `diff`

Current focus is polish and edge-case tightening rather than major missing commands.

## Milestone status

This is now in a solid **v0.1 / first usable native milestone** state on the current host:
- native C binary builds cleanly
- repository init/scan/status work against the C-local SQLite repo
- history inspection commands (`log`, `week`) are usable
- content inspection (`cat`) works by hash or path + change id
- restore is atomic and post-write hash-verified
- revert supports scan-id rollback of a single scan batch
- diff supports unified output, `-U/--unified`, and `--stat`

## Host assumptions

This milestone is intentionally host-bound today:
- Linux-oriented build/runtime assumptions (`/proc/self/exe` is used to locate the DB beside the binary)
- system SQLite headers/libs must be available to build
- tracked roots are compiled into `config.h`; `config.json` is not authoritative yet
- the default DB is `nanocvs.db` in the same directory as the built `nanocvs-c` binary

## Build

Build natively on the current host (requires SQLite dev headers):

```sh
make
```

Run the host-local smoke test:

```sh
./smoke.sh
```

The smoke script writes, modifies, restores, and deletes a real temporary file under the compiled tracked roots and uses `/tmp` for scratch outputs. Only run it where that tracked location is safe for test writes.

## PHP browser on LOQ

Keep the served page linked to the Git checkout so it updates with a pull.
Clone this repository to `~/github/nanocvs-c`, preserve any changes in an existing
`/var/www/html/cvs.php`, then link it:

```sh
ln -sfn "$HOME/github/nanocvs-c/web/cvs.php" /var/www/html/cvs.php
```

Update the checkout with:

```sh
git -C "$HOME/github/nanocvs-c" pull --ff-only
```

The web server must be able to read the checkout and follow the symlink. This
links only the PHP browser; its configured binary and database remain at
`/home/nybo/services/nanocvs-c`. Pulling updates the live page immediately.
Edit `web/cvs.php` in the checkout and commit/push those edits rather than
creating another deployed copy. The existing LOQ setup is documented in
[RECOVERY.md](RECOVERY.md).

## Command overview

```sh
./nanocvs-c help
./nanocvs-c --version
./nanocvs-c roots
./nanocvs-c status
./nanocvs-c scan
./nanocvs-c log [-n N|--limit N] [--path RELPATH]
./nanocvs-c cat PATH [REV]
./nanocvs-c restore PATH [REV] [--to DEST]
./nanocvs-c revert SCAN_ID
./nanocvs-c diff PATH REV
```

## Change ids and revisions

`log` prints a leading change-event id in brackets: `[12]`.
Use this `REV` in `cat`, `diff`, and `restore`. `REV^` refers to the **before** side of a change.

## LLM hooks / extensions

In suckless terms, this should stay an optional **patch / hook layer**, not core product logic.

If you want LLM integration, prefer simple external hooks such as:
- `pre-scan` hook: snapshot context or attach an operator tag before `scan`
- `post-scan` hook: inspect the latest `scan_id`, `log`, or `week` output and summarize changes
- `pre-restore` hook: ask for confirmation or produce a dry-run style explanation
- `post-revert` hook: summarize which files were restored, deleted, or still need review

Recommended style:
- keep `nanocvs-c` itself as the small local-history primitive
- let hooks be normal shell scripts or wrappers that call `nanocvs-c`
- pass plain text / JSON between the hook and the LLM
- treat LLM behavior as advisory around the primitive, not embedded in the primitive

In other words: the suckless name for this is closer to a **patch**, **wrapper**, or **hook script** than a built-in AI subsystem.

## Files

- `main.c` - command dispatch and help
- `nanocvs.h` - shared declarations
- `db.c` - DB/schema helpers
- `scan.c` - scanner and change detection
- `history.c` - `log` and `week`
- `commands.c` - `cat` and `restore`
- `diff.c` - line-based diff helpers
- `util.c` - misc helpers and file I/O utilities
- `sha256.c` / `sha256.h` - embedded SHA-256
- `Makefile` - native build

## TODO / Future LLM Improvements (Case: 2026-04-07 Rollback)

**Context:**
A multi-file refactor (15+ files) on 2026-04-07 caused accidental deletions (`navbar.php`) and corruptions (`cvs.php`). Rolling back required manual identification and restoration of every affected path.

**Architectural Goal:**
Keep it "suckless." Avoid bloat. Prefer composable primitives over complex high-level commands.

**Suggested minimalist improvements:**
- **Revert by Scan ID:** A simple command to revert all changes associated with a specific `scan_id`. This is the "suckless" way to handle batch rollbacks since `scan` groups related changes.
- **Improved Output for Scripting:** Add a `--raw` or `--csv` flag to `log` so standard unix tools (`awk`, `cut`, `grep`) can easily parse and act on the history without needing `jq`.
- **Pre-flight Checkpoints:** A lightweight `checkpoint` (essentially a named scan or a manual scan-id alias) to mark the state before a risky LLM operation.
- **Safety:** Add a `--dry-run` to `restore` when handling multiple files.
