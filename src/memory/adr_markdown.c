/*
 * adr_markdown.c - Deterministic, generated ADR Markdown mirror.
 *
 * The memory database is the sole authority. This module opens no write
 * transaction against SQLite and exposes no Markdown import path.
 */

#include "memory/adr_markdown.h"

#include "foundation/compat_fs.h"
#include "foundation/compat.h"
#include "foundation/constants.h"
#include "foundation/platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>
#include <yyjson/yyjson.h>

#ifdef _WIN32
#include "foundation/win_utf8.h"
#endif

#define ADR_EXPORT_DIR ".semantic-memory/adr"
#define ADR_MANIFEST "manifest.json"
#define ADR_SCHEMA "semantic-memory-adr/v1"
#define ADR_MANIFEST_SCHEMA "semantic-memory-adr-manifest/v1"
#define ADR_DIR_MODE 0755
#define ADR_MAX_RECURSE 32

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} adr_buf_t;

typedef struct {
    char **values;
    int count;
    int cap;
} adr_strings_t;

typedef struct {
    char *id;
    char *kind;
    char *layer;
    char *title;
    char *summary;
    char *content;
    char *scope_user;
    char *scope_project;
    char *scope_task;
    char *entity_key;
    char *predicate;
    char *status;
    int version;
    char *supersedes;
    int64_t created_at;
    adr_strings_t anchors;
    char *relative_path;
    char *markdown;
    char file_sha256[65];
} adr_export_item_t;

typedef struct {
    adr_export_item_t *items;
    int count;
    int cap;
} adr_export_items_t;

/* Minimal SHA-256 implementation used for stable paths and projection hashes. */
typedef struct {
    uint8_t block[64];
    uint32_t state[8];
    uint64_t bit_count;
    size_t block_len;
} adr_sha256_t;

static uint32_t sha_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

