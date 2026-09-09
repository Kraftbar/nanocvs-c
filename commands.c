#include "nanocvs.h"

static int print_artifact_content(sqlite3 *db, const char *hash) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    rc = sqlite3_prepare_v2(db,
        "SELECT content, size_bytes FROM artifact WHERE hash=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        fprintf(stderr, "not found\n");
        return NANOCVS_ERR;
    }
    {
        const void *content = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        if (size > 0 && fwrite(content, 1, (size_t)size, stdout) != (size_t)size) {
            sqlite3_finalize(stmt);
            fprintf(stderr, "write failed\n");
            return NANOCVS_ERR;
        }
    }
    sqlite3_finalize(stmt);
    return NANOCVS_OK;
}

static int parse_change_revision(const char *revision, sqlite3_int64 *change_id_out, int *before_side_out) {
    sqlite3_int64 change_id = 0;
    char *end = NULL;

    if (!revision || !*revision) return NANOCVS_ERR;
    change_id = strtoll(revision, &end, 10);
    if (!end || (*end != '\0' && strcmp(end, "^") != 0) || change_id <= 0) {
        return NANOCVS_ERR;
    }
    if (change_id_out) *change_id_out = change_id;
    if (before_side_out) *before_side_out = (end && strcmp(end, "^") == 0) ? 1 : 0;
    return NANOCVS_OK;
}

