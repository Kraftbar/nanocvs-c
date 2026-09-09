#include "nanocvs.h"
#include <fcntl.h>

char nanocvs_db_path[PATH_MAX];

void nanocvs_resolve_paths(void) {
    char exe[PATH_MAX];
    char *dir;
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) {
        if (!getcwd(exe, sizeof(exe))) exe[0] = '\0';
    } else {
        exe[len] = '\0';
    }
    dir = strrchr(exe, '/');
    if (dir) *dir = '\0';
    snprintf(nanocvs_db_path, sizeof(nanocvs_db_path), "%s/nanocvs.db", exe);
}

static int mkdir_p(const char *dir) {
    char tmp[PATH_MAX];
    size_t len;
    char *p;

    if (!dir || !*dir) return NANOCVS_OK;
    len = strlen(dir);
    if (len >= sizeof(tmp)) return NANOCVS_ERR;
    memcpy(tmp, dir, len + 1);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return NANOCVS_ERR;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return NANOCVS_ERR;
    return NANOCVS_OK;
}

int ensure_parent_dirs(const char *path) {
    char tmp[PATH_MAX];
    char *slash;
    size_t len;

    if (!path || !*path) return NANOCVS_ERR;
    len = strlen(path);
    if (len >= sizeof(tmp)) return NANOCVS_ERR;
    memcpy(tmp, path, len + 1);
    slash = strrchr(tmp, '/');
    if (!slash) return NANOCVS_OK;
    if (slash == tmp) {
        return NANOCVS_OK;
    }
    *slash = '\0';
    return mkdir_p(tmp);
}

int write_file_atomic(const char *path, const void *data, size_t size, int make_backup, char *backup_path, size_t backup_path_size) {
    char tmp_path[PATH_MAX];
    char bak_path[PATH_MAX];
    int fd = -1;
    FILE *in = NULL;
    FILE *out = NULL;
    int rc = NANOCVS_ERR;
    int had_existing = 0;
    struct stat st;

    if (!path || !data) return NANOCVS_ERR;
    if (ensure_parent_dirs(path) != NANOCVS_OK) return NANOCVS_ERR;
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.nanocvs", path) >= (int)sizeof(tmp_path)) return NANOCVS_ERR;

    if (stat(path, &st) == 0) had_existing = 1;
    if (had_existing && make_backup) {
        if (snprintf(bak_path, sizeof(bak_path), "%s.bak", path) >= (int)sizeof(bak_path)) return NANOCVS_ERR;
        in = fopen(path, "rb");
        if (!in) return NANOCVS_ERR;
        out = fopen(bak_path, "wb");
        if (!out) {
            fclose(in);
            return NANOCVS_ERR;
        }
        for (;;) {
            char buf[8192];
            size_t got = fread(buf, 1, sizeof(buf), in);
            if (got > 0 && fwrite(buf, 1, got, out) != got) {
                fclose(in);
                fclose(out);
                unlink(bak_path);
                return NANOCVS_ERR;
            }
            if (got < sizeof(buf)) {
                if (ferror(in)) {
                    fclose(in);
                    fclose(out);
                    unlink(bak_path);
                    return NANOCVS_ERR;
                }
                break;
            }
        }
        fclose(in);
        if (fflush(out) != 0) {
            fclose(out);
            unlink(bak_path);
            return NANOCVS_ERR;
        }
        fd = fileno(out);
        if (fd < 0 || fsync(fd) != 0) {
            fclose(out);
            unlink(bak_path);
            return NANOCVS_ERR;
        }
        fclose(out);
        if (backup_path && backup_path_size > 0) snprintf(backup_path, backup_path_size, "%s", bak_path);
    } else if (backup_path && backup_path_size > 0) {
        backup_path[0] = '\0';
    }

    out = fopen(tmp_path, "wb");
    if (!out) return NANOCVS_ERR;
    if (size > 0 && fwrite(data, 1, size, out) != size) goto done;
    if (fflush(out) != 0) goto done;
    fd = fileno(out);
    if (fd < 0 || fsync(fd) != 0) goto done;
    if (fclose(out) != 0) {
        out = NULL;
        goto done;
    }
    out = NULL;
    if (rename(tmp_path, path) != 0) goto done;
    rc = NANOCVS_OK;

done:
    if (out) fclose(out);
    if (rc != NANOCVS_OK) unlink(tmp_path);
    return rc;
}

