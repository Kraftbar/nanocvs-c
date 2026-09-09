#include "nanocvs.h"

static int parse_positive_int(const char *s, int *out) {
    char *end = NULL;
    long v;
    if (!s || !*s) return NANOCVS_ERR;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v <= 0 || v > INT_MAX) return NANOCVS_ERR;
    *out = (int)v;
    return NANOCVS_OK;
}

static int parse_log_args(int argc, char **argv, int *limit_out, const char **path_out, int *json_out) {
    int i;
    int limit = 20;
    const char *path = NULL;
    int is_json = 0;
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--limit") == 0) {
            if (i + 1 >= argc || parse_positive_int(argv[i + 1], &limit) != NANOCVS_OK) return NANOCVS_ERR;
            i++;
        } else if (strcmp(argv[i], "--path") == 0) {
            if (i + 1 >= argc || path) return NANOCVS_ERR;
            path = argv[++i];
        } else if (strcmp(argv[i], "--json") == 0) {
            is_json = 1;
        } else if (argv[i][0] == '-') {
            return NANOCVS_ERR;
        } else {
            if (path) return NANOCVS_ERR;
            path = argv[i];
        }
    }
    *limit_out = limit;
    *path_out = path;
    *json_out = is_json;
    return NANOCVS_OK;
}

static int parse_week_args(int argc, char **argv, int *days_out, const char **path_out) {
    int i;
    int days = 7;
    const char *path = NULL;
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--days") == 0) {
            if (i + 1 >= argc || parse_positive_int(argv[i + 1], &days) != NANOCVS_OK) return NANOCVS_ERR;
            i++;
        } else if (strcmp(argv[i], "--path") == 0) {
            if (i + 1 >= argc || path) return NANOCVS_ERR;
            path = argv[++i];
        } else if (argv[i][0] == '-') {
            return NANOCVS_ERR;
        } else {
            if (path) return NANOCVS_ERR;
            path = argv[i];
        }
    }
    *days_out = days;
    *path_out = path;
    return NANOCVS_OK;
}

static void print_int_or_dash(sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) printf("-");
    else printf("%d", sqlite3_column_int(stmt, col));
}

static void print_i64_or_dash(sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) printf("-");
    else printf(SQLITE_INT64_PRINTF, SQLITE_INT64_ARG(sqlite3_column_int64(stmt, col)));
}

static void print_json_int_or_null(sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) printf("null");
    else printf("%d", sqlite3_column_int(stmt, col));
}

static void print_json_i64_or_null(sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) printf("null");
    else printf(SQLITE_INT64_PRINTF, SQLITE_INT64_ARG(sqlite3_column_int64(stmt, col)));
}