static int lookup_hash_for_path(sqlite3 *db, const char *relpath, const char *revision, char hash_out[65]) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    const char *sql_current =
        "SELECT fs.current_hash "
        "FROM path p JOIN file_state fs ON fs.path_id=p.id "
        "WHERE p.relpath=? AND fs.exists_now=1 "
        "ORDER BY p.id DESC LIMIT 1";
    const char *sql_latest_event =
        "SELECT ce.after_hash "
        "FROM change_event ce JOIN path p ON p.id=ce.path_id "
        "WHERE p.relpath=? "
        "ORDER BY ce.id DESC LIMIT 1";
    const char *sql_revision =
        "SELECT CASE ? "
        "WHEN '0' THEN ce.before_hash "
        "ELSE ce.after_hash END "
        "FROM change_event ce JOIN path p ON p.id=ce.path_id "
        "WHERE p.relpath=? AND ce.id=? LIMIT 1";

    hash_out[0] = '\0';
    if (!revision) {
        rc = sqlite3_prepare_v2(db, sql_current, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return NANOCVS_ERR;
        sqlite3_bind_text(stmt, 1, relpath, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW && sqlite3_column_text(stmt, 0)) {
            snprintf(hash_out, 65, "%s", (const char *)sqlite3_column_text(stmt, 0));
            sqlite3_finalize(stmt);
            return NANOCVS_OK;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
        rc = sqlite3_prepare_v2(db, sql_latest_event, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return NANOCVS_ERR;
        sqlite3_bind_text(stmt, 1, relpath, -1, SQLITE_TRANSIENT);
    } else {
        const char *phase;
        sqlite3_int64 change_id = 0;
        int before_side = 0;
        if (parse_change_revision(revision, &change_id, &before_side) != NANOCVS_OK) {
            fprintf(stderr, "bad revision\n");
            return NANOCVS_ERR;
        }
        phase = before_side ? "0" : "1";
        rc = sqlite3_prepare_v2(db, sql_revision, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return NANOCVS_ERR;
        sqlite3_bind_text(stmt, 1, phase, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, relpath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, change_id);
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW && sqlite3_column_text(stmt, 0)) {
        snprintf(hash_out, 65, "%s", (const char *)sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
        return NANOCVS_OK;
    }
    sqlite3_finalize(stmt);
    fprintf(stderr, "not found\n");
    return NANOCVS_ERR;
}

static int restore_to_path(sqlite3 *db, const char *relpath, const char *dest_path, const char *revision) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    char hash[65];
    char actual_hash[65];
    char backup_path[PATH_MAX];
    hash[0] = '\0';
    actual_hash[0] = '\0';
    backup_path[0] = '\0';
    if (lookup_hash_for_path(db, relpath, revision, hash) != NANOCVS_OK) return NANOCVS_ERR;

    rc = sqlite3_prepare_v2(db,
        "SELECT content, size_bytes FROM artifact WHERE hash=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        fprintf(stderr, "not found\n");
        return NANOCVS_ERR;
    }

    {
        const void *content = sqlite3_column_blob(stmt, 0);
        int size = sqlite3_column_bytes(stmt, 0);
        size_t verify_size = 0;
        int verify_lines = 0;
        char *written = NULL;

        if (write_file_atomic(dest_path, content, (size_t)size, 1, backup_path, sizeof(backup_path)) != NANOCVS_OK) {
            sqlite3_finalize(stmt);
            fprintf(stderr, "restore failed\n");
            return NANOCVS_ERR;
        }
        written = read_file_text(dest_path, &verify_size, &verify_lines);
        (void)verify_lines;
        if (!written) {
            sqlite3_finalize(stmt);
            fprintf(stderr, "restore verify failed\n");
            return NANOCVS_ERR;
        }
        sha256_text_hex(written, verify_size, actual_hash);
        free(written);
        if (strcmp(actual_hash, hash) != 0) {
            sqlite3_finalize(stmt);
            fprintf(stderr, "restore verify mismatch\n");
            return NANOCVS_ERR;
        }
    }
    sqlite3_finalize(stmt);
    if (backup_path[0]) printf("restored %s (backup %s)\n", dest_path, backup_path);
    else printf("restored %s\n", dest_path);
    return NANOCVS_OK;
}

int cmd_cat(int argc, char **argv) {
    sqlite3 *db = NULL;
    int rc;
    char hash[65];
    if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        printf("Usage: %s cat <HASH|PATH> [REV]\n", argv[0]);
        printf("Print stored content by hash or path plus change-event id.\n");
        return NANOCVS_OK;
    }
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s cat <HASH|PATH> [REV]\n", argv[0]);
        fprintf(stderr, "Try '%s help cat' for details.\n", argv[0]);
        return NANOCVS_ERR;
    }
    if (db_open(&db) != NANOCVS_OK) return NANOCVS_ERR;
    if (argc == 3 && strlen(argv[2]) == 64) {
        rc = print_artifact_content(db, argv[2]);
    } else {
        rc = lookup_hash_for_path(db, argv[2], argc == 4 ? argv[3] : NULL, hash);
        if (rc == NANOCVS_OK) rc = print_artifact_content(db, hash);
    }
    sqlite3_close(db);
    return rc;
}

static char *artifact_text_by_hash(sqlite3 *db, const char *hash, size_t *size_out) {
    sqlite3_stmt *stmt = NULL;
    char *buf = NULL;
    int rc;
    if (size_out) *size_out = 0;
    if (!hash || !*hash) {
        buf = malloc(1);
        if (!buf) return NULL;
        buf[0] = '\0';
        return buf;
    }
    rc = sqlite3_prepare_v2(db,
        "SELECT content FROM artifact WHERE hash=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int n = sqlite3_column_bytes(stmt, 0);
        const void *blob = sqlite3_column_blob(stmt, 0);
        buf = malloc((size_t)n + 1);
        if (buf) {
            if (n > 0 && blob) memcpy(buf, blob, (size_t)n);
            buf[n] = '\0';
            if (size_out) *size_out = (size_t)n;
        }
    }
    sqlite3_finalize(stmt);
    return buf;
}

typedef struct {
    char *text;
    int has_newline;
} OwnedLine;

static int split_text_lines_owned(const char *text, OwnedLine **lines_out, int *count_out) {
    const char *p = text ? text : "";
    const char *start = p;
    OwnedLine *lines = NULL;
    int count = 0;
    int cap = 0;

    while (*p) {
        if (*p == '\n') {
            size_t len = (size_t)(p - start);
            char *line;
            if (count == cap) {
                int newcap = cap ? cap * 2 : 32;
                OwnedLine *n = realloc(lines, sizeof(OwnedLine) * (size_t)newcap);
                if (!n) goto oom;
                lines = n;
                cap = newcap;
            }
            line = malloc(len + 1);
            if (!line) goto oom;
            memcpy(line, start, len);
            line[len] = '\0';
            lines[count].text = line;
            lines[count].has_newline = 1;
            count++;
            start = p + 1;
        }
        p++;
    }
    if (p != start) {
        size_t len = (size_t)(p - start);
        char *line;
        if (count == cap) {
            int newcap = cap ? cap * 2 : 32;
            OwnedLine *n = realloc(lines, sizeof(OwnedLine) * (size_t)newcap);
            if (!n) goto oom;
            lines = n;
            cap = newcap;
        }
        line = malloc(len + 1);
        if (!line) goto oom;
        memcpy(line, start, len);
        line[len] = '\0';
        lines[count].text = line;
        lines[count].has_newline = 0;
        count++;
    }

    *lines_out = lines;
    *count_out = count;
    return NANOCVS_OK;

oom:
    if (lines) {
        int i;
        for (i = 0; i < count; ++i) free(lines[i].text);
        free(lines);
    }
    return NANOCVS_ERR;
}

static void free_owned_lines(OwnedLine *lines, int count) {
    int i;
    if (!lines) return;
    for (i = 0; i < count; ++i) free(lines[i].text);
    free(lines);
}

typedef struct {
    char tag;
    const OwnedLine *line;
} DiffOp;

static int build_diff_ops(OwnedLine *before_lines, int before_count, OwnedLine *after_lines, int after_count,
    DiffOp **ops_out, int *op_count_out) {
    int *lcs = NULL;
    DiffOp *ops = NULL;
    int op_cap = 0;
    int op_count = 0;
    int i, j;

    #define LCS_AT(r, c) lcs[(r) * (after_count + 1) + (c)]

    lcs = calloc((size_t)(before_count + 1) * (size_t)(after_count + 1), sizeof(int));
    if (!lcs) return NANOCVS_ERR;

    for (i = before_count - 1; i >= 0; --i) {
        for (j = after_count - 1; j >= 0; --j) {
            if (strcmp(before_lines[i].text, after_lines[j].text) == 0 &&
                before_lines[i].has_newline == after_lines[j].has_newline) {
                LCS_AT(i, j) = 1 + LCS_AT(i + 1, j + 1);
            } else {
                LCS_AT(i, j) = LCS_AT(i + 1, j) > LCS_AT(i, j + 1) ? LCS_AT(i + 1, j) : LCS_AT(i, j + 1);
            }
        }
    }

    i = 0;
    j = 0;
    while (i < before_count || j < after_count) {
        DiffOp op;
        if (op_count == op_cap) {
            int newcap = op_cap ? op_cap * 2 : 64;
            DiffOp *n = realloc(ops, sizeof(DiffOp) * (size_t)newcap);
            if (!n) {
                free(lcs);
                free(ops);
                return NANOCVS_ERR;
            }
            ops = n;
            op_cap = newcap;
        }
        if (i < before_count && j < after_count &&
            strcmp(before_lines[i].text, after_lines[j].text) == 0 &&
            before_lines[i].has_newline == after_lines[j].has_newline) {
            op.tag = ' ';
            op.line = &before_lines[i];
            i++;
            j++;
        } else if (j >= after_count || (i < before_count && LCS_AT(i + 1, j) >= LCS_AT(i, j + 1))) {
            op.tag = '-';
            op.line = &before_lines[i++];
        } else {
            op.tag = '+';
            op.line = &after_lines[j++];
        }
        ops[op_count++] = op;
    }

    free(lcs);
    *ops_out = ops;
    *op_count_out = op_count;
    return NANOCVS_OK;

    #undef LCS_AT
}

static void print_unified_range(int start, int count) {
    if (count == 0) printf("%d,0", start - 1);
    else if (count == 1) printf("%d", start);
    else printf("%d,%d", start, count);
}

static int diff_ops_have_changes(DiffOp *ops, int op_count) {
    int i;
    for (i = 0; i < op_count; ++i) {
        if (ops[i].tag != ' ') return 1;
    }
    return 0;
}

static void print_unified_hunks(DiffOp *ops, int op_count, int context) {
    int i = 0;
    int old_line = 1;
    int new_line = 1;

    while (i < op_count) {
        int first_change = -1;
        int start, end, k;
        int hunk_old_start, hunk_new_start, hunk_old_count, hunk_new_count;
        int walk_old, walk_new;

        while (i < op_count && ops[i].tag == ' ') {
            old_line++;
            new_line++;
            i++;
        }
        if (i >= op_count) break;
        first_change = i;
        start = first_change - context;
        if (start < 0) start = 0;
        end = first_change;

        walk_old = old_line;
        walk_new = new_line;
        for (k = first_change; k < op_count; ++k) {
            if (ops[k].tag == ' ') {
                int unchanged_run = 0;
                int t = k;
                while (t < op_count && ops[t].tag == ' ') {
                    unchanged_run++;
                    if (unchanged_run > context) break;
                    t++;
                }
                if (unchanged_run > context) {
                    end = k + context;
                    break;
                }
            }
            end = k + 1;
        }
        if (end > op_count) end = op_count;

        hunk_old_start = old_line;
        hunk_new_start = new_line;
        for (k = first_change - 1; k >= start; --k) {
            if (ops[k].tag != '+') hunk_old_start--;
            if (ops[k].tag != '-') hunk_new_start--;
        }

        hunk_old_count = 0;
        hunk_new_count = 0;
        for (k = start; k < end; ++k) {
            if (ops[k].tag != '+') hunk_old_count++;
            if (ops[k].tag != '-') hunk_new_count++;
        }

        printf("@@ -");
        print_unified_range(hunk_old_start, hunk_old_count);
        printf(" +");
        print_unified_range(hunk_new_start, hunk_new_count);
        printf(" @@\n");
        for (k = start; k < end; ++k) {
            printf("%c%s\n", ops[k].tag, ops[k].line->text);
            if ((ops[k].tag == '-' || ops[k].tag == '+') && !ops[k].line->has_newline) {
                printf("\\ No newline at end of file\n");
            }
        }

        for (k = i; k < end; ++k) {
            if (ops[k].tag != '+') walk_old++;
            if (ops[k].tag != '-') walk_new++;
        }
        old_line = walk_old;
        new_line = walk_new;
        i = end;
    }
}

static int print_simple_diff(sqlite3 *db, const char *relpath, const char *revision, int context, int stat_only) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    sqlite3_int64 change_id = 0;
    const char *before_hash;
    const char *after_hash;
    char action_buf[32];
    char *before_text = NULL;
    char *after_text = NULL;
    OwnedLine *before_lines = NULL;
    OwnedLine *after_lines = NULL;
    DiffOp *ops = NULL;
    int op_count = 0;
    int before_count = 0;
    int after_count = 0;
    size_t ignored_size = 0;
    LineDiffCounts counts;

    if (!revision) {
        fprintf(stderr, "Usage: nanocvs-c diff [--stat] [-U N|--unified N] PATH REV\n");
        fprintf(stderr, "Try 'nanocvs-c help diff' for details.\n");
        return NANOCVS_ERR;
    }
    if (context < 0) context = 0;
    if (parse_change_revision(revision, &change_id, NULL) != NANOCVS_OK) {
        fprintf(stderr, "invalid revision '%s' (expected change-event id like 12 or 12^)\n", revision);
        return NANOCVS_ERR;
    }

    rc = sqlite3_prepare_v2(db,
        "SELECT ce.id, ce.action, ce.before_hash, ce.after_hash "
        "FROM change_event ce JOIN path p ON p.id=ce.path_id "
        "WHERE p.relpath=? AND ce.id=? LIMIT 1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, relpath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, change_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        fprintf(stderr, "revision %s not found for path %s\n", revision, relpath);
        return NANOCVS_ERR;
    }
    snprintf(action_buf, sizeof(action_buf), "%s",
        sqlite3_column_text(stmt, 1) ? (const char *)sqlite3_column_text(stmt, 1) : "?");
    before_hash = sqlite3_column_text(stmt, 2) ? (const char *)sqlite3_column_text(stmt, 2) : NULL;
    after_hash = sqlite3_column_text(stmt, 3) ? (const char *)sqlite3_column_text(stmt, 3) : NULL;

    before_text = artifact_text_by_hash(db, before_hash, &ignored_size);
    after_text = artifact_text_by_hash(db, after_hash, &ignored_size);
    sqlite3_finalize(stmt);

    if (!before_text || !after_text) {
        free(before_text);
        free(after_text);
        fprintf(stderr, "artifact load failed for diff\n");
        return NANOCVS_ERR;
    }

    if (line_diff_count(before_text, after_text, &counts) != NANOCVS_OK) {
        free(before_text);
        free(after_text);
        fprintf(stderr, "diff failed\n");
        return NANOCVS_ERR;
    }

    if (stat_only) {
        printf("diff %s [" SQLITE_INT64_PRINTF "] action=%s +%d -%d\n",
            relpath, SQLITE_INT64_ARG(change_id), action_buf, counts.added, counts.deleted);
        free(before_text);
        free(after_text);
        return NANOCVS_OK;
    }

    if (split_text_lines_owned(before_text, &before_lines, &before_count) != NANOCVS_OK ||
        split_text_lines_owned(after_text, &after_lines, &after_count) != NANOCVS_OK ||
        build_diff_ops(before_lines, before_count, after_lines, after_count, &ops, &op_count) != NANOCVS_OK) {
        free(before_text);
        free(after_text);
        free_owned_lines(before_lines, before_count);
        free_owned_lines(after_lines, after_count);
        free(ops);
        fprintf(stderr, "diff failed\n");
        return NANOCVS_ERR;
    }

    (void)change_id;
    (void)action_buf;
    printf("--- %s\n", relpath);
    printf("+++ %s\n", relpath);

    if (diff_ops_have_changes(ops, op_count)) {
        print_unified_hunks(ops, op_count, context);
    }

    free(before_text);
    free(after_text);
    free_owned_lines(before_lines, before_count);
    free_owned_lines(after_lines, after_count);
    free(ops);
    return NANOCVS_OK;
}

