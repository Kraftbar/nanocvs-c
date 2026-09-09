#include "nanocvs.h"

int nanocvs_readonly = 0;

const char *NANOCVS_SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS artifact ("
    " hash TEXT PRIMARY KEY,"
    " encoding TEXT NOT NULL,"
    " size_bytes INTEGER NOT NULL,"
    " line_count INTEGER,"
    " content BLOB NOT NULL,"
    " created_at TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS path ("
    " id INTEGER PRIMARY KEY,"
    " root TEXT NOT NULL,"
    " relpath TEXT NOT NULL,"
    " abspath TEXT NOT NULL UNIQUE,"
    " first_seen_at TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS scan_run ("
    " id INTEGER PRIMARY KEY,"
    " started_at TEXT NOT NULL,"
    " finished_at TEXT,"
    " roots_json TEXT NOT NULL,"
    " files_seen INTEGER NOT NULL DEFAULT 0,"
    " changes_found INTEGER NOT NULL DEFAULT 0,"
    " tag TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS change_event ("
    " id INTEGER PRIMARY KEY,"
    " ts TEXT NOT NULL,"
    " path_id INTEGER NOT NULL,"
    " action TEXT NOT NULL,"
    " before_hash TEXT,"
    " after_hash TEXT,"
    " bytes_before INTEGER,"
    " bytes_after INTEGER,"
    " lines_before INTEGER,"
    " lines_after INTEGER,"
    " lines_added INTEGER NOT NULL DEFAULT 0,"
    " lines_deleted INTEGER NOT NULL DEFAULT 0,"
    " scan_id INTEGER NOT NULL,"
    " FOREIGN KEY(path_id) REFERENCES path(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS file_state ("
    " path_id INTEGER PRIMARY KEY,"
    " exists_now INTEGER NOT NULL,"
    " current_hash TEXT,"
    " size_bytes INTEGER,"
    " line_count INTEGER,"
    " mtime_ns INTEGER,"
    " last_change_id INTEGER,"
    " updated_at TEXT NOT NULL,"
    " FOREIGN KEY(path_id) REFERENCES path(id),"
    " FOREIGN KEY(last_change_id) REFERENCES change_event(id)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_change_event_path_ts ON change_event(path_id, ts);"
    "CREATE INDEX IF NOT EXISTS idx_change_event_scan ON change_event(scan_id);"
    "CREATE INDEX IF NOT EXISTS idx_file_state_exists ON file_state(exists_now);";

int db_open(sqlite3 **db) {
    sqlite3_stmt *stmt = NULL;
    int user_version = 0;
    if (nanocvs_readonly) {
        /* immutable=1 skips WAL/SHM entirely — safe for concurrent readers */
        char uri[PATH_MAX + 32];
        snprintf(uri, sizeof(uri), "file://%s?immutable=1", nanocvs_db_path);
        if (sqlite3_open_v2(uri, db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
            fprintf(stderr, "sqlite open failed: %s\n", sqlite3_errmsg(*db));
            return NANOCVS_ERR;
        }
        /* skip schema setup in readonly mode */
    } else {
        if (sqlite3_open(nanocvs_db_path, db) != SQLITE_OK) {
            fprintf(stderr, "sqlite open failed: %s\n", sqlite3_errmsg(*db));
            return NANOCVS_ERR;
        }
        if (db_exec(*db, NANOCVS_SCHEMA) != NANOCVS_OK) return NANOCVS_ERR;
    }
    if (sqlite3_prepare_v2(*db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        user_version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (user_version > 2) {
        fprintf(stderr, "nanocvs: database schema newer than this binary\n");
        return NANOCVS_ERR;
    }
    if (user_version == 0) {
        /* fresh DB: schema already includes all columns, jump straight to current version */
        if (db_exec(*db, "PRAGMA user_version=2;") != NANOCVS_OK) return NANOCVS_ERR;
        user_version = 2;
    }
    if (user_version < 2) {
        if (db_exec(*db,
            "ALTER TABLE scan_run ADD COLUMN tag TEXT;"
            "PRAGMA user_version=2;"
        ) != NANOCVS_OK) return NANOCVS_ERR;
    }
    if (db_exec(*db,
        "CREATE INDEX IF NOT EXISTS idx_path_relpath ON path(relpath);"
        "CREATE INDEX IF NOT EXISTS idx_change_event_ts ON change_event(ts);"
    ) != NANOCVS_OK) return NANOCVS_ERR;
    return NANOCVS_OK;
}

int db_exec(sqlite3 *db, const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite exec failed: %s\n", errmsg ? errmsg : "unknown error");
        sqlite3_free(errmsg);
        return NANOCVS_ERR;
    }
    return NANOCVS_OK;
}

int cmd_init(void) {
    sqlite3 *db = NULL;
    if (db_open(&db) != NANOCVS_OK) return NANOCVS_ERR;
    sqlite3_close(db);
    printf("initialized %s\n", nanocvs_db_path);
    return NANOCVS_OK;
}

int cmd_status(int argc, char **argv) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_stmt *stmt2 = NULL;
    int tracked = 0, existing = 0, total_changes = 0;
    int i, is_json = 0;

    if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        printf("Usage: %s status [--json]\n", argv[0]);
        printf("Show repository summary and last scan.\n");
        return NANOCVS_OK;
    }

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) {
            is_json = 1;
        } else {
            fprintf(stderr, "Usage: %s status [--json]\n", argv[0]);
            fprintf(stderr, "Try '%s help status' for details.\n", argv[0]);
            return NANOCVS_ERR;
        }
    }

    if (db_open(&db) != NANOCVS_OK) return NANOCVS_ERR;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM path", -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        tracked = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM file_state WHERE exists_now=1", -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        existing = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM change_event", -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        total_changes = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (is_json) {
        printf("{\n");
        printf("  \"db_path\": \"%s\",\n", nanocvs_db_path);
        printf("  \"tracked_paths\": %d,\n", tracked);
        printf("  \"existing_now\": %d,\n", existing);
        printf("  \"total_changes\": %d", total_changes);
        if (sqlite3_prepare_v2(db,
            "SELECT id, started_at, finished_at, files_seen, changes_found FROM scan_run ORDER BY id DESC LIMIT 1",
            -1, &stmt2, NULL) == SQLITE_OK && sqlite3_step(stmt2) == SQLITE_ROW) {
            printf(",\n  \"last_scan\": {\n");
            printf("    \"id\": " SQLITE_INT64_PRINTF ",\n", SQLITE_INT64_ARG(sqlite3_column_int64(stmt2, 0)));
            printf("    \"started_at\": \"%s\",\n", (const char *)sqlite3_column_text(stmt2, 1));
            printf("    \"finished_at\": \"%s\",\n", sqlite3_column_text(stmt2, 2) ? (const char *)sqlite3_column_text(stmt2, 2) : "null");
            printf("    \"files_seen\": %d,\n", sqlite3_column_int(stmt2, 3));
            printf("    \"changes_found\": %d\n", sqlite3_column_int(stmt2, 4));
            printf("  }");
        }
        printf("\n}\n");
    } else {
        printf("db: %s\n", nanocvs_db_path);
        printf("tracked paths: %d\n", tracked);
        printf("existing now: %d\n", existing);
        printf("total changes: %d\n", total_changes);

        if (sqlite3_prepare_v2(db,
            "SELECT id, started_at, finished_at, files_seen, changes_found FROM scan_run ORDER BY id DESC LIMIT 1",
            -1, &stmt2, NULL) == SQLITE_OK && sqlite3_step(stmt2) == SQLITE_ROW) {
            printf("last scan: id=%lld started=%s finished=%s files_seen=%d changes_found=%d\n",
                (long long)sqlite3_column_int64(stmt2, 0),
                (const char *)sqlite3_column_text(stmt2, 1),
                sqlite3_column_text(stmt2, 2) ? (const char *)sqlite3_column_text(stmt2, 2) : "(null)",
                sqlite3_column_int(stmt2, 3),
                sqlite3_column_int(stmt2, 4));
        } else {
            printf("last scan: none\n");
        }
    }
    sqlite3_finalize(stmt2);
    sqlite3_close(db);
    return NANOCVS_OK;
}