static void sha_transform(adr_sha256_t *ctx, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        int o = i * 4;
        w[i] = ((uint32_t)block[o] << 24) | ((uint32_t)block[o + 1] << 16) |
               ((uint32_t)block[o + 2] << 8) | (uint32_t)block[o + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha_init(adr_sha256_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
}

static void sha_update(adr_sha256_t *ctx, const void *input, size_t len) {
    const uint8_t *data = (const uint8_t *)input;
    for (size_t i = 0; i < len; i++) {
        ctx->block[ctx->block_len++] = data[i];
        if (ctx->block_len == sizeof(ctx->block)) {
            sha_transform(ctx, ctx->block);
            ctx->bit_count += 512;
            ctx->block_len = 0;
        }
    }
}

static void sha_final(adr_sha256_t *ctx, uint8_t digest[32]) {
    ctx->bit_count += (uint64_t)ctx->block_len * 8U;
    ctx->block[ctx->block_len++] = 0x80;
    if (ctx->block_len > 56) {
        while (ctx->block_len < 64)
            ctx->block[ctx->block_len++] = 0;
        sha_transform(ctx, ctx->block);
        ctx->block_len = 0;
    }
    while (ctx->block_len < 56)
        ctx->block[ctx->block_len++] = 0;
    for (int i = 7; i >= 0; i--)
        ctx->block[ctx->block_len++] = (uint8_t)(ctx->bit_count >> (i * 8));
    sha_transform(ctx, ctx->block);
    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

static void sha_hex(const void *data, size_t len, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    adr_sha256_t ctx;
    uint8_t digest[32];
    sha_init(&ctx);
    sha_update(&ctx, data, len);
    sha_final(&ctx, digest);
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

static int buf_reserve(adr_buf_t *buf, size_t extra) {
    if (extra > SIZE_MAX - buf->len - 1)
        return CBM_STORE_ERR;
    size_t need = buf->len + extra + 1;
    if (need <= buf->cap)
        return CBM_STORE_OK;
    size_t cap = buf->cap ? buf->cap : 512;
    while (cap < need) {
        if (cap > SIZE_MAX / 2)
            cap = need;
        else
            cap *= 2;
    }
    char *next = (char *)realloc(buf->data, cap);
    if (!next)
        return CBM_STORE_ERR;
    buf->data = next;
    buf->cap = cap;
    return CBM_STORE_OK;
}

static int buf_append_n(adr_buf_t *buf, const char *text, size_t len) {
    if (buf_reserve(buf, len) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    if (len)
        memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return CBM_STORE_OK;
}

static int buf_append(adr_buf_t *buf, const char *text) {
    return buf_append_n(buf, text ? text : "", text ? strlen(text) : 0);
}

static int buf_appendf(adr_buf_t *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0 || buf_reserve(buf, (size_t)n) != CBM_STORE_OK) {
        va_end(ap);
        return CBM_STORE_ERR;
    }
    vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap);
    va_end(ap);
    buf->len += (size_t)n;
    return CBM_STORE_OK;
}

static int buf_append_json_string(adr_buf_t *buf, const char *text) {
    static const char hex[] = "0123456789abcdef";
    if (buf_append(buf, "\"") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    for (; *p; p++) {
        char esc[7];
        const char *short_esc = NULL;
        if (*p == '"')
            short_esc = "\\\"";
        else if (*p == '\\')
            short_esc = "\\\\";
        else if (*p == '\n')
            short_esc = "\\n";
        else if (*p == '\r')
            short_esc = "\\r";
        else if (*p == '\t')
            short_esc = "\\t";
        if (short_esc) {
            if (buf_append(buf, short_esc) != CBM_STORE_OK)
                return CBM_STORE_ERR;
        } else if (*p < 0x20) {
            memcpy(esc, "\\u00", 4);
            esc[4] = hex[*p >> 4];
            esc[5] = hex[*p & 0x0f];
            esc[6] = '\0';
            if (buf_append(buf, esc) != CBM_STORE_OK)
                return CBM_STORE_ERR;
        } else if (buf_append_n(buf, (const char *)p, 1) != CBM_STORE_OK) {
            return CBM_STORE_ERR;
        }
    }
    return buf_append(buf, "\"");
}

static void buf_free(adr_buf_t *buf) {
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

static int strings_add(adr_strings_t *list, const char *value) {
    if (list->count == list->cap) {
        int cap = list->cap ? list->cap * 2 : 8;
        char **next = (char **)realloc(list->values, (size_t)cap * sizeof(*next));
        if (!next)
            return CBM_STORE_ERR;
        list->values = next;
        list->cap = cap;
    }
    list->values[list->count] = cbm_strdup(value ? value : "");
    if (!list->values[list->count])
        return CBM_STORE_ERR;
    list->count++;
    return CBM_STORE_OK;
}

static void strings_free(adr_strings_t *list) {
    for (int i = 0; i < list->count; i++)
        free(list->values[i]);
    free(list->values);
    memset(list, 0, sizeof(*list));
}

static int string_cmp(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static bool strings_contains(const adr_strings_t *list, const char *value) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->values[i], value) == 0)
            return true;
    }
    return false;
}

static char *dup_column(sqlite3_stmt *stmt, int col) {
    const char *value = (const char *)sqlite3_column_text(stmt, col);
    return cbm_strdup(value ? value : "");
}

static void export_item_free(adr_export_item_t *item) {
    free(item->id);
    free(item->kind);
    free(item->layer);
    free(item->title);
    free(item->summary);
    free(item->content);
    free(item->scope_user);
    free(item->scope_project);
    free(item->scope_task);
    free(item->entity_key);
    free(item->predicate);
    free(item->status);
    free(item->supersedes);
    strings_free(&item->anchors);
    free(item->relative_path);
    free(item->markdown);
    memset(item, 0, sizeof(*item));
}

static void export_items_free(adr_export_items_t *items) {
    for (int i = 0; i < items->count; i++)
        export_item_free(&items->items[i]);
    free(items->items);
    memset(items, 0, sizeof(*items));
}

static adr_export_item_t *export_items_add(adr_export_items_t *items) {
    if (items->count == items->cap) {
        int cap = items->cap ? items->cap * 2 : 16;
        adr_export_item_t *next =
            (adr_export_item_t *)realloc(items->items, (size_t)cap * sizeof(*next));
        if (!next)
            return NULL;
        items->items = next;
        items->cap = cap;
    }
    adr_export_item_t *item = &items->items[items->count++];
    memset(item, 0, sizeof(*item));
    return item;
}

static int load_anchors(sqlite3 *db, adr_export_item_t *item) {
    const char *sql = "SELECT substr(dst_id,6) FROM memory_edge "
                      "WHERE src_id=?1 AND type='about_code' AND dst_id LIKE 'code:%' "
                      "ORDER BY substr(dst_id,6) COLLATE BINARY;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, item->id, -1, SQLITE_TRANSIENT);
    int rc = CBM_STORE_OK;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *anchor = (const char *)sqlite3_column_text(stmt, 0);
        if (strings_add(&item->anchors, anchor) != CBM_STORE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
    }
    if (rc == CBM_STORE_OK && step_rc != SQLITE_DONE)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

static int load_snapshot(cbm_store_t *store, const char *project, adr_export_items_t *items) {
    sqlite3 *db = cbm_store_get_db(store);
    if (!db)
        return CBM_STORE_ERR;
    if (sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    const char *sql =
        "SELECT id,kind,layer,title,summary,content,scope_user,scope_project,scope_task,"
        "entity_key,predicate,status,version,supersedes,created_at FROM memory_item "
        "WHERE scope_project=?1 AND deleted_at IS NULL "
        "AND kind IN ('decision','constraint') "
        "ORDER BY COALESCE(NULLIF(entity_key,''),id) COLLATE BINARY, version, id COLLATE BINARY;";
    sqlite3_stmt *stmt = NULL;
    int rc = CBM_STORE_ERR;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        adr_export_item_t *item = export_items_add(items);
        if (!item)
            goto done;
        item->id = dup_column(stmt, 0);
        item->kind = dup_column(stmt, 1);
        item->layer = dup_column(stmt, 2);
        item->title = dup_column(stmt, 3);
        item->summary = dup_column(stmt, 4);
        item->content = dup_column(stmt, 5);
        item->scope_user = dup_column(stmt, 6);
        item->scope_project = dup_column(stmt, 7);
        item->scope_task = dup_column(stmt, 8);
        item->entity_key = dup_column(stmt, 9);
        item->predicate = dup_column(stmt, 10);
        item->status = dup_column(stmt, 11);
        item->version = sqlite3_column_int(stmt, 12);
        item->supersedes = dup_column(stmt, 13);
        item->created_at = sqlite3_column_int64(stmt, 14);
        if (!item->id || !item->kind || !item->layer || !item->title || !item->summary ||
            !item->content || !item->scope_user || !item->scope_project || !item->scope_task ||
            !item->entity_key || !item->predicate || !item->status || !item->supersedes ||
            load_anchors(db, item) != CBM_STORE_OK) {
            goto done;
        }
    }
    if (step_rc != SQLITE_DONE)
        goto done;
    rc = CBM_STORE_OK;
done:
    if (stmt)
        sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK)
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    else
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return rc;
}

static void ascii_slug(const char *text, char *out, size_t out_size) {
    size_t n = 0;
    bool dash = false;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (n + 1 >= out_size)
                break;
            out[n++] = (char)c;
            dash = false;
        } else if ((c == '-' || c == '_' || c == '.' || c == ' ') && n > 0 && !dash) {
            if (n + 1 >= out_size)
                break;
            out[n++] = '-';
            dash = true;
        }
    }
    while (n > 0 && out[n - 1] == '-')
        n--;
    if (n == 0) {
        const char fallback[] = "entity";
        size_t count = sizeof(fallback) < out_size ? sizeof(fallback) : out_size;
        memcpy(out, fallback, count);
        out[out_size - 1] = '\0';
        return;
    }
    out[n] = '\0';
}

