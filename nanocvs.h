#ifndef NANOCVS_H
#define NANOCVS_H

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"

#define NANOCVS_OK 0
#define NANOCVS_ERR 1

extern char nanocvs_db_path[PATH_MAX];

void nanocvs_resolve_paths(void);

#ifndef SQLITE_INT64_PRINTF
#define SQLITE_INT64_PRINTF "%lld"
#define SQLITE_INT64_ARG(v) ((long long)(v))
#endif

extern const char *NANOCVS_SCHEMA;

int cmd_init(void);
int cmd_status(int argc, char **argv);
int cmd_scan(int argc, char **argv);
int cmd_log(int argc, char **argv);
int cmd_week(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_restore(int argc, char **argv);
int cmd_revert(int argc, char **argv);
int cmd_diff(int argc, char **argv);

extern int nanocvs_readonly;
int db_open(sqlite3 **db);
int db_exec(sqlite3 *db, const char *sql);
char *now_utc_iso8601(void);
char *read_file_text(const char *path, size_t *size_out, int *line_count_out);
int write_file_atomic(const char *path, const void *data, size_t size, int make_backup, char *backup_path, size_t backup_path_size);
int ensure_parent_dirs(const char *path);
int count_lines_text(const char *text);
void sha256_text_hex(const char *text, size_t len, char out_hex[65]);
typedef struct {
    int added;
    int deleted;
} LineDiffCounts;

int path_is_allowed_text(const char *path);
int path_should_ignore(const char *relpath, const char *name);
int json_extract_string_array(const char *json, const char *key, char ***items_out, int *count_out);
void free_string_array(char **items, int count);
int line_diff_count(const char *old_text, const char *new_text, LineDiffCounts *counts);

#endif
