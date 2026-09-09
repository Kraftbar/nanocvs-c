#include "nanocvs.h"
#include "config.h"

typedef struct {
    char **items;
    int count;
    int cap;
} SeenList;

static int seen_add(SeenList *s, const char *path) {
    char *copy;
    if (s->count == s->cap) {
        int newcap = s->cap ? s->cap * 2 : 64;
        char **n = realloc(s->items, sizeof(char *) * (size_t)newcap);
        if (!n) return NANOCVS_ERR;
        s->items = n;
        s->cap = newcap;
    }
    copy = strdup(path);
    if (!copy) return NANOCVS_ERR;
    s->items[s->count++] = copy;
    return NANOCVS_OK;
}

static int seen_has(SeenList *s, const char *path) {
    int i;
    for (i = 0; i < s->count; ++i) {
        if (strcmp(s->items[i], path) == 0) return 1;
    }
    return 0;
}

static void seen_free(SeenList *s) {
    int i;
    for (i = 0; i < s->count; ++i) free(s->items[i]);
    free(s->items);
}

static int insert_scan_run(sqlite3 *db, const char *started_at, const char *roots_json, const char *tag, sqlite3_int64 *scan_id) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO scan_run(started_at, roots_json, files_seen, changes_found, tag) VALUES(?,?,0,0,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, started_at, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, roots_json, -1, SQLITE_TRANSIENT);
    if (tag && *tag) sqlite3_bind_text(stmt, 3, tag, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 3);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return NANOCVS_ERR;
    *scan_id = sqlite3_last_insert_rowid(db);
    return NANOCVS_OK;
}

static int ensure_path_row(sqlite3 *db, const char *root, const char *relpath, const char *abspath, const char *ts, sqlite3_int64 *path_id) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    rc = sqlite3_prepare_v2(db, "SELECT id FROM path WHERE abspath=?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, abspath, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *path_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        rc = sqlite3_prepare_v2(db,
            "UPDATE path SET root=?, relpath=? WHERE id=?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) return NANOCVS_ERR;
        sqlite3_bind_text(stmt, 1, root, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, relpath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, *path_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return NANOCVS_ERR;
        return NANOCVS_OK;
    }
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db,
        "INSERT INTO path(root, relpath, abspath, first_seen_at) VALUES(?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, root, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, relpath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, abspath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return NANOCVS_ERR;
    *path_id = sqlite3_last_insert_rowid(db);
    return NANOCVS_OK;
}

static int ensure_artifact(sqlite3 *db, const char *hash_hex, const char *text, size_t size_bytes, int line_count, const char *ts) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO artifact(hash, encoding, size_bytes, line_count, content, created_at) VALUES(?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, hash_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, "utf-8", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)size_bytes);
    sqlite3_bind_int(stmt, 4, line_count);
    sqlite3_bind_blob(stmt, 5, text, (int)size_bytes, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? NANOCVS_OK : NANOCVS_ERR;
}

static int fetch_file_state(sqlite3 *db, sqlite3_int64 path_id, int *exists_now, char current_hash[65], sqlite3_int64 *size_bytes, int *line_count, sqlite3_int64 *mtime_ns, sqlite3_int64 *last_change_id, int *found) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT exists_now, current_hash, size_bytes, line_count, mtime_ns, last_change_id FROM file_state WHERE path_id=?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_int64(stmt, 1, path_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *exists_now = sqlite3_column_int(stmt, 0);
        if (sqlite3_column_text(stmt, 1)) {
            snprintf(current_hash, 65, "%s", (const char *)sqlite3_column_text(stmt, 1));
        } else {
            current_hash[0] = '\0';
        }
        *size_bytes = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 2);
        *line_count = sqlite3_column_type(stmt, 3) == SQLITE_NULL ? 0 : sqlite3_column_int(stmt, 3);
        *mtime_ns = sqlite3_column_type(stmt, 4) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 4);
        *last_change_id = sqlite3_column_type(stmt, 5) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 5);
        *found = 1;
    } else {
        *found = 0;
    }
    sqlite3_finalize(stmt);
    return NANOCVS_OK;
}