static int build_relative_path(adr_export_item_t *item) {
    const char *entity = item->entity_key[0] ? item->entity_key : item->id;
    char entity_slug[49], id_slug[65], entity_hash[65], id_hash[65];
    ascii_slug(entity, entity_slug, sizeof(entity_slug));
    ascii_slug(item->id, id_slug, sizeof(id_slug));
    sha_hex(entity, strlen(entity), entity_hash);
    sha_hex(item->id, strlen(item->id), id_hash);
    char path[CBM_SZ_1K];
    int n = snprintf(path, sizeof(path), "items/%s-%.12s/v%04d-%s-%.8s.md", entity_slug,
                     entity_hash, item->version < 0 ? 0 : item->version, id_slug, id_hash);
    if (n < 0 || (size_t)n >= sizeof(path))
        return CBM_STORE_ERR;
    item->relative_path = cbm_strdup(path);
    return item->relative_path ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int append_yaml_string(adr_buf_t *buf, const char *key, const char *value, int indent) {
    if (buf_appendf(buf, "%*s%s: ", indent, "", key) != CBM_STORE_OK ||
        buf_append_json_string(buf, value) != CBM_STORE_OK || buf_append(buf, "\n") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    return CBM_STORE_OK;
}

static int append_yaml_nullable(adr_buf_t *buf, const char *key, const char *value, int indent) {
    if (value && value[0])
        return append_yaml_string(buf, key, value, indent);
    return buf_appendf(buf, "%*s%s: null\n", indent, "", key);
}

static int append_heading_text(adr_buf_t *buf, const char *text) {
    const unsigned char *p = (const unsigned char *)(text && text[0] ? text : "Untitled ADR");
    for (; *p; p++) {
        char c = (char)*p;
        if (c == '\r' || c == '\n' || (unsigned char)c < 0x20)
            c = ' ';
        if (buf_append_n(buf, &c, 1) != CBM_STORE_OK)
            return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static int build_markdown(adr_export_item_t *item) {
    if (build_relative_path(item) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    adr_buf_t buf = {0};
    char content_hash[65];
    sha_hex(item->content, strlen(item->content), content_hash);
    if (buf_append(&buf, "---\n") != CBM_STORE_OK ||
        append_yaml_string(&buf, "schema", ADR_SCHEMA, 0) != CBM_STORE_OK ||
        append_yaml_string(&buf, "authority", "sqlite", 0) != CBM_STORE_OK ||
        buf_append(&buf, "generated: true\n") != CBM_STORE_OK ||
        append_yaml_string(&buf, "id", item->id, 0) != CBM_STORE_OK ||
        append_yaml_string(&buf, "kind", item->kind, 0) != CBM_STORE_OK ||
        append_yaml_string(&buf, "layer", item->layer, 0) != CBM_STORE_OK ||
        append_yaml_string(&buf, "title", item->title, 0) != CBM_STORE_OK ||
        append_yaml_string(&buf, "summary", item->summary, 0) != CBM_STORE_OK ||
        append_yaml_nullable(&buf, "entity_key", item->entity_key, 0) != CBM_STORE_OK ||
        append_yaml_nullable(&buf, "predicate", item->predicate, 0) != CBM_STORE_OK ||
        buf_appendf(&buf, "version: %d\n", item->version) != CBM_STORE_OK ||
        append_yaml_string(&buf, "status", item->status, 0) != CBM_STORE_OK ||
        append_yaml_nullable(&buf, "supersedes", item->supersedes, 0) != CBM_STORE_OK ||
        buf_append(&buf, "scope:\n") != CBM_STORE_OK ||
        append_yaml_nullable(&buf, "user", item->scope_user, 2) != CBM_STORE_OK ||
        append_yaml_nullable(&buf, "project", item->scope_project, 2) != CBM_STORE_OK ||
        append_yaml_nullable(&buf, "task", item->scope_task, 2) != CBM_STORE_OK ||
        buf_appendf(&buf, "created_at: %lld\n", (long long)item->created_at) != CBM_STORE_OK ||
        buf_append(&buf, "about_code:") != CBM_STORE_OK) {
        buf_free(&buf);
        return CBM_STORE_ERR;
    }
    if (item->anchors.count == 0) {
        if (buf_append(&buf, " []\n") != CBM_STORE_OK) {
            buf_free(&buf);
            return CBM_STORE_ERR;
        }
    } else {
        if (buf_append(&buf, "\n") != CBM_STORE_OK) {
            buf_free(&buf);
            return CBM_STORE_ERR;
        }
        for (int i = 0; i < item->anchors.count; i++) {
            if (buf_append(&buf, "  - ") != CBM_STORE_OK ||
                buf_append_json_string(&buf, item->anchors.values[i]) != CBM_STORE_OK ||
                buf_append(&buf, "\n") != CBM_STORE_OK) {
                buf_free(&buf);
                return CBM_STORE_ERR;
            }
        }
    }
    if (append_yaml_string(&buf, "content_sha256", content_hash, 0) != CBM_STORE_OK ||
        buf_append(&buf, "---\n\n# ") != CBM_STORE_OK ||
        append_heading_text(&buf, item->title) != CBM_STORE_OK ||
        buf_append(&buf, "\n\n## Summary\n\n") != CBM_STORE_OK ||
        buf_append(&buf, item->summary) != CBM_STORE_OK ||
        buf_append(&buf, "\n\n## Content\n\n") != CBM_STORE_OK ||
        buf_append(&buf, item->content) != CBM_STORE_OK || buf_append(&buf, "\n") != CBM_STORE_OK) {
        buf_free(&buf);
        return CBM_STORE_ERR;
    }
    item->markdown = buf.data;
    sha_hex(item->markdown, strlen(item->markdown), item->file_sha256);
    return CBM_STORE_OK;
}

static int build_projection(adr_export_items_t *items, char projection_hash[65]) {
    adr_sha256_t projection;
    sha_init(&projection);
    for (int i = 0; i < items->count; i++) {
        if (build_markdown(&items->items[i]) != CBM_STORE_OK)
            return CBM_STORE_ERR;
        if (i > 0 && strcmp(items->items[i - 1].relative_path, items->items[i].relative_path) == 0)
            return CBM_STORE_ERR;
        sha_update(&projection, items->items[i].relative_path,
                   strlen(items->items[i].relative_path));
        sha_update(&projection, "\0", 1);
        sha_update(&projection, items->items[i].file_sha256, 64);
        sha_update(&projection, "\0", 1);
    }
    uint8_t digest[32];
    static const char hex[] = "0123456789abcdef";
    sha_final(&projection, digest);
    for (int i = 0; i < 32; i++) {
        projection_hash[i * 2] = hex[digest[i] >> 4];
        projection_hash[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    projection_hash[64] = '\0';
    return CBM_STORE_OK;
}

static char *build_manifest(const char *project, const adr_export_items_t *items,
                            const char *projection_hash) {
    adr_buf_t buf = {0};
    if (buf_append(&buf, "{\n  \"schema\": ") != CBM_STORE_OK ||
        buf_append_json_string(&buf, ADR_MANIFEST_SCHEMA) != CBM_STORE_OK ||
        buf_append(&buf, ",\n  \"authority\": \"sqlite\",\n  \"project\": ") != CBM_STORE_OK ||
        buf_append_json_string(&buf, project) != CBM_STORE_OK ||
        buf_appendf(&buf, ",\n  \"count\": %d,\n  \"projection_sha256\": \"%s\",\n  \"files\": [",
                    items->count, projection_hash) != CBM_STORE_OK) {
        buf_free(&buf);
        return NULL;
    }
    for (int i = 0; i < items->count; i++) {
        const adr_export_item_t *item = &items->items[i];
        if (buf_append(&buf, i == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ") != CBM_STORE_OK ||
            buf_append_json_string(&buf, item->id) != CBM_STORE_OK ||
            buf_append(&buf, ", \"path\": ") != CBM_STORE_OK ||
            buf_append_json_string(&buf, item->relative_path) != CBM_STORE_OK ||
            buf_appendf(&buf, ", \"sha256\": \"%s\"}", item->file_sha256) != CBM_STORE_OK) {
            buf_free(&buf);
            return NULL;
        }
    }
    if (buf_append(&buf, items->count ? "\n  ]\n}\n" : "]\n}\n") != CBM_STORE_OK) {
        buf_free(&buf);
        return NULL;
    }
    return buf.data;
}

static int path_join(char *out, size_t out_size, const char *base, const char *rel) {
    int n = snprintf(out, out_size, "%s/%s", base, rel);
    return n >= 0 && (size_t)n < out_size ? CBM_STORE_OK : CBM_STORE_ERR;
}

/* Returns 0 when read, 1 when missing, -1 on error. */
static int read_file(const char *path, char **out, size_t *out_len) {
    *out = NULL;
    if (out_len)
        *out_len = 0;
    FILE *fp = cbm_fopen(path, "rb");
    if (!fp)
        return errno == ENOENT ? 1 : CBM_STORE_ERR;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return CBM_STORE_ERR;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return CBM_STORE_ERR;
    }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(fp);
        return CBM_STORE_ERR;
    }
    size_t got = fread(data, 1, (size_t)size, fp);
    int close_rc = fclose(fp);
    if (got != (size_t)size || close_rc != 0) {
        free(data);
        return CBM_STORE_ERR;
    }
    data[got] = '\0';
    *out = data;
    if (out_len)
        *out_len = got;
    return 0;
}

static int replace_file(const char *tmp, const char *path) {
#ifdef _WIN32
    wchar_t *wtmp = cbm_utf8_to_wide(tmp);
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wtmp || !wpath) {
        free(wtmp);
        free(wpath);
        return CBM_STORE_ERR;
    }
    BOOL ok = MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(wtmp);
    free(wpath);
    return ok ? CBM_STORE_OK : CBM_STORE_ERR;
#else
    return rename(tmp, path) == 0 ? CBM_STORE_OK : CBM_STORE_ERR;
#endif
}

static int write_file_atomic(const char *path, const char *data, size_t len) {
    char tmp[CBM_SZ_4K];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return CBM_STORE_ERR;
    FILE *fp = cbm_fopen(tmp, "wb");
    if (!fp)
        return CBM_STORE_ERR;
    size_t written = fwrite(data, 1, len, fp);
    int close_rc = fclose(fp);
    if (written != len || close_rc != 0) {
        cbm_unlink(tmp);
        return CBM_STORE_ERR;
    }
    if (replace_file(tmp, path) != CBM_STORE_OK) {
        cbm_unlink(tmp);
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static bool file_matches(const char *path, const char *expected, size_t expected_len,
                         bool *exists) {
    char *actual = NULL;
    size_t len = 0;
    int rc = read_file(path, &actual, &len);
    *exists = rc == 0;
    bool matches = rc == 0 && len == expected_len && memcmp(actual, expected, len) == 0;
    free(actual);
    return matches;
}

static int collect_files_recursive(const char *root, const char *rel, int depth,
                                   adr_strings_t *files) {
    if (depth > ADR_MAX_RECURSE)
        return CBM_STORE_ERR;
    char dir_path[CBM_SZ_4K];
    if (rel && rel[0]) {
        if (path_join(dir_path, sizeof(dir_path), root, rel) != CBM_STORE_OK)
            return CBM_STORE_ERR;
    } else {
        snprintf(dir_path, sizeof(dir_path), "%s", root);
    }
    cbm_dir_t *dir = cbm_opendir(dir_path);
    if (!dir)
        return cbm_file_exists(root) ? CBM_STORE_ERR : CBM_STORE_OK;
    int rc = CBM_STORE_OK;
    cbm_dirent_t *entry;
    while ((entry = cbm_readdir(dir)) != NULL) {
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0)
            continue;
        char child_rel[CBM_SZ_4K];
        int n = rel && rel[0] ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, entry->name)
                              : snprintf(child_rel, sizeof(child_rel), "%s", entry->name);
        if (n < 0 || (size_t)n >= sizeof(child_rel)) {
            rc = CBM_STORE_ERR;
            break;
        }
        if (entry->is_dir) {
            if (collect_files_recursive(root, child_rel, depth + 1, files) != CBM_STORE_OK) {
                rc = CBM_STORE_ERR;
                break;
            }
        } else if (strings_add(files, child_rel) != CBM_STORE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
    }
    cbm_closedir(dir);
    if (files->count > 1)
        qsort(files->values, (size_t)files->count, sizeof(*files->values), string_cmp);
    return rc;
}

static int parse_manifest_paths(const char *manifest, adr_strings_t *paths) {
    yyjson_doc *doc = yyjson_read(manifest, strlen(manifest), 0);
    if (!doc)
        return CBM_STORE_ERR;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *schema = yyjson_obj_get(root, "schema");
    yyjson_val *files = yyjson_obj_get(root, "files");
    if (!yyjson_is_obj(root) || !yyjson_is_str(schema) ||
        strcmp(yyjson_get_str(schema), ADR_MANIFEST_SCHEMA) != 0 || !yyjson_is_arr(files)) {
        yyjson_doc_free(doc);
        return CBM_STORE_ERR;
    }
    size_t idx, max;
    yyjson_val *value;
    yyjson_arr_foreach(files, idx, max, value) {
        yyjson_val *path = yyjson_is_obj(value) ? yyjson_obj_get(value, "path") : NULL;
        const char *p = yyjson_is_str(path) ? yyjson_get_str(path) : NULL;
        if (!p || strncmp(p, "items/", 6) != 0 || strstr(p, "..") ||
            strings_add(paths, p) != CBM_STORE_OK) {
            yyjson_doc_free(doc);
            return CBM_STORE_ERR;
        }
    }
    yyjson_doc_free(doc);
    if (paths->count > 1)
        qsort(paths->values, (size_t)paths->count, sizeof(*paths->values), string_cmp);
    return CBM_STORE_OK;
}

static int ensure_parent_directory(const char *root, const char *relative_path) {
    char full[CBM_SZ_4K];
    if (path_join(full, sizeof(full), root, relative_path) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char *slash = strrchr(full, '/');
    if (!slash)
        return CBM_STORE_ERR;
    *slash = '\0';
    return cbm_mkdir_p(full, ADR_DIR_MODE) ? CBM_STORE_OK : CBM_STORE_ERR;
}

static char *make_report(const char *project, const char *mode, const char *output_dir,
                         const char *projection_hash, int count, int creates, int updates,
                         int deletes, int unchanged, int unmanaged, bool manifest_changed,
                         bool clean, const char *status, const char *error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "status", status ? status : "error");
    yyjson_mut_obj_add_strcpy(doc, root, "project", project ? project : "");
    yyjson_mut_obj_add_strcpy(doc, root, "mode", mode ? mode : "");
    yyjson_mut_obj_add_strcpy(doc, root, "output_dir", output_dir ? output_dir : "");
    yyjson_mut_obj_add_bool(doc, root, "authority_sqlite", true);
    yyjson_mut_obj_add_bool(doc, root, "clean", clean);
    yyjson_mut_obj_add_int(doc, root, "count", count);
    yyjson_mut_obj_add_int(doc, root, "create", creates);
    yyjson_mut_obj_add_int(doc, root, "update", updates);
    yyjson_mut_obj_add_int(doc, root, "delete", deletes);
    yyjson_mut_obj_add_int(doc, root, "unchanged", unchanged);
    yyjson_mut_obj_add_int(doc, root, "unmanaged", unmanaged);
    yyjson_mut_obj_add_bool(doc, root, "manifest_changed", manifest_changed);
    if (projection_hash)
        yyjson_mut_obj_add_strcpy(doc, root, "projection_sha256", projection_hash);
    if (error)
        yyjson_mut_obj_add_strcpy(doc, root, "error", error);
    size_t len = 0;
    char *json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    return json;
}

static const char *mode_name(cbm_adr_export_mode_t mode) {
    if (mode == CBM_ADR_EXPORT_WRITE)
        return "write";
    if (mode == CBM_ADR_EXPORT_CHECK)
        return "check";
    return "plan";
}

int cbm_store_memory_adr_export(cbm_store_t *store, const char *project, const char *repo_path,
                                cbm_adr_export_mode_t mode, char **out_report_json,
                                bool *out_clean) {
    if (out_report_json)
        *out_report_json = NULL;
    if (out_clean)
        *out_clean = false;
    if (!store || !project || !project[0] || !repo_path || !repo_path[0] || !out_report_json ||
        !out_clean || mode < CBM_ADR_EXPORT_PLAN || mode > CBM_ADR_EXPORT_CHECK ||
        !cbm_is_dir(repo_path)) {
        return CBM_STORE_ERR;
    }

    char output_dir[CBM_SZ_4K], manifest_path[CBM_SZ_4K];
    if (path_join(output_dir, sizeof(output_dir), repo_path, ADR_EXPORT_DIR) != CBM_STORE_OK ||
        path_join(manifest_path, sizeof(manifest_path), output_dir, ADR_MANIFEST) != CBM_STORE_OK)
        return CBM_STORE_ERR;

    adr_export_items_t items = {0};
    adr_strings_t expected = {0}, actual = {0}, old_managed = {0};
    char projection_hash[65];
    bool projection_ready = false;
    char *manifest = NULL, *old_manifest = NULL;
    size_t old_manifest_len = 0;
    int rc = CBM_STORE_ERR;
    const char *error = NULL;

    if (load_snapshot(store, project, &items) != CBM_STORE_OK ||
        build_projection(&items, projection_hash) != CBM_STORE_OK) {
        error = "failed to build a consistent ADR snapshot";
        goto done;
    }
    projection_ready = true;
    manifest = build_manifest(project, &items, projection_hash);
    if (!manifest) {
        error = "failed to serialize ADR manifest";
        goto done;
    }
    for (int i = 0; i < items.count; i++) {
        if (strings_add(&expected, items.items[i].relative_path) != CBM_STORE_OK) {
            error = "failed to allocate expected file list";
            goto done;
        }
    }

    int manifest_read = read_file(manifest_path, &old_manifest, &old_manifest_len);
    bool manifest_exists = manifest_read == 0;
    bool manifest_valid = true;
    if (manifest_read == CBM_STORE_ERR) {
        error = "failed to read existing manifest";
        goto done;
    }
    if (manifest_exists && parse_manifest_paths(old_manifest, &old_managed) != CBM_STORE_OK)
        manifest_valid = false;
    if (collect_files_recursive(output_dir, "", 0, &actual) != CBM_STORE_OK) {
        error = "failed to inspect existing ADR mirror";
        goto done;
    }

    int creates = 0, updates = 0, deletes = 0, unchanged = 0, unmanaged = 0;
    for (int i = 0; i < items.count; i++) {
        char full[CBM_SZ_4K];
        if (path_join(full, sizeof(full), output_dir, items.items[i].relative_path) !=
            CBM_STORE_OK) {
            error = "generated ADR path is too long";
            goto done;
        }
        bool exists = false;
        if (file_matches(full, items.items[i].markdown, strlen(items.items[i].markdown), &exists))
            unchanged++;
        else if (exists)
            updates++;
        else
            creates++;
    }
    for (int i = 0; i < actual.count; i++) {
        const char *path = actual.values[i];
        if (strcmp(path, ADR_MANIFEST) == 0 || strings_contains(&expected, path))
            continue;
        if (manifest_valid && strings_contains(&old_managed, path))
            deletes++;
        else
            unmanaged++;
    }
    bool manifest_changed =
        !manifest_exists || old_manifest_len != strlen(manifest) ||
        memcmp(old_manifest ? old_manifest : "", manifest, strlen(manifest)) != 0;
    bool clean_before = manifest_valid && creates == 0 && updates == 0 && deletes == 0 &&
                        unmanaged == 0 && !manifest_changed;
    *out_clean = clean_before;

    if (!manifest_valid) {
        error = "existing manifest is invalid; refusing to overwrite the mirror";
        goto report_error;
    }
    if (mode == CBM_ADR_EXPORT_WRITE && unmanaged > 0) {
        error = "unmanaged files exist under .semantic-memory/adr; refusing to overwrite them";
        goto report_error;
    }
    if (mode == CBM_ADR_EXPORT_WRITE && !clean_before) {
        if (!cbm_mkdir_p(output_dir, ADR_DIR_MODE)) {
            error = "failed to create ADR mirror directory";
            goto report_error;
        }
        for (int i = 0; i < items.count; i++) {
            char full[CBM_SZ_4K];
            bool exists = false;
            if (path_join(full, sizeof(full), output_dir, items.items[i].relative_path) !=
                    CBM_STORE_OK ||
                ensure_parent_directory(output_dir, items.items[i].relative_path) != CBM_STORE_OK) {
                error = "failed to create ADR item directory";
                goto report_error;
            }
            if (!file_matches(full, items.items[i].markdown, strlen(items.items[i].markdown),
                              &exists) &&
                write_file_atomic(full, items.items[i].markdown, strlen(items.items[i].markdown)) !=
                    CBM_STORE_OK) {
                error = "failed to write an ADR Markdown file";
                goto report_error;
            }
        }
        for (int i = 0; i < old_managed.count; i++) {
            const char *relative = old_managed.values[i];
            if (strings_contains(&expected, relative))
                continue;
            char full[CBM_SZ_4K];
            if (path_join(full, sizeof(full), output_dir, relative) != CBM_STORE_OK ||
                (cbm_file_exists(full) && cbm_unlink(full) != 0)) {
                error = "failed to delete a stale managed ADR file";
                goto report_error;
            }
            char *slash = strrchr(full, '/');
            if (slash) {
                *slash = '\0';
                (void)cbm_rmdir(full);
            }
        }
        if (write_file_atomic(manifest_path, manifest, strlen(manifest)) != CBM_STORE_OK) {
            error = "failed to write ADR manifest";
            goto report_error;
        }
    }

    {
        const char *status =
            mode == CBM_ADR_EXPORT_WRITE
                ? (clean_before ? "unchanged" : "written")
                : (mode == CBM_ADR_EXPORT_CHECK ? (clean_before ? "clean" : "drift") : "planned");
        *out_report_json = make_report(project, mode_name(mode), output_dir, projection_hash,
                                       items.count, creates, updates, deletes, unchanged, unmanaged,
                                       manifest_changed, clean_before, status, NULL);
        rc = *out_report_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    goto done;

report_error:
    *out_report_json = make_report(project, mode_name(mode), output_dir, projection_hash,
                                   items.count, creates, updates, deletes, unchanged, unmanaged,
                                   manifest_changed, false, "error", error);
    rc = CBM_STORE_ERR;

done:
    if (rc != CBM_STORE_OK && !*out_report_json) {
        *out_report_json = make_report(
            project, mode_name(mode), output_dir, projection_ready ? projection_hash : NULL,
            items.count, 0, 0, 0, 0, 0, false, false, "error", error ? error : "ADR export failed");
    }
    free(manifest);
    free(old_manifest);
    strings_free(&expected);
    strings_free(&actual);
    strings_free(&old_managed);
    export_items_free(&items);
    return rc;
}