char *now_utc_iso8601(void) {
    time_t t = time(NULL);
    struct tm tmv;
    char *buf = malloc(21);
    if (!buf) return NULL;
    gmtime_r(&t, &tmv);
    strftime(buf, 21, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

int count_lines_text(const char *text) {
    int n = 0;
    const char *p;
    if (!text || !*text) return 0;
    for (p = text; *p; ++p) {
        if (*p == '\n') n++;
    }
    if (p != text && p[-1] != '\n') n++;
    return n;
}

char *read_file_text(const char *path, size_t *size_out, int *line_count_out) {
    FILE *f;
    long sz;
    size_t got;
    char *buf;

    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (memchr(buf, '\0', got) != NULL) {
        free(buf);
        return NULL;
    }
    if (size_out) *size_out = got;
    if (line_count_out) *line_count_out = count_lines_text(buf);
    return buf;
}

void sha256_text_hex(const char *text, size_t len, char out_hex[65]) {
    sha256_hex((const uint8_t *)text, len, out_hex);
}

/* allext patch: accept any file; read_file_text rejects binaries (null bytes) */
int path_is_allowed_text(const char *path) {
    (void)path;
    return 1;
}

int path_should_ignore(const char *relpath, const char *name) {
    static const char *dirs[] = {
        ".git", "node_modules", "__pycache__", "sessions",
        "nano-jobs", "nano-worker", "jobs", "reference",
        "ocr-tests", "playwright-venv", "venvs"
    };
    static const char *globs[] = {
        "*.pyc", "*.o", "*.so", "*.zip", "*.tar", "*.gz",
        "*.png", "*.jpg", "*.jpeg", "*.webp", "nanobot-web-sidecar",
        "nanocvs.db", "nanocvs.db-*", "*.tmp.nanocvs"
    };
    size_t i;
    if (strcmp(name, "nanocvs-c") == 0 && strcmp(relpath, "nanocvs-c") != 0) return 1;
    for (i = 0; i < sizeof(dirs)/sizeof(dirs[0]); ++i) {
        if (strstr(relpath, dirs[i]) != NULL) return 1;
    }
    for (i = 0; i < sizeof(globs)/sizeof(globs[0]); ++i) {
        if (fnmatch(globs[i], name, 0) == 0 || fnmatch(globs[i], relpath, 0) == 0) return 1;
    }
    return 0;
}

static char *dup_range(const char *a, const char *b) {
    size_t n = (size_t)(b - a);
    char *s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, a, n);
    s[n] = '\0';
    return s;
}

int json_extract_string_array(const char *json, const char *key, char ***items_out, int *count_out) {
    char needle[128];
    const char *p;
    const char *a;
    char **items = NULL;
    int count = 0;

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (!p) return NANOCVS_ERR;
    p = strchr(p, '[');
    if (!p) return NANOCVS_ERR;
    p++;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') {
            free_string_array(items, count);
            return NANOCVS_ERR;
        }
        a = ++p;
        while (*p && *p != '"') p++;
        if (*p != '"') {
            free_string_array(items, count);
            return NANOCVS_ERR;
        }
        items = realloc(items, sizeof(char *) * (size_t)(count + 1));
        if (!items) return NANOCVS_ERR;
        items[count] = dup_range(a, p);
        if (!items[count]) {
            free_string_array(items, count);
            return NANOCVS_ERR;
        }
        count++;
        p++;
    }
    *items_out = items;
    *count_out = count;
    return NANOCVS_OK;
}

void free_string_array(char **items, int count) {
    int i;
    if (!items) return;
    for (i = 0; i < count; ++i) free(items[i]);
    free(items);
}