int cmd_log(int argc, char **argv) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int rows = 0;
    int limit = 20;
    const char *path = NULL;
    int is_json = 0;
    if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        printf("Usage: %s log [-n N|--limit N] [--path RELPATH|RELPATH] [--json]\n", argv[0]);
        printf("Show recent change events.\n");
        return NANOCVS_OK;
    }
    const char *sql_all =
        "SELECT ce.id, ce.ts, ce.action, p.relpath, ce.lines_before, ce.lines_after, "
        "ce.lines_added, ce.lines_deleted, ce.bytes_before, ce.bytes_after, ce.scan_id "
        "FROM change_event AS ce "
        "JOIN path AS p ON p.id = ce.path_id "
        "ORDER BY ce.ts DESC, ce.id DESC "
        "LIMIT ?1";
    const char *sql_path =
        "SELECT ce.id, ce.ts, ce.action, p.relpath, ce.lines_before, ce.lines_after, "
        "ce.lines_added, ce.lines_deleted, ce.bytes_before, ce.bytes_after, ce.scan_id "
        "FROM change_event AS ce "
        "JOIN path AS p ON p.id = ce.path_id "
        "WHERE p.relpath = ?1 "
        "ORDER BY ce.ts DESC, ce.id DESC "
        "LIMIT ?2";

    if (parse_log_args(argc, argv, &limit, &path, &is_json) != NANOCVS_OK) {
        fprintf(stderr, "Usage: %s log [-n N|--limit N] [--path RELPATH|RELPATH] [--json]\n", argv[0]);
        fprintf(stderr, "Try '%s help log' for details.\n", argv[0]);
        return NANOCVS_ERR;
    }
    if (db_open(&db) != NANOCVS_OK) return NANOCVS_ERR;

    if (path) {
        rc = sqlite3_prepare_v2(db, sql_path, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, limit);
        }
    } else {
        rc = sqlite3_prepare_v2(db, sql_all, -1, &stmt, NULL);
        if (rc == SQLITE_OK) sqlite3_bind_int(stmt, 1, limit);
    }
    if (rc != SQLITE_OK) {
        fprintf(stderr, "log query failed\n");
        sqlite3_close(db);
        return NANOCVS_ERR;
    }

    if (is_json) printf("[\n");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (is_json) {
            if (rows > 0) printf(",\n");
            printf("  {\n");
            printf("    \"id\": " SQLITE_INT64_PRINTF ",\n", SQLITE_INT64_ARG(sqlite3_column_int64(stmt, 0)));
            printf("    \"ts\": \"%s\",\n", (const char *)sqlite3_column_text(stmt, 1));
            printf("    \"action\": \"%s\",\n", (const char *)sqlite3_column_text(stmt, 2));
            printf("    \"relpath\": \"%s\",\n", (const char *)sqlite3_column_text(stmt, 3));
            printf("    \"lines_before\": "); print_json_int_or_null(stmt, 4); printf(",\n");
            printf("    \"lines_after\": "); print_json_int_or_null(stmt, 5); printf(",\n");
            printf("    \"lines_added\": %d,\n", sqlite3_column_int(stmt, 6));
            printf("    \"lines_deleted\": %d,\n", sqlite3_column_int(stmt, 7));
            printf("    \"bytes_before\": "); print_json_i64_or_null(stmt, 8); printf(",\n");
            printf("    \"bytes_after\": "); print_json_i64_or_null(stmt, 9); printf(",\n");
            printf("    \"scan_id\": " SQLITE_INT64_PRINTF "\n", SQLITE_INT64_ARG(sqlite3_column_int64(stmt, 10)));
            printf("  }");
        } else {
            printf("[" SQLITE_INT64_PRINTF "] %s %s %s lines ",
                SQLITE_INT64_ARG(sqlite3_column_int64(stmt, 0)),
                sqlite3_column_text(stmt, 1) ? (const char *)sqlite3_column_text(stmt, 1) : "?",
                sqlite3_column_text(stmt, 2) ? (const char *)sqlite3_column_text(stmt, 2) : "?",
                sqlite3_column_text(stmt, 3) ? (const char *)sqlite3_column_text(stmt, 3) : "?");
            print_int_or_dash(stmt, 4);
            printf("->");
            print_int_or_dash(stmt, 5);
            printf(" (+%d -%d) bytes ", sqlite3_column_int(stmt, 6), sqlite3_column_int(stmt, 7));
            print_i64_or_dash(stmt, 8);
            printf("->");
            print_i64_or_dash(stmt, 9);
            printf(" scan=" SQLITE_INT64_PRINTF "\n", SQLITE_INT64_ARG(sqlite3_column_int64(stmt, 10)));
        }
        rows++;
    }

    if (is_json) printf("\n]\n");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (rows == 0 && !is_json) {
        if (path) printf("no history for %s\n", path);
        else printf("no history\n");
    }
    return NANOCVS_OK;
}