int cmd_restore(int argc, char **argv) {
    sqlite3 *db = NULL;
    int rc;
    const char *relpath;
    const char *dest_path;
    const char *revision = NULL;
    int i;

    if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        printf("Usage: %s restore PATH [REV] [--to DEST]\n", argv[0]);
        printf("Restore stored file content to disk.\n");
        return NANOCVS_OK;
    }
    if (argc < 3 || argc > 6) {
        fprintf(stderr, "Usage: %s restore PATH [REV] [--to DEST]\n", argv[0]);
        fprintf(stderr, "Try '%s help restore' for details.\n", argv[0]);
        return NANOCVS_ERR;
    }
    relpath = argv[2];
    dest_path = argv[2];

    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--to") == 0) {
            if (i + 1 >= argc || dest_path != relpath) {
                fprintf(stderr, "Usage: %s restore PATH [REV] [--to DEST]\n", argv[0]);
                fprintf(stderr, "Try '%s help restore' for details.\n", argv[0]);
                return NANOCVS_ERR;
            }
            dest_path = argv[++i];
        } else if (!revision) {
            revision = argv[i];
        } else {
            fprintf(stderr, "Usage: %s restore PATH [REV] [--to DEST]\n", argv[0]);
            fprintf(stderr, "Try '%s help restore' for details.\n", argv[0]);
            return NANOCVS_ERR;
        }
    }

    if (db_open(&db) != NANOCVS_OK) return NANOCVS_ERR;
    rc = restore_to_path(db, relpath, dest_path, revision);
    sqlite3_close(db);
    return rc;
}