static int insert_change(sqlite3 *db, const char *ts, sqlite3_int64 path_id, const char *action,
    const char *before_hash, const char *after_hash,
    sqlite3_int64 bytes_before, sqlite3_int64 bytes_after,
    int lines_before, int lines_after, int lines_added, int lines_deleted,
    sqlite3_int64 scan_id, sqlite3_int64 *change_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO change_event(ts, path_id, action, before_hash, after_hash, bytes_before, bytes_after, lines_before, lines_after, lines_added, lines_deleted, scan_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, path_id);
    sqlite3_bind_text(stmt, 3, action, -1, SQLITE_TRANSIENT);
    if (before_hash && *before_hash) sqlite3_bind_text(stmt, 4, before_hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 4);
    if (after_hash && *after_hash) sqlite3_bind_text(stmt, 5, after_hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 5);
    if (bytes_before >= 0) sqlite3_bind_int64(stmt, 6, bytes_before); else sqlite3_bind_null(stmt, 6);
    if (bytes_after >= 0) sqlite3_bind_int64(stmt, 7, bytes_after); else sqlite3_bind_null(stmt, 7);
    if (lines_before >= 0) sqlite3_bind_int(stmt, 8, lines_before); else sqlite3_bind_null(stmt, 8);
    if (lines_after >= 0) sqlite3_bind_int(stmt, 9, lines_after); else sqlite3_bind_null(stmt, 9);
    sqlite3_bind_int(stmt, 10, lines_added);
    sqlite3_bind_int(stmt, 11, lines_deleted);
    sqlite3_bind_int64(stmt, 12, scan_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return NANOCVS_ERR;
    *change_id = sqlite3_last_insert_rowid(db);
    return NANOCVS_OK;
}

static int upsert_file_state(sqlite3 *db, sqlite3_int64 path_id, int exists_now, const char *current_hash,
    sqlite3_int64 size_bytes, int line_count, sqlite3_int64 mtime_ns, sqlite3_int64 last_change_id, const char *ts) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO file_state(path_id, exists_now, current_hash, size_bytes, line_count, mtime_ns, last_change_id, updated_at) VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(path_id) DO UPDATE SET exists_now=excluded.exists_now, current_hash=excluded.current_hash, size_bytes=excluded.size_bytes, line_count=excluded.line_count, mtime_ns=excluded.mtime_ns, last_change_id=excluded.last_change_id, updated_at=excluded.updated_at",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_int64(stmt, 1, path_id);
    sqlite3_bind_int(stmt, 2, exists_now);
    if (current_hash && *current_hash) sqlite3_bind_text(stmt, 3, current_hash, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 3);
    if (size_bytes >= 0) sqlite3_bind_int64(stmt, 4, size_bytes); else sqlite3_bind_null(stmt, 4);
    if (line_count >= 0) sqlite3_bind_int(stmt, 5, line_count); else sqlite3_bind_null(stmt, 5);
    if (mtime_ns >= 0) sqlite3_bind_int64(stmt, 6, mtime_ns); else sqlite3_bind_null(stmt, 6);
    if (last_change_id >= 0) sqlite3_bind_int64(stmt, 7, last_change_id); else sqlite3_bind_null(stmt, 7);
    sqlite3_bind_text(stmt, 8, ts, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? NANOCVS_OK : NANOCVS_ERR;
}

static int load_artifact_content(sqlite3 *db, const char *hash_hex, char **content_out, size_t *size_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    const void *blob;
    int size;
    char *copy;

    if (!hash_hex || !*hash_hex) {
        *content_out = NULL;
        if (size_out) *size_out = 0;
        return NANOCVS_OK;
    }

    rc = sqlite3_prepare_v2(db, "SELECT content, size_bytes FROM artifact WHERE hash=?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    sqlite3_bind_text(stmt, 1, hash_hex, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return NANOCVS_ERR;
    }

    blob = sqlite3_column_blob(stmt, 0);
    size = sqlite3_column_bytes(stmt, 0);
    copy = malloc((size_t)size + 1);
    if (!copy) {
        sqlite3_finalize(stmt);
        return NANOCVS_ERR;
    }
    if (size > 0 && blob) memcpy(copy, blob, (size_t)size);
    copy[size] = '\0';
    if (size_out) *size_out = (size_t)size;
    *content_out = copy;
    sqlite3_finalize(stmt);
    return NANOCVS_OK;
}

static void read_config_roots(const char ***roots_out, int *count_out) {
    int n = 0;
    while (TRACKED_ROOTS[n]) n++;
    *roots_out = TRACKED_ROOTS;
    *count_out = n;
}

static char *json_quote_roots(char **roots, int count) {
    int i;
    size_t total = 3;
    char *buf;
    size_t pos = 0;
    for (i = 0; i < count; ++i) total += strlen(roots[i]) + 4;
    buf = malloc(total + 1);
    if (!buf) return NULL;
    buf[pos++] = '[';
    for (i = 0; i < count; ++i) {
        if (i) buf[pos++] = ',';
        buf[pos++] = '"';
        memcpy(buf + pos, roots[i], strlen(roots[i]));
        pos += strlen(roots[i]);
        buf[pos++] = '"';
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    return buf;
}

static int process_file(sqlite3 *db, const char *root, const char *abspath, const char *relpath,
    sqlite3_int64 scan_id, int *changes_found, int *processed, SeenList *seen) {
    size_t size = 0;
    int lines = 0;
    char *text = NULL;
    char hash_hex[65];
    sqlite3_int64 path_id = 0;
    int exists_now = 0, found = 0, old_lines = 0;
    sqlite3_int64 old_size = 0, old_mtime = 0, last_change_id = 0, change_id = 0;
    char old_hash[65] = {0};
    struct stat st;
    char *ts = now_utc_iso8601();
    sqlite3_int64 mtime_ns = 0;
    int added = 0, deleted = 0;
    char *old_text = NULL;

    if (processed) *processed = 0;
    if (!ts) return NANOCVS_ERR;
    text = read_file_text(abspath, &size, &lines);
    if (!text) {
        free(ts);
        return NANOCVS_OK;
    }
    if (stat(abspath, &st) != 0) {
        free(text);
        free(ts);
        return NANOCVS_ERR;
    }
    mtime_ns = (sqlite3_int64)st.st_mtim.tv_sec * 1000000000LL + (sqlite3_int64)st.st_mtim.tv_nsec;
    sha256_text_hex(text, size, hash_hex);
    if (ensure_path_row(db, root, relpath, abspath, ts, &path_id) != NANOCVS_OK) goto fail;
    if (ensure_artifact(db, hash_hex, text, size, lines, ts) != NANOCVS_OK) goto fail;
    if (fetch_file_state(db, path_id, &exists_now, old_hash, &old_size, &old_lines, &old_mtime, &last_change_id, &found) != NANOCVS_OK) goto fail;

    if (!found) {
        if (insert_change(db, ts, path_id, "create", NULL, hash_hex, -1, (sqlite3_int64)size, -1, lines, lines, 0, scan_id, &change_id) != NANOCVS_OK) goto fail;
        if (upsert_file_state(db, path_id, 1, hash_hex, (sqlite3_int64)size, lines, mtime_ns, change_id, ts) != NANOCVS_OK) goto fail;
        (*changes_found)++;
    } else if (!exists_now || strcmp(old_hash, hash_hex) != 0) {
        if (exists_now) {
            LineDiffCounts counts = {0};
            if (load_artifact_content(db, old_hash, &old_text, NULL) != NANOCVS_OK) goto fail;
            if (line_diff_count(old_text ? old_text : "", text, &counts) != NANOCVS_OK) goto fail;
            added = counts.added;
            deleted = counts.deleted;
        } else {
            added = lines;
            deleted = 0;
        }
        if (insert_change(db, ts, path_id, exists_now ? "modify" : "create", exists_now ? old_hash : NULL, hash_hex,
                exists_now ? old_size : -1, (sqlite3_int64)size,
                exists_now ? old_lines : -1, lines, added, deleted, scan_id, &change_id) != NANOCVS_OK) goto fail;
        if (upsert_file_state(db, path_id, 1, hash_hex, (sqlite3_int64)size, lines, mtime_ns, change_id, ts) != NANOCVS_OK) goto fail;
        (*changes_found)++;
    } else {
        if (upsert_file_state(db, path_id, 1, hash_hex, (sqlite3_int64)size, lines, mtime_ns, last_change_id, ts) != NANOCVS_OK) goto fail;
    }

    if (seen_add(seen, abspath) != NANOCVS_OK) {
        fprintf(stderr, "out of memory recording seen path: %s\n", abspath);
        goto fail;
    }
    if (processed) *processed = 1;
    free(text);
    free(old_text);
    free(ts);
    return NANOCVS_OK;
 
 fail:
    free(text);
    free(old_text);
    free(ts);
    return NANOCVS_ERR;

}

static int walk_root(sqlite3 *db, const char *root, const char *dirpath, sqlite3_int64 scan_id, int *files_seen, int *changes_found, SeenList *seen) {
    DIR *dir = opendir(dirpath);
    struct dirent *ent;
    if (!dir) return NANOCVS_ERR;
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        char rel[PATH_MAX];
        struct stat st;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(full, sizeof(full), "%s/%s", dirpath, ent->d_name);
        if (stat(full, &st) != 0) continue;
        if (strncmp(full, root, strlen(root)) == 0) {
            const char *r = full + strlen(root);
            if (*r == '/') r++;
            snprintf(rel, sizeof(rel), "%s", *r ? r : ".");
        } else {
            snprintf(rel, sizeof(rel), "%s", ent->d_name);
        }
        if (path_should_ignore(rel, ent->d_name)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (walk_root(db, root, full, scan_id, files_seen, changes_found, seen) != NANOCVS_OK) {
                closedir(dir);
                return NANOCVS_ERR;
            }
        } else if (S_ISREG(st.st_mode)) {
            int processed = 0;
            if (!path_is_allowed_text(full)) continue;
            if (process_file(db, root, full, rel, scan_id, changes_found, &processed, seen) != NANOCVS_OK) {
                closedir(dir);
                return NANOCVS_ERR;
            }
            if (processed) (*files_seen)++;
        }
    }
    closedir(dir);
    return NANOCVS_OK;
}

static int mark_deletes(sqlite3 *db, char **roots, int root_count, sqlite3_int64 scan_id, int *changes_found, SeenList *seen) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT p.id, p.abspath, fs.current_hash, fs.size_bytes, fs.line_count FROM path p JOIN file_state fs ON fs.path_id=p.id WHERE fs.exists_now=1",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NANOCVS_ERR;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 path_id = sqlite3_column_int64(stmt, 0);
        const char *abspath = (const char *)sqlite3_column_text(stmt, 1);
        const char *current_hash = (const char *)sqlite3_column_text(stmt, 2);
        sqlite3_int64 size_bytes = sqlite3_column_int64(stmt, 3);
        int line_count = sqlite3_column_int(stmt, 4);
        int under_root = 0;
        int i;
        sqlite3_int64 change_id = 0;
        char *ts;
        for (i = 0; i < root_count; ++i) {
            size_t n = strlen(roots[i]);
            if (strncmp(abspath, roots[i], n) == 0 && (abspath[n] == '/' || abspath[n] == '\0')) {
                under_root = 1;
                break;
            }
        }
        if (!under_root || seen_has(seen, abspath)) continue;
        ts = now_utc_iso8601();
        if (!ts) {
            sqlite3_finalize(stmt);
            return NANOCVS_ERR;
        }
        if (insert_change(db, ts, path_id, "delete", current_hash, NULL, size_bytes, -1, line_count, -1, 0, line_count, scan_id, &change_id) != NANOCVS_OK) {
            free(ts);
            sqlite3_finalize(stmt);
            return NANOCVS_ERR;
        }
        if (upsert_file_state(db, path_id, 0, NULL, -1, -1, -1, change_id, ts) != NANOCVS_OK) {
            free(ts);
            sqlite3_finalize(stmt);
            return NANOCVS_ERR;
        }
        (*changes_found)++;
        free(ts);
    }
    sqlite3_finalize(stmt);
    return NANOCVS_OK;
}