int cmd_week(int argc, char **argv) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_stmt *total_stmt = NULL;
    int rc;
    int rows = 0;
    int days = 7;
    const char *path = NULL;
    if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)) {
        printf("Usage: %s week [--days N] [--path RELPATH|RELPATH]\n", argv[0]);
        printf("Summarize recent activity by day.\n");
        return NANOCVS_OK;
    }
    const char *sql_all =
        "SELECT substr(ce.ts, 1, 10) AS day, "
        "COUNT(*) AS changes, "
        "COUNT(DISTINCT ce.path_id) AS files, "
        "SUM(CASE WHEN ce.action='create' THEN 1 ELSE 0 END) AS creates, "
        "SUM(CASE WHEN ce.action='modify' THEN 1 ELSE 0 END) AS modifies, "
        "SUM(CASE WHEN ce.action='delete' THEN 1 ELSE 0 END) AS deletes, "
        "COALESCE(SUM(ce.lines_added), 0) AS lines_added, "
        "COALESCE(SUM(ce.lines_deleted), 0) AS lines_deleted "
        "FROM change_event AS ce "
        "WHERE ce.ts >= strftime('%Y-%m-%dT00:00:00Z', 'now', printf('-%d days', ?1 - 1)) "
        "  AND ce.ts <  strftime('%Y-%m-%dT00:00:00Z', 'now', '+1 day') "
        "GROUP BY substr(ce.ts, 1, 10) "
        "ORDER BY day DESC";
    const char *sql_path =
        "SELECT substr(ce.ts, 1, 10) AS day, "
        "COUNT(*) AS changes, "
        "COUNT(DISTINCT ce.path_id) AS files, "
        "SUM(CASE WHEN ce.action='create' THEN 1 ELSE 0 END) AS creates, "
        "SUM(CASE WHEN ce.action='modify' THEN 1 ELSE 0 END) AS modifies, "
        "SUM(CASE WHEN ce.action='delete' THEN 1 ELSE 0 END) AS deletes, "
        "COALESCE(SUM(ce.lines_added), 0) AS lines_added, "
        "COALESCE(SUM(ce.lines_deleted), 0) AS lines_deleted "
        "FROM change_event AS ce "
        "JOIN path AS p ON p.id = ce.path_id "
        "WHERE p.relpath = ?1 "
        "  AND ce.ts >= strftime('%Y-%m-%dT00:00:00Z', 'now', printf('-%d days', ?2 - 1)) "
        "  AND ce.ts <  strftime('%Y-%m-%dT00:00:00Z', 'now', '+1 day') "
        "GROUP BY substr(ce.ts, 1, 10) "
        "ORDER BY day DESC";
    const char *total_all =
        "SELECT COUNT(*), COUNT(DISTINCT ce.path_id), "
        "SUM(CASE WHEN ce.action='create' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN ce.action='modify' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN ce.action='delete' THEN 1 ELSE 0 END), "
        "COALESCE(SUM(ce.lines_added), 0), COALESCE(SUM(ce.lines_deleted), 0) "
        "FROM change_event AS ce "
        "WHERE ce.ts >= strftime('%Y-%m-%dT00:00:00Z', 'now', printf('-%d days', ?1 - 1)) "
        "  AND ce.ts <  strftime('%Y-%m-%dT00:00:00Z', 'now', '+1 day')";
    const char *total_path =
        "SELECT COUNT(*), COUNT(DISTINCT ce.path_id), "
        "SUM(CASE WHEN ce.action='create' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN ce.action='modify' THEN 1 ELSE 0 END), "
        "SUM(CASE WHEN ce.action='delete' THEN 1 ELSE 0 END), "
        "COALESCE(SUM(ce.lines_added), 0), COALESCE(SUM(ce.lines_deleted), 0) "
        "FROM change_event AS ce JOIN path AS p ON p.id = ce.path_id "
        "WHERE p.relpath = ?1 "
        "  AND ce.ts >= strftime('%Y-%m-%dT00:00:00Z', 'now', printf('-%d days', ?2 - 1)) "
        "  AND ce.ts <  strftime('%Y-%m-%dT00:00:00Z', 'now', '+1 day')";

    if (parse_week_args(argc, argv, &days, &path) != NANOCVS_OK) {
        fprintf(stderr, "Usage: %s week [--days N] [--path RELPATH|RELPATH]\n", argv[0]);
        fprintf(stderr, "Try '%s help week' for details.\n", argv[0]);
        return NANOCVS_ERR;
    }
    if (db_open(&db) != NANOCVS_OK) return NANOCVS_ERR;

    if (path) {
        rc = sqlite3_prepare_v2(db, sql_path, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, days);
        }
    } else {
        rc = sqlite3_prepare_v2(db, sql_all, -1, &stmt, NULL);
        if (rc == SQLITE_OK) sqlite3_bind_int(stmt, 1, days);
    }
    if (rc != SQLITE_OK) {
        fprintf(stderr, "week query failed\n");
        sqlite3_close(db);
        return NANOCVS_ERR;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%s changes=%d files=%d creates=%d modifies=%d deletes=%d +%d -%d\n",
            sqlite3_column_text(stmt, 0) ? (const char *)sqlite3_column_text(stmt, 0) : "??????????",
            sqlite3_column_int(stmt, 1), sqlite3_column_int(stmt, 2), sqlite3_column_int(stmt, 3),
            sqlite3_column_int(stmt, 4), sqlite3_column_int(stmt, 5), sqlite3_column_int(stmt, 6), sqlite3_column_int(stmt, 7));
        rows++;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (rows == 0) {
        sqlite3_close(db);
        if (path) printf("no changes in last %d days for %s\n", days, path);
        else printf("no changes in last %d days\n", days);
        return NANOCVS_OK;
    }

    if (path) {
        rc = sqlite3_prepare_v2(db, total_path, -1, &total_stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(total_stmt, 1, path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(total_stmt, 2, days);
        }
    } else {
        rc = sqlite3_prepare_v2(db, total_all, -1, &total_stmt, NULL);
        if (rc == SQLITE_OK) sqlite3_bind_int(total_stmt, 1, days);
    }
    if (rc != SQLITE_OK) {
        fprintf(stderr, "week total query failed\n");
        sqlite3_close(db);
        return NANOCVS_ERR;
    }

    if (sqlite3_step(total_stmt) == SQLITE_ROW) {
        printf("total      changes=%d files=%d creates=%d modifies=%d deletes=%d +%d -%d\n",
            sqlite3_column_int(total_stmt, 0), sqlite3_column_int(total_stmt, 1), sqlite3_column_int(total_stmt, 2),
            sqlite3_column_int(total_stmt, 3), sqlite3_column_int(total_stmt, 4), sqlite3_column_int(total_stmt, 5), sqlite3_column_int(total_stmt, 6));
    }

    sqlite3_finalize(total_stmt);
    sqlite3_close(db);
    return NANOCVS_OK;
}
