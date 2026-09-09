#include "nanocvs.h"

typedef struct {
    const char *ptr;
    size_t len;
    int has_newline;
} LineView;

static int split_lines(const char *text, LineView **lines_out, int *count_out) {
    const char *p = text ? text : "";
    const char *start = p;
    LineView *lines = NULL;
    int count = 0;
    int cap = 0;

    while (*p) {
        if (*p == '\n') {
            if (count == cap) {
                int newcap = cap ? cap * 2 : 32;
                LineView *n = realloc(lines, sizeof(LineView) * (size_t)newcap);
                if (!n) {
                    free(lines);
                    return NANOCVS_ERR;
                }
                lines = n;
                cap = newcap;
            }
            lines[count].ptr = start;
            lines[count].len = (size_t)(p - start);
            lines[count].has_newline = 1;
            count++;
            start = p + 1;
        }
        p++;
    }
    if (p != start) {
        if (count == cap) {
            int newcap = cap ? cap * 2 : 32;
            LineView *n = realloc(lines, sizeof(LineView) * (size_t)newcap);
            if (!n) {
                free(lines);
                return NANOCVS_ERR;
            }
            lines = n;
            cap = newcap;
        }
        lines[count].ptr = start;
        lines[count].len = (size_t)(p - start);
        lines[count].has_newline = 0;
        count++;
    }

    *lines_out = lines;
    *count_out = count;
    return NANOCVS_OK;
}

static int lines_equal(LineView a, LineView b) {
    return a.len == b.len && a.has_newline == b.has_newline &&
        memcmp(a.ptr, b.ptr, a.len) == 0;
}

int line_diff_count(const char *old_text, const char *new_text, LineDiffCounts *counts) {
    LineView *old_lines = NULL;
    LineView *new_lines = NULL;
    int old_count = 0;
    int new_count = 0;
    int *prev = NULL;
    int *curr = NULL;
    int i;
    int j;

    if (!counts) return NANOCVS_ERR;
    counts->added = 0;
    counts->deleted = 0;

    if (split_lines(old_text, &old_lines, &old_count) != NANOCVS_OK) return NANOCVS_ERR;
    if (split_lines(new_text, &new_lines, &new_count) != NANOCVS_OK) {
        free(old_lines);
        return NANOCVS_ERR;
    }

    prev = calloc((size_t)new_count + 1, sizeof(int));
    curr = calloc((size_t)new_count + 1, sizeof(int));
    if (!prev || !curr) {
        free(old_lines);
        free(new_lines);
        free(prev);
        free(curr);
        return NANOCVS_ERR;
    }

    for (i = 1; i <= old_count; ++i) {
        for (j = 1; j <= new_count; ++j) {
            if (lines_equal(old_lines[i - 1], new_lines[j - 1])) {
                curr[j] = prev[j - 1] + 1;
            } else {
                curr[j] = prev[j] > curr[j - 1] ? prev[j] : curr[j - 1];
            }
        }
        for (j = 0; j <= new_count; ++j) {
            prev[j] = curr[j];
            curr[j] = 0;
        }
    }

    counts->deleted = old_count - prev[new_count];
    counts->added = new_count - prev[new_count];

    free(old_lines);
    free(new_lines);
    free(prev);
    free(curr);
    return NANOCVS_OK;
}
