#include "nanocvs.h"
#include "config.h"

#ifndef NANOCVS_VERSION
#define NANOCVS_VERSION "0.1"
#endif

typedef int (*SubcmdFn)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    SubcmdFn fn;
} CommandInfo;

static int cmd_roots_wrap(int argc, char **argv) {
    int i = 0;
    (void)argc; (void)argv;
    while (TRACKED_ROOTS[i]) { printf("%s\n", TRACKED_ROOTS[i]); i++; }
    return NANOCVS_OK;
}
static int cmd_init_wrap(int argc, char **argv) { (void)argc; (void)argv; return cmd_init(); }
static int cmd_status_wrap(int argc, char **argv) { return cmd_status(argc, argv); }
static int cmd_scan_wrap(int argc, char **argv) { return cmd_scan(argc, argv); }
static int cmd_log_wrap(int argc, char **argv) { return cmd_log(argc, argv); }
static int cmd_week_wrap(int argc, char **argv) { return cmd_week(argc, argv); }
static int cmd_cat_wrap(int argc, char **argv) { return cmd_cat(argc, argv); }
static int cmd_restore_wrap(int argc, char **argv) { return cmd_restore(argc, argv); }
static int cmd_revert_wrap(int argc, char **argv) { return cmd_revert(argc, argv); }
static int cmd_diff_wrap(int argc, char **argv) { return cmd_diff(argc, argv); }

static const CommandInfo COMMANDS[] = {
    {"roots",   "roots",                                 "list tracked root directories", cmd_roots_wrap},
    {"init",    "init",                                  "initialize the repository database", cmd_init_wrap},
    {"status",  "status",                                "show repository summary and last scan", cmd_status_wrap},
    {"scan",    "scan",                                  "scan tracked roots and record changes", cmd_scan_wrap},
    {"log",     "log [-n N|--limit N] [--path RELPATH|RELPATH]", "show recent change events", cmd_log_wrap},
    {"week",    "week [--days N] [--path RELPATH|RELPATH]",      "summarize recent activity by day", cmd_week_wrap},
    {"cat",     "cat <HASH|PATH> [REV]",                 "print stored content by hash or path plus change-event id", cmd_cat_wrap},
    {"restore", "restore PATH [REV] [--to DEST]",        "restore stored file content to disk", cmd_restore_wrap},
    {"revert",  "revert SCAN_ID",                        "revert all changes in a specific scan", cmd_revert_wrap},
    {"diff",    "diff [--stat] [-U N|--unified N] PATH REV", "show a compact unified diff for PATH at a change-event id", cmd_diff_wrap},
};

static const size_t COMMAND_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

static const CommandInfo *find_command(const char *name) {
    size_t i;
    for (i = 0; i < COMMAND_COUNT; ++i) {
        if (strcmp(COMMANDS[i].name, name) == 0) return &COMMANDS[i];
    }
    return NULL;
}

static void usage(FILE *out, const char *argv0) {
    size_t i;
    fprintf(out, "Usage:\n");
    fprintf(out, "  %s help [COMMAND]\n", argv0);
    fprintf(out, "  %s --version\n", argv0);
    fprintf(out, "  %s --help\n", argv0);
    fprintf(out, "  %s -h\n", argv0);
    for (i = 0; i < COMMAND_COUNT; ++i) {
        fprintf(out, "  %s %s\n", argv0, COMMANDS[i].usage);
    }
}

static void help_all(FILE *out, const char *argv0) {
    size_t i;
    fprintf(out, "nanocvs-c: a small Fossil-inspired local change tracker.\n");
    fprintf(out, "It is not a full VCS clone. It keeps a single SQLite repo of scanned file history,\n");
    fprintf(out, "content-addressed artifacts, immutable change events, and simple restore/diff primitives.\n\n");
    usage(out, argv0);
    fprintf(out, "\nCommands:\n");
    for (i = 0; i < COMMAND_COUNT; ++i) {
        fprintf(out, "  %-8s %s\n", COMMANDS[i].name, COMMANDS[i].summary);
    }
}

static int help_one(FILE *out, FILE *err, const char *argv0, const char *name) {
    const CommandInfo *cmd = find_command(name);
    if (!cmd) {
        fprintf(err, "unknown command: %s\n\n", name);
        help_all(err, argv0);
        return 1;
    }
    fprintf(out, "Usage:\n  %s %s\n\n%s\n", argv0, cmd->usage, cmd->summary);
    if (strcmp(name, "cat") == 0) {
        fprintf(out, "\nNotes:\n  REV is a change-event id shown by `log`. Use REV^ for the before-side when available.\n");
    } else if (strcmp(name, "restore") == 0) {
        fprintf(out, "\nNotes:\n  Restores atomically. If DEST already exists, a DEST.bak backup is written first.\n");
    } else if (strcmp(name, "revert") == 0) {
        fprintf(out, "\nNotes:\n  Undoes all changes from a single scan. Deletes newly created files and restores previous content for modified/deleted files.\n");
    } else if (strcmp(name, "log") == 0) {
        fprintf(out, "\nNotes:\n  The leading [id] is the change-event id you can pass to `cat`, `diff`, or `restore`.\n");
    } else if (strcmp(name, "week") == 0) {
        fprintf(out, "\nNotes:\n  Defaults to the last 7 days. Use --path RELPATH to restrict to one file.\n");
    } else if (strcmp(name, "diff") == 0) {
        fprintf(out, "\nNotes:\n  REV is a change-event id shown by `log`. This prints a compact unified diff between the before and after sides of that change. Use `-U N` or `--unified N` to change context lines. Use `--stat` to print only the per-event line-count summary.\n");
    }
    return 0;
}

int main(int argc, char **argv) {
    const CommandInfo *cmd;

    nanocvs_resolve_paths();

    /* --ro flag: open DB read-only (safe for web/untrusted callers) */
    if (argc >= 2 && strcmp(argv[argc - 1], "--ro") == 0) {
        nanocvs_readonly = 1;
        argc--;
    }

    if (argc < 2) {
        help_all(stderr, argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "help") == 0) {
        if (argc == 2) {
            help_all(stdout, argv[0]);
            return 0;
        }
        if (argc == 3) {
            return help_one(stdout, stderr, argv[0], argv[2]);
        }
        fprintf(stderr, "Usage: %s help [COMMAND]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        help_all(stdout, argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("nanocvs-c %s\n", NANOCVS_VERSION);
        return 0;
    }

    cmd = find_command(argv[1]);
    if (cmd) return cmd->fn(argc, argv);

    fprintf(stderr, "unknown command: %s\n\n", argv[1]);
    help_all(stderr, argv[0]);
    return 1;
}