int cmd_revert(int argc, char **argv) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_int64 scan_id = 0;
    int rc;
    char *endptr = NULL;

    if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        printf("Usage: %s revert SCAN_ID\n", argv[0]);
        printf("Revert all changes associated with a specific scan id.\n");
        return NANOCVS_OK;
    }
    if (argc != 3) {
        fprintf(stderr, "Usage: %s revert SCAN_ID\n", argv[0]);
        fprintf(stderr, "Try '%s help revert' for details.\n", argv[0]);
        return NANOCVS_ERR;
    }

    scan_id = strtoll(argv[2], &endptr, 10);
    if (!endptr || *endptr != '\0' || scan_id <= 0) {
        fprintf(stderr, "invalid scan id: %s\n", argv[2]);
        return NANOCVS_ERR;
    }

    if (db_open(&db) != NANOCVS_OK) return NANOCVS_ERR;

    /* Query all changes for this scan_id */
    rc = sqlite3_prepare_v2(db,
        "SELECT ce.id, p.relpath, ce.action "
        "FROM change_event ce JOIN path p ON p.id=ce.path_id "
        "WHERE ce.scan_id=? ORDER BY ce.id DESC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db error: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NANOCVS_ERR;
    }
    sqlite3_bind_int64(stmt, 1, scan_id);

    printf("reverting scan %lld...\n", (long long)scan_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 cid = sqlite3_column_int64(stmt, 0);
        const char *relpath = (const char *)sqlite3_column_text(stmt, 1);
        const char *action = (const char *)sqlite3_column_text(stmt, 2);
        char rev_buf[32];

        if (strcmp(action, "create") == 0) {
            if (unlink(relpath) == 0) {
                printf("deleted %s\n", relpath);
            } else if (errno != ENOENT) {
                fprintf(stderr, "failed to delete %s: %s\n", relpath, strerror(errno));
            }
        } else {
            /* For modify or delete, restore the before side content */
            snprintf(rev_buf, sizeof(rev_buf), "%lld^", (long long)cid);
            restore_to_path(db, relpath, relpath, rev_buf);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return NANOCVS_OK;
}

int cmd_diff(int argc, char **argv) {
    sqlite3 *db = NULL;
    int rc;
    int context = 3;
    int stat_only = 0;
    const char *relpath = NULL;
    const char *revision = NULL;
    int i;

    if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        printf("Usage: %s diff [--stat] [-U N|--unified N] PATH REV\n", argv[0]);
        printf("Show a compact unified diff for PATH at a change-event id.\n");
        return NANOCVS_OK;
    }

    for (i = 2; i < argc; ++i) {
        if ((strcmp(argv[i], "-U") == 0 || strcmp(argv[i], "--unified") == 0)) {
            char *endptr = NULL;
            long v;
            if (i + 1 >= argc) {
                fprintf(stderr, "Usage: %s diff [--stat] [-U N|--unified N] PATH REV\n", argv[0]);
                fprintf(stderr, "Try '%s help diff' for details.\n", argv[0]);
                return NANOCVS_ERR;
            }
            v = strtol(argv[++i], &endptr, 10);
            if (!endptr || *endptr != '\0' || v < 0 || v > 100000) {
                fprintf(stderr, "invalid unified context: %s\n", argv[i]);
                return NANOCVS_ERR;
            }
            context = (int)v;
        } else if (strcmp(argv[i], "--stat") == 0) {
            stat_only = 1;
        } else if (!relpath) {
            relpath = argv[i];
        } else if (!revision) {
            revision = argv[i];
        } else {
            fprintf(stderr, "Usage: %s diff [--stat] [-U N|--unified N] PATH REV\n", argv[0]);
            fprintf(stderr, "Try '%s help diff' for details.\n", argv[0]);
            return NANOCVS_ERR;
        }
    }

    if (!relpath || !revision) {
        fprintf(stderr, "Usage: %s diff [--stat] [-U N|--unified N] PATH REV\n", argv[0]);
        fprintf(stderr, "Try '%s help diff' for details.\n", argv[0]);
        return NANOCVS_ERR;
    }

    if (db_open(&db) != NANOCVS_OK) return NANOCVS_ERR;
    rc = print_simple_diff(db, relpath, revision, context, stat_only);
    sqlite3_close(db);
    return rc;
}