int cmd_scan(int argc, char **argv) {
    sqlite3 *db = NULL;
    const char **roots = NULL;
    int root_count = 0;
    int i;
    char *started_at = NULL;
    char *finished_at = NULL;
    char *roots_json = NULL;
    sqlite3_int64 scan_id = 0;
    int files_seen = 0;
    int changes_found = 0;
    SeenList seen = {0};
    sqlite3_stmt *stmt = NULL;
    const char *tag = NULL;

    if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        printf("Usage: %s scan [--tag LABEL]\n", argv[0]);
        printf("Scan tracked roots and record changes.\n");
        return NANOCVS_OK;
    }

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--tag") == 0 && i + 1 < argc) {
            tag = argv[++i];
        } else {
            fprintf(stderr, "Usage: %s scan [--tag LABEL]\n", argv[0]);
            fprintf(stderr, "Try '%s help scan' for details.\n", argv[0]);
            return NANOCVS_ERR;
        }
    }

    read_config_roots(&roots, &root_count);
    roots_json = json_quote_roots((char **)roots, root_count);
    started_at = now_utc_iso8601();
    if (!roots_json || !started_at) goto fail;
    if (db_open(&db) != NANOCVS_OK) goto fail;
    if (db_exec(db, "BEGIN IMMEDIATE") != NANOCVS_OK) goto fail;
    if (insert_scan_run(db, started_at, roots_json, tag, &scan_id) != NANOCVS_OK) goto fail;

    for (i = 0; i < root_count; ++i) {
        struct stat st;
        if (stat(roots[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (walk_root(db, roots[i], roots[i], scan_id, &files_seen, &changes_found, &seen) != NANOCVS_OK) goto fail;
    }
    if (mark_deletes(db, (char **)roots, root_count, scan_id, &changes_found, &seen) != NANOCVS_OK) goto fail;

    finished_at = now_utc_iso8601();
    if (!finished_at) goto fail;
    if (sqlite3_prepare_v2(db,
        "UPDATE scan_run SET finished_at=?, files_seen=?, changes_found=? WHERE id=?",
        -1, &stmt, NULL) != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, finished_at, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, files_seen);
    sqlite3_bind_int(stmt, 3, changes_found);
    sqlite3_bind_int64(stmt, 4, scan_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (db_exec(db, "COMMIT") != NANOCVS_OK) goto fail;

    if (tag && *tag)
        printf("scan %lld: tag=%s files_seen=%d changes_found=%d\n", (long long)scan_id, tag, files_seen, changes_found);
    else
        printf("scan %lld: files_seen=%d changes_found=%d\n", (long long)scan_id, files_seen, changes_found);
    sqlite3_close(db);
    free(roots_json);
    free(started_at);
    free(finished_at);
    seen_free(&seen);
    return NANOCVS_OK;


fail:
    if (stmt) sqlite3_finalize(stmt);
    if (db) {
        db_exec(db, "ROLLBACK");
        sqlite3_close(db);
    }
    free(roots_json);
    free(started_at);
    free(finished_at);
    seen_free(&seen);
    fprintf(stderr, "scan failed\n");
    return NANOCVS_ERR;
}
