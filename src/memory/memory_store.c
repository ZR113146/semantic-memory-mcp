/*
 * memory_store.c — Local-fork memory/ADR extensions for the semantic-memory-mcp
 * project. Extracted from store/store.c so that the memory lifecycle, CJK FTS
 * segmentation, vector embedding, conflict resolution, decay model, and ADR
 * queries can evolve independently while the upstream store layer stays aligned
 * with the code-graph-only reference implementation.
 *
 * All memory data lives in a separate SQLite file (<cache>/<project>-memory.db)
 * so rebuilding the code graph never destroys long-term memory.
 */

#include "memory/memory_store.h"
#include "foundation/constants.h"
#include "store/store_internal.h"
#include "store/embed.h"
#include "foundation/platform.h"
#include "foundation/compat.h"
#include "foundation/log.h"
#include "foundation/compat_regex.h"
#include "foundation/str_util.h"
#include "semantic/semantic.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "yyjson/yyjson.h"

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#define CBM_STORE_DENOM_EPS_D 1e-10

/* ══ Extracted from store.c lines 191-320 ══ */

static uint32_t mem_utf8_decode(const unsigned char *p, int *adv) {
    unsigned char c = p[0];
    if (c < 0x80) {
        *adv = 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *adv = 2;
        return ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *adv = 3;
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *adv = 4;
        return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
               ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *adv = 1;
    return c;
}

/* True for CJK codepoints we want to segment into character bigrams:
 * CJK Unified (incl. Ext-A), compatibility ideographs, and Hiragana/Katakana.
 * These scripts have no word spaces, so unicode61 indexes a whole run as one
 * token and substring MATCH fails — bigram splitting restores recall. */
static int mem_is_cjk(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x9FFF)     /* CJK Unified + Ext-A */
           || (cp >= 0xF900 && cp <= 0xFAFF)  /* CJK compatibility ideographs */
           || (cp >= 0x3040 && cp <= 0x30FF); /* Hiragana + Katakana */
}

/* Segment UTF-8 text for FTS indexing/query (zhiwei-kb lattice approach, lite):
 *   - ASCII alphanumeric runs are kept whole and lowercased (one token).
 *   - CJK runs are split into overlapping character bigrams; a lone CJK char
 *     becomes a unigram. Tokens are space-joined.
 * Returns a heap string the caller must free, or NULL on OOM/empty.
 * Worst case each 3-byte CJK char yields a ~7-byte "xy " bigram token, so a
 * 4x size bound is safe. */
static char *memory_segment_cjk(const char *text) {
    if (!text || !text[0]) {
        return NULL;
    }
    size_t in_len = strlen(text);
    /* Generous bound: bigrams duplicate each CJK char (max 4 bytes) plus a space. */
    size_t cap = in_len * 4 + 16;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t w = 0;
    int need_space = 0; /* a separator is needed before the next token */

#define MEM_SEG_PUT(src, n)                         \
    do {                                            \
        if (need_space && w < cap)                  \
            out[w++] = ' ';                         \
        for (int _i = 0; _i < (n) && w < cap; _i++) \
            out[w++] = (src)[_i];                   \
        need_space = 1;                             \
    } while (0)

    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *end = p + in_len;
    while (p < end) {
        int adv = 1;
        uint32_t cp = mem_utf8_decode(p, &adv);

        if (mem_is_cjk(cp)) {
            /* Consume the whole CJK run, recording byte offsets of each char. */
            const unsigned char *run_start = p;
            const unsigned char *q = p;
            size_t offs[256];
            int nchar = 0;
            while (q < end && nchar < (int)(sizeof(offs) / sizeof(offs[0]))) {
                int a = 1;
                uint32_t c = mem_utf8_decode(q, &a);
                if (!mem_is_cjk(c))
                    break;
                offs[nchar++] = (size_t)(q - run_start);
                q += a;
            }
            size_t run_bytes = (size_t)(q - run_start);
            if (nchar == 1) {
                MEM_SEG_PUT((const char *)run_start, (int)run_bytes);
            } else {
                for (int i = 0; i + 1 < nchar; i++) {
                    size_t bstart = offs[i];
                    size_t bend = (i + 2 < nchar) ? offs[i + 2] : run_bytes;
                    MEM_SEG_PUT((const char *)(run_start + bstart), (int)(bend - bstart));
                }
            }
            p = q;
            continue;
        }

        int is_ascii_word = (cp < 0x80) && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                                            (cp >= '0' && cp <= '9') || cp == '_');
        if (is_ascii_word) {
            /* Consume the whole ASCII word run, lowercasing. */
            char word[256];
            int wn = 0;
            const unsigned char *q = p;
            while (q < end && wn < (int)sizeof(word)) {
                unsigned char c = *q;
                int aw = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_';
                if (!aw)
                    break;
                word[wn++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                q++;
            }
            MEM_SEG_PUT(word, wn);
            p = q;
            continue;
        }

        /* Whitespace / punctuation / other: token separator. */
        if (w > 0)
            need_space = 1;
        p += adv;
    }
    if (w >= cap)
        w = cap - 1;
    out[w] = '\0';
#undef MEM_SEG_PUT
    return out;
}

/* ══ Extracted from store.c lines 527-537 ══ */

static int memory_vec_upsert(cbm_store_t *s, const char *item_id, const char *content);
/* memory_meta key/value helpers — defined below (schema v2); forward-declared
 * here so the merged-DB migration (cbm_store_migrate_memory_from_graph, defined
 * above their definition) can read/stamp the migration marker. */
static int64_t memory_meta_get_i64(cbm_store_t *s, const char *key, int64_t fallback);
static void memory_meta_set_i64(cbm_store_t *s, const char *key, int64_t value);
/* Audit-event helper (P0-2/P0-4): writes a memory_event row inside the caller's
 * open transaction. Defined below near the delete path; forward-declared here so
 * the lifecycle mutators (update_status, consolidate, decay) can record events. */
static int memory_delete_audit(cbm_store_t *s, const char *type, const char *mode, const char *id,
                               const char *project, const char *user);

/* ── Memory schema + versioned migrations ─────────────────────────
 *
 * Owns the memory_* DDL and its migration chain so the upstream store layer
 * (store.c init_schema) stays graph-only / zero-diff. Called from the store
 * open path right after the graph schema init; versioning rides on
 * PRAGMA user_version, which upstream does not use.
 *
 * Each step runs inside a transaction that also bumps user_version, so a
 * step's DDL and its version stamp commit (or roll back) atomically — a crash
 * can never leave "DDL applied but version not advanced" or vice versa.
 *
 * Step 0→1 is the baseline: all CREATE ... IF NOT EXISTS, so it is safe both
 * for a truly fresh DB and for a legacy DB that already has the tables but
 * predates versioning (user_version stays 0 until stamped here). Later steps
 * use ALTER TABLE ADD COLUMN (instant in SQLite, no table rewrite).
 *
 * NOTE: PRAGMA user_version cannot be parameterized; the version number is a
 * compile-time constant formatted into the statement, never user input. */

#define CBM_MEMORY_SCHEMA_VERSION 5

static int mem_exec(cbm_store_t *s, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(s->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        if (err) {
            snprintf(s->errbuf, sizeof(s->errbuf), "%s", err);
            sqlite3_free(err);
        }
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* Baseline memory schema (v1). Tables + FTS + indexes, all IF NOT EXISTS. */
static int memory_init_schema(cbm_store_t *s) {
    const char *ddl =
        "CREATE TABLE IF NOT EXISTS memory_event ("
        "  id TEXT PRIMARY KEY,"
        "  type TEXT NOT NULL,"
        "  source TEXT NOT NULL,"
        "  timestamp INTEGER NOT NULL,"
        "  project TEXT,"
        "  user TEXT,"
        "  payload TEXT NOT NULL,"
        "  confidence REAL DEFAULT 0.5,"
        "  context TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_item ("
        "  id TEXT PRIMARY KEY,"
        "  kind TEXT NOT NULL DEFAULT 'event',"
        "  layer TEXT NOT NULL DEFAULT 'episodic',"
        "  title TEXT,"
        "  summary TEXT,"
        "  content TEXT NOT NULL,"
        "  scope_user TEXT,"
        "  scope_project TEXT,"
        "  scope_task TEXT,"
        "  entity_key TEXT,"
        "  predicate TEXT,"
        "  importance REAL DEFAULT 0.5,"
        "  confidence REAL DEFAULT 0.5,"
        "  reusability REAL DEFAULT 0.5,"
        "  specificity REAL DEFAULT 0.5,"
        "  hit_count INTEGER DEFAULT 0,"
        "  last_hit_at INTEGER,"
        "  decay REAL DEFAULT 0.0,"
        "  status TEXT DEFAULT 'candidate',"
        "  version INTEGER DEFAULT 1,"
        "  supersedes TEXT,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  source_event_ids TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_edge ("
        "  id TEXT PRIMARY KEY,"
        "  src_id TEXT NOT NULL,"
        "  dst_id TEXT NOT NULL,"
        "  type TEXT NOT NULL,"
        "  weight REAL DEFAULT 1.0,"
        "  origin TEXT NOT NULL,"
        "  confidence REAL DEFAULT 0.5,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_vec ("
        "  item_id TEXT PRIMARY KEY,"
        "  dim INTEGER DEFAULT 256,"
        "  embedding BLOB,"
        "  embedding_json TEXT,"
        "  updated_at INTEGER"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_memory_item_scope ON memory_item(scope_user, "
        "scope_project, scope_task);"
        "CREATE INDEX IF NOT EXISTS idx_memory_item_dedup ON memory_item(entity_key, predicate, "
        "scope_project);"
        "CREATE INDEX IF NOT EXISTS idx_memory_item_status ON memory_item(status);"
        "CREATE INDEX IF NOT EXISTS idx_memory_edge_src ON memory_edge(src_id, type);"
        "CREATE INDEX IF NOT EXISTS idx_memory_edge_dst ON memory_edge(dst_id, type);";

    int rc = mem_exec(s, ddl);
    if (rc != CBM_STORE_OK) {
        return rc;
    }

    /* FTS5 contentless-style virtual table for memory BM25 search (item_id is
     * UNINDEXED — only title/summary/content feed ranking). Tolerates a build
     * without FTS5 compiled in: retrieval degrades to structured queries. */
    {
        char *fts_err = NULL;
        int fts_rc = sqlite3_exec(s->db,
                                  "CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5("
                                  "  item_id UNINDEXED, title, summary, content,"
                                  "  tokenize='unicode61 remove_diacritics 2'"
                                  ");",
                                  NULL, NULL, &fts_err);
        if (fts_rc != SQLITE_OK && fts_err) {
            sqlite3_free(fts_err);
        }
    }
    return CBM_STORE_OK;
}

static int memory_read_user_version(cbm_store_t *s) {
    sqlite3_stmt *st = NULL;
    int ver = 0;
    if (sqlite3_prepare_v2(s->db, "PRAGMA user_version;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            ver = sqlite3_column_int(st, 0);
        }
    }
    sqlite3_finalize(st);
    return ver;
}

/* Drop all stored memory vectors and re-embed every item from its content via
 * the current embedding dispatcher. Shared by the v2→3 (256→768) and v4→5
 * (768→1024) migrations: stored widths are incomparable across dims
 * (cbm_cosine_i8 scores mismatched widths as 0), so a full rebuild is the only
 * correct move. Single-user DBs hold few items; this is cheap. Collects
 * (id, content) first — finalizing the read cursor before writing memory_vec —
 * to avoid cursor-vs-write races. Runs inside the caller's open transaction. */
static int memory_vec_rebuild_all(cbm_store_t *s) {
    if (mem_exec(s, "DELETE FROM memory_vec;") != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    char **ids = NULL;
    char **contents = NULL;
    int count = 0;
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT id, content FROM memory_item;", -1, &sel, NULL) ==
        SQLITE_OK) {
        int cap = 0;
        while (sqlite3_step(sel) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(sel, 0);
            const char *content = (const char *)sqlite3_column_text(sel, 1);
            if (!id)
                continue;
            if (count == cap) {
                int ncap = cap ? cap * 2 : 16;
                char **ni = realloc(ids, (size_t)ncap * sizeof(*ni));
                char **nc = realloc(contents, (size_t)ncap * sizeof(*nc));
                if (!ni || !nc) {
                    free(ni ? ni : ids);
                    free(nc ? nc : contents);
                    ids = NULL;
                    contents = NULL;
                    break;
                }
                ids = ni;
                contents = nc;
                cap = ncap;
            }
            ids[count] = heap_strdup(id);
            contents[count] = heap_strdup(content ? content : "");
            count++;
        }
    }
    sqlite3_finalize(sel);
    int rebuild_ok = (ids != NULL || count == 0);
    for (int i = 0; i < count && rebuild_ok; i++) {
        if (memory_vec_upsert(s, ids[i], contents[i]) != CBM_STORE_OK) {
            rebuild_ok = 0;
        }
    }
    for (int i = 0; i < count; i++) {
        free(ids[i]);
        free(contents[i]);
    }
    free(ids);
    free(contents);
    return rebuild_ok ? CBM_STORE_OK : CBM_STORE_ERR;
}

/* Run one migration step: BEGIN → step SQL/callback → stamp version → COMMIT. */
static int memory_migration_step(cbm_store_t *s, int target_ver, int (*step)(cbm_store_t *)) {
    int rc = cbm_store_begin(s);
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    rc = step(s);
    if (rc != CBM_STORE_OK) {
        cbm_store_rollback(s);
        return rc;
    }
    char stamp[48];
    snprintf(stamp, sizeof(stamp), "PRAGMA user_version = %d;", target_ver);
    rc = mem_exec(s, stamp);
    if (rc != CBM_STORE_OK) {
        cbm_store_rollback(s);
        return rc;
    }
    rc = cbm_store_commit(s);
    if (rc != CBM_STORE_OK) {
        cbm_store_rollback(s);
        return rc;
    }
    return CBM_STORE_OK;
}

/* ver 1→2: memory_meta — a small key/value table for memory-subsystem
 * bookkeeping (e.g. last auto-maintenance timestamps). The DB is one file per
 * project, so meta is naturally project-scoped; no scope column needed. */
static int memory_migrate_v2(cbm_store_t *s) {
    return mem_exec(s, "CREATE TABLE IF NOT EXISTS memory_meta ("
                       "  key TEXT PRIMARY KEY,"
                       "  value TEXT"
                       ");");
}

/* ver 3→4: soft-delete support. A nullable deleted_at timestamp marks an item
 * as awaiting physical removal: hidden from all retrieval paths immediately,
 * kept on disk through a grace window so a soft delete can be undone
 * (restore). A retention sweep physically purges items past the window.
 * NULL means "live". The index keeps the sweep's deleted_at scan cheap. */
static int memory_migrate_v4(cbm_store_t *s) {
    int rc = mem_exec(s, "ALTER TABLE memory_item ADD COLUMN deleted_at INTEGER;");
    if (rc != CBM_STORE_OK) {
        return rc;
    }
    return mem_exec(
        s, "CREATE INDEX IF NOT EXISTS idx_memory_item_deleted ON memory_item(deleted_at);");
}

int cbm_memory_run_migrations(cbm_store_t *s) {
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    int ver = memory_read_user_version(s);
    if (ver >= CBM_MEMORY_SCHEMA_VERSION) {
        return CBM_STORE_OK;
    }
    if (ver < 1 && memory_migration_step(s, 1, memory_init_schema) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    if (ver < 2 && memory_migration_step(s, 2, memory_migrate_v2) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    /* ver 2→3: memory vectors moved from 256-d signed feature-hashing to the
     * shared 768-d nomic space. Old BLOBs are a different dimension and
     * geometry — incomparable — so drop and re-embed everything. */
    if (ver < 3 && memory_migration_step(s, 3, memory_vec_rebuild_all) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    if (ver < 4 && memory_migration_step(s, 4, memory_migrate_v4) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    /* ver 4→5: memory vectors widened from CBM_SEM_DIM (768) to MEMORY_VEC_DIM
     * (1024) so the optional bge-m3 sidecar can store its native sentence
     * vectors. Same width-mismatch problem as v2→3: drop and re-embed via the
     * current dispatcher (sidecar if enabled, else static zero-padded). */
    if (ver < 5 && memory_migration_step(s, 5, memory_vec_rebuild_all) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* ══ Extracted from store.c lines 1238-1256 ══ */

/* Derive the per-project memory DB path: <cache>/<project>-memory.db.
 * Memory lives in its own file so that rebuilding the code graph (which deletes
 * and recreates <project>.db wholesale) never destroys long-term memory. The
 * graph DB keeps the legacy <project>.db name for backward compatibility.
 * Returns CBM_STORE_OK and fills buf, or CBM_STORE_ERR on bad input/overflow. */
int cbm_memory_db_path(const char *project, char *buf, size_t bufsz) {
    if (!project || !buf || bufsz == 0 || !cbm_validate_project_name(project)) {
        return CBM_STORE_ERR;
    }
    const char *cdir = cbm_resolve_cache_dir();
    if (!cdir) {
        cdir = cbm_tmpdir();
    }
    int n = snprintf(buf, bufsz, "%s/%s-memory.db", cdir, project);
    if (n < 0 || (size_t)n >= bufsz) {
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

/* ══ Extracted from store.c lines 1322-1432 ══ */

/* One-time migration: pull memory_* tables out of a legacy merged graph DB into
 * a freshly created standalone memory DB. Historically memory and code-graph
 * tables shared one <project>.db file, so rebuilding the graph (which unlinks
 * that file) destroyed memory. New installs keep memory in <project>-memory.db;
 * this lifts pre-existing memory rows across on first open of the new file.
 *
 * Idempotent and best-effort: guarded by a memory_meta flag, and any failure
 * leaves the (empty) memory DB usable rather than aborting open. Uses a raw
 * sqlite3 handle with NO authorizer so the trusted internal ATTACH is allowed
 * (the store_authorizer hard-denies ATTACH on normal handles). Source memory
 * rows are left in place in the graph DB for now — harmless, and a safety net
 * until the split is proven; they simply stop being read. */
int cbm_store_migrate_memory_from_graph(cbm_store_t *mem, const char *graph_db_path) {
    if (!mem || !mem->db || !mem->db_path || !graph_db_path) {
        return CBM_STORE_ERR;
    }
    /* Already migrated? memory_meta marker makes this a no-op on every later open. */
    if (memory_meta_get_i64(mem, "migrated_from_merged", 0) != 0) {
        return CBM_STORE_OK;
    }
    /* Nothing to lift if there is no legacy graph file. Still stamp the marker so
     * we don't re-probe forever on fresh installs. */
    if (!cbm_file_exists(graph_db_path)) {
        memory_meta_set_i64(mem, "migrated_from_merged", 1);
        return CBM_STORE_OK;
    }

    /* Open a dedicated, authorizer-free handle on the memory DB for the copy.
     * mem->db carries store_authorizer (denies ATTACH); this trusted path must
     * attach, so it uses its own connection to the same file. */
    sqlite3 *cx = NULL;
    if (sqlite3_open_v2(mem->db_path, &cx, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        sqlite3_close(cx);
        return CBM_STORE_ERR;
    }

    int result = CBM_STORE_ERR;
    char *err = NULL;
    sqlite3_stmt *st = NULL;

    /* ATTACH the legacy graph DB. Bind the path as a parameter rather than
     * formatting a file: URI — the cache path is a native Windows path (drive
     * letter + backslashes) that does not survive URI construction, and binding
     * also sidesteps SQL quoting. We only SELECT from it, so RW attach is fine. */
    {
        sqlite3_stmt *att = NULL;
        if (sqlite3_prepare_v2(cx, "ATTACH DATABASE ?1 AS legacy;", CBM_NOT_FOUND, &att, NULL) !=
            SQLITE_OK) {
            goto done;
        }
        sqlite3_bind_text(att, 1, graph_db_path, -1, SQLITE_TRANSIENT);
        int arc = sqlite3_step(att);
        sqlite3_finalize(att);
        if (arc != SQLITE_DONE) {
            goto done; /* graph DB unreadable — leave memory DB empty, do not stamp */
        }
    }

    /* Does the legacy DB actually carry memory rows? If memory_item is absent or
     * empty there is nothing to lift; stamp the marker and finish clean. */
    bool has_rows = false;
    if (sqlite3_prepare_v2(cx,
                           "SELECT count(*) FROM legacy.sqlite_master "
                           "WHERE type='table' AND name='memory_item';",
                           CBM_NOT_FOUND, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0) {
        sqlite3_finalize(st);
        st = NULL;
        if (sqlite3_prepare_v2(cx, "SELECT count(*) FROM legacy.memory_item;", CBM_NOT_FOUND, &st,
                               NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0) {
            has_rows = true;
        }
    }
    sqlite3_finalize(st);
    st = NULL;

    if (has_rows) {
        /* Copy each memory table. INSERT OR IGNORE keeps the copy idempotent and
         * tolerant of the destination tables already existing (created by the
         * memory DB's own schema init). Wrapped in one transaction so a partial
         * failure rolls back and the marker is not stamped. */
        static const char *const copy_sql =
            "BEGIN;"
            "INSERT OR IGNORE INTO memory_event  SELECT * FROM legacy.memory_event;"
            "INSERT OR IGNORE INTO memory_item   SELECT * FROM legacy.memory_item;"
            "INSERT OR IGNORE INTO memory_edge   SELECT * FROM legacy.memory_edge;"
            "INSERT OR IGNORE INTO memory_vec    SELECT * FROM legacy.memory_vec;"
            "INSERT OR IGNORE INTO memory_fts (item_id,title,summary,content) "
            "  SELECT item_id,title,summary,content FROM legacy.memory_fts;"
            "INSERT OR IGNORE INTO memory_meta   SELECT * FROM legacy.memory_meta;"
            "COMMIT;";
        if (sqlite3_exec(cx, copy_sql, NULL, NULL, &err) != SQLITE_OK) {
            (void)sqlite3_exec(cx, "ROLLBACK;", NULL, NULL, NULL);
            goto done;
        }
    }

    (void)sqlite3_exec(cx, "DETACH DATABASE legacy;", NULL, NULL, NULL);
    /* Stamp via the store handle so it shares mem->db's view immediately. */
    memory_meta_set_i64(mem, "migrated_from_merged", 1);
    result = CBM_STORE_OK;

done:
    if (err) {
        snprintf(mem->errbuf, sizeof(mem->errbuf), "memory migrate: %s", err);
        sqlite3_free(err);
    }
    sqlite3_close_v2(cx);
    return result;
}

/* ══ Extracted from store.c lines 2317-5472 ══ */

/* -- Long-term memory MVP --------------------------------------- */

static int64_t memory_now_ms(void) {
    return (int64_t)time(NULL) * 1000LL;
}

/* memory_meta key/value helpers (schema v2). Single-statement reads/writes;
 * callers that need atomicity with other writes wrap them in their own txn. */
static int64_t memory_meta_get_i64(cbm_store_t *s, const char *key, int64_t fallback) {
    if (!s || !s->db || !key) {
        return fallback;
    }
    sqlite3_stmt *stmt = NULL;
    int64_t out = fallback;
    if (sqlite3_prepare_v2(s->db, "SELECT value FROM memory_meta WHERE key=?1;", CBM_NOT_FOUND,
                           &stmt, NULL) == SQLITE_OK) {
        bind_text(stmt, 1, key);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (v && v[0]) {
                out = (int64_t)strtoll(v, NULL, 10);
            }
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

static void memory_meta_set_i64(cbm_store_t *s, const char *key, int64_t value) {
    if (!s || !s->db || !key) {
        return;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "INSERT OR REPLACE INTO memory_meta (key,value) VALUES (?1,?2);",
                           CBM_NOT_FOUND, &stmt, NULL) == SQLITE_OK) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)value);
        bind_text(stmt, 1, key);
        bind_text(stmt, 2, buf);
        (void)sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

static void memory_make_id(char *buf, size_t sz, const char *prefix) {
    static unsigned long seq = 0;
    /* ID uniqueness must NOT depend on wall-clock precision: memory_now_ms is
     * second-granular (time(NULL)*1000), and `seq` resets to 0 in every CLI
     * process, so two writes in the same wall-clock second across back-to-back
     * CLI invocations produced IDENTICAL ids → PRIMARY KEY collision →
     * "failed to append memory event" (intermittent, depending on whether the
     * calls straddled a second boundary). Use the high-resolution monotonic
     * clock (cbm_now_ns, shared QPC on Windows) for the time component so even
     * same-second / cross-process writes get distinct ids; ++seq stays as an
     * in-process tiebreak for two ids minted within the same ns. */
    snprintf(buf, sz, "%s-%llu-%lu", prefix ? prefix : "mem", (unsigned long long)cbm_now_ns(),
             ++seq);
}

static const char *memory_nonempty(const char *v, const char *fallback) {
    return (v && v[0]) ? v : fallback;
}

static void memory_bind_nullable(sqlite3_stmt *stmt, int col, const char *v) {
    if (v) {
        bind_text(stmt, col, v);
    } else {
        sqlite3_bind_null(stmt, col);
    }
}

static char *memory_dup_col(sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        return NULL;
    }
    return heap_strdup((const char *)sqlite3_column_text(stmt, col));
}

static void memory_scan_item(sqlite3_stmt *stmt, cbm_memory_item_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = memory_dup_col(stmt, 0);
    out->kind = memory_dup_col(stmt, 1);
    out->layer = memory_dup_col(stmt, 2);
    out->title = memory_dup_col(stmt, 3);
    out->summary = memory_dup_col(stmt, 4);
    out->content = memory_dup_col(stmt, 5);
    out->scope_user = memory_dup_col(stmt, 6);
    out->scope_project = memory_dup_col(stmt, 7);
    out->scope_task = memory_dup_col(stmt, 8);
    out->entity_key = memory_dup_col(stmt, 9);
    out->predicate = memory_dup_col(stmt, 10);
    out->importance = sqlite3_column_double(stmt, 11);
    out->confidence = sqlite3_column_double(stmt, 12);
    out->reusability = sqlite3_column_double(stmt, 13);
    out->specificity = sqlite3_column_double(stmt, 14);
    out->hit_count = sqlite3_column_int(stmt, 15);
    out->last_hit_at = sqlite3_column_int64(stmt, 16);
    out->decay = sqlite3_column_double(stmt, 17);
    out->status = memory_dup_col(stmt, 18);
    out->version = sqlite3_column_int(stmt, 19);
    out->supersedes = memory_dup_col(stmt, 20);
    out->created_at = sqlite3_column_int64(stmt, 21);
    out->updated_at = sqlite3_column_int64(stmt, 22);
    out->source_event_ids = memory_dup_col(stmt, 23);
}

void cbm_store_memory_item_free(cbm_memory_item_t *item) {
    if (!item) {
        return;
    }
    safe_str_free(&item->id);
    safe_str_free(&item->kind);
    safe_str_free(&item->layer);
    safe_str_free(&item->title);
    safe_str_free(&item->summary);
    safe_str_free(&item->content);
    safe_str_free(&item->scope_user);
    safe_str_free(&item->scope_project);
    safe_str_free(&item->scope_task);
    safe_str_free(&item->entity_key);
    safe_str_free(&item->predicate);
    safe_str_free(&item->status);
    safe_str_free(&item->supersedes);
    safe_str_free(&item->source_event_ids);
    safe_str_free(&item->conflict_ids);
    safe_str_free(&item->conflict_resolution);
    safe_str_free(&item->evidence_json);
    safe_str_free(&item->retrieval_source);
    memset(item, 0, sizeof(*item));
}

void cbm_store_memory_result_free(cbm_memory_result_t *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->count; i++) {
        cbm_store_memory_item_free(&out->items[i]);
    }
    free(out->items);
    memset(out, 0, sizeof(*out));
}

int cbm_store_memory_append_event(cbm_store_t *s, const cbm_memory_event_t *event,
                                  char **out_event_id) {
    if (out_event_id) {
        *out_event_id = NULL;
    }
    if (!s || !s->db || !event || !event->payload) {
        return CBM_STORE_ERR;
    }
    char idbuf[CBM_SZ_128];
    const char *id = event->id;
    if (!id || !id[0]) {
        memory_make_id(idbuf, sizeof(idbuf), "evt");
        id = idbuf;
    }
    int64_t ts = event->timestamp_ms > 0 ? event->timestamp_ms : memory_now_ms();
    const char *sql = "INSERT INTO memory_event "
                      "(id,type,source,timestamp,project,user,payload,confidence,context) "
                      "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_event_prepare");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, memory_nonempty(event->type, "conversation"));
    bind_text(stmt, 3, memory_nonempty(event->source, "user"));
    sqlite3_bind_int64(stmt, 4, ts);
    memory_bind_nullable(stmt, 5, event->project);
    memory_bind_nullable(stmt, 6, event->user);
    bind_text(stmt, 7, event->payload);
    sqlite3_bind_double(stmt, 8, event->confidence > 0.0 ? event->confidence : 0.5);
    memory_bind_nullable(stmt, 9, event->context_json);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        store_set_error_sqlite(s, "memory_event_insert");
        return CBM_STORE_ERR;
    }
    if (out_event_id) {
        *out_event_id = heap_strdup(id);
    }
    return CBM_STORE_OK;
}

static int memory_fts_upsert(cbm_store_t *s, const cbm_memory_item_t *item, const char *id) {
    sqlite3_stmt *stmt = NULL;
    const char *del_sql = "DELETE FROM memory_fts WHERE item_id=?1;";
    if (sqlite3_prepare_v2(s->db, del_sql, CBM_NOT_FOUND, &stmt, NULL) == SQLITE_OK) {
        bind_text(stmt, 1, id);
        (void)sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    const char *ins_sql =
        "INSERT INTO memory_fts (item_id,title,summary,content) VALUES (?1,?2,?3,?4);";
    if (sqlite3_prepare_v2(s->db, ins_sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_OK;
    }
    /* Index CJK-segmented text so unicode61 sees space-separated bigram tokens
     * (Chinese has no word spaces; without this a whole run is one token and
     * substring MATCH never hits). Display still uses the original content. */
    char *seg_title = memory_segment_cjk(item->title);
    char *seg_summary = memory_segment_cjk(item->summary);
    char *seg_content = memory_segment_cjk(memory_nonempty(item->content, ""));
    bind_text(stmt, 1, id);
    memory_bind_nullable(stmt, 2, seg_title);
    memory_bind_nullable(stmt, 3, seg_summary);
    bind_text(stmt, 4, seg_content ? seg_content : "");
    (void)sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(seg_title);
    free(seg_summary);
    free(seg_content);
    return CBM_STORE_OK;
}

int cbm_store_memory_append_candidate(cbm_store_t *s, const cbm_memory_item_t *item,
                                      char **out_item_id) {
    if (out_item_id) {
        *out_item_id = NULL;
    }
    if (!s || !s->db || !item || !item->content) {
        return CBM_STORE_ERR;
    }
    char idbuf[CBM_SZ_128];
    const char *id = item->id;
    if (!id || !id[0]) {
        memory_make_id(idbuf, sizeof(idbuf), "itm");
        id = idbuf;
    }
    int64_t now = memory_now_ms();
    int64_t created = item->created_at > 0 ? item->created_at : now;
    int64_t updated = item->updated_at > 0 ? item->updated_at : now;
    const char *sql =
        "INSERT INTO memory_item "
        "(id,kind,layer,title,summary,content,scope_user,scope_project,scope_task,"
        "entity_key,predicate,importance,confidence,reusability,specificity,hit_count,"
        "last_hit_at,decay,status,version,supersedes,created_at,updated_at,source_event_ids) "
        "VALUES "
        "(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_item_prepare");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, memory_nonempty(item->kind, "event"));
    bind_text(stmt, 3, memory_nonempty(item->layer, "episodic"));
    memory_bind_nullable(stmt, 4, item->title);
    memory_bind_nullable(stmt, 5, item->summary);
    bind_text(stmt, 6, item->content);
    memory_bind_nullable(stmt, 7, item->scope_user);
    memory_bind_nullable(stmt, 8, item->scope_project);
    memory_bind_nullable(stmt, 9, item->scope_task);
    memory_bind_nullable(stmt, 10, item->entity_key);
    memory_bind_nullable(stmt, 11, item->predicate);
    sqlite3_bind_double(stmt, 12, item->importance > 0.0 ? item->importance : 0.5);
    sqlite3_bind_double(stmt, 13, item->confidence > 0.0 ? item->confidence : 0.5);
    sqlite3_bind_double(stmt, 14, item->reusability > 0.0 ? item->reusability : 0.5);
    sqlite3_bind_double(stmt, 15, item->specificity > 0.0 ? item->specificity : 0.5);
    sqlite3_bind_int(stmt, 16, item->hit_count);
    if (item->last_hit_at > 0) {
        sqlite3_bind_int64(stmt, 17, item->last_hit_at);
    } else {
        sqlite3_bind_null(stmt, 17);
    }
    sqlite3_bind_double(stmt, 18, item->decay);
    bind_text(stmt, 19, memory_nonempty(item->status, "candidate"));
    sqlite3_bind_int(stmt, 20, item->version > 0 ? item->version : 1);
    memory_bind_nullable(stmt, 21, item->supersedes);
    sqlite3_bind_int64(stmt, 22, created);
    sqlite3_bind_int64(stmt, 23, updated);
    memory_bind_nullable(stmt, 24, item->source_event_ids);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        store_set_error_sqlite(s, "memory_item_insert");
        return CBM_STORE_ERR;
    }
    /* FTS upsert is intentionally deferred to the consolidate pass (P1).
     * The hot path only appends the raw event + candidate item; all indexing
     * and enrichment runs asynchronously so writes stay cheap. */
    if (out_item_id) {
        *out_item_id = heap_strdup(id);
    }
    return CBM_STORE_OK;
}

/* MEMORY_SELECT_RAW: same columns prefixed with "m." for JOIN-qualified queries */
#define MEMORY_SELECT_RAW                                                                          \
    "m.id,m.kind,m.layer,m.title,m.summary,m.content,m.scope_user,m.scope_project,m.scope_task,m." \
    "entity_key,m.predicate,"                                                                      \
    "m.importance,m.confidence,m.reusability,m.specificity,m.hit_count,m.last_hit_at,m.decay,m."   \
    "status,m.version,"                                                                            \
    "m.supersedes,m.created_at,m.updated_at,m.source_event_ids"

static const char *memory_select_cols =
    "id,kind,layer,title,summary,content,scope_user,scope_project,scope_task,entity_key,predicate,"
    "importance,confidence,reusability,specificity,hit_count,last_hit_at,decay,status,version,"
    "supersedes,created_at,updated_at,source_event_ids";

int cbm_store_memory_get_item(cbm_store_t *s, const char *id, cbm_memory_item_t *out) {
    if (!s || !s->db || !id || !out) {
        return CBM_STORE_ERR;
    }
    char sql[CBM_SZ_1K];
    snprintf(sql, sizeof(sql), "SELECT %s FROM memory_item WHERE id=?1;", memory_select_cols);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_get_prepare");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        memory_scan_item(stmt, out);
        sqlite3_finalize(stmt);
        return CBM_STORE_OK;
    }
    sqlite3_finalize(stmt);
    return CBM_STORE_NOT_FOUND;
}

static int memory_scope_score(const cbm_memory_item_t *item, const cbm_memory_query_t *query) {
    int score = 0;
    if (!item || !query) {
        return score;
    }
    if (query->project && item->scope_project && strcmp(query->project, item->scope_project) == 0) {
        score += 4;
    }
    if (query->user && item->scope_user && strcmp(query->user, item->scope_user) == 0) {
        score += 2;
    }
    if (query->task && item->scope_task && strcmp(query->task, item->scope_task) == 0) {
        score += 1;
    }
    return score;
}

static int memory_compare_for_conflict(const cbm_memory_item_t *a, const cbm_memory_item_t *b,
                                       const cbm_memory_query_t *query) {
    /* The meaning of retrieval_score depends on the path that produced the item:
     *   - fts/vector: it is real query relevance (fts=1.0, vector=cosine). There it
     *     must stay the primary key, so a low-relevance vector candidate can't hide
     *     the high-relevance hit the user searched for.
     *   - structured (entity_key/filter): it is a storage-quality composite
     *     (importance+confidence+reusability+specificity+hit_count-decay) — NOT query
     *     relevance. Leading with it there would bake confidence into the primary key
     *     and make an explicit query scope (task/user) unable to win, which contradicts
     *     scope-aware retrieval. So on the structured path scope is primary instead,
     *     keeping this order consistent with memory_conflict_resolution_reason(). */
    bool a_structured = a->retrieval_source && strcmp(a->retrieval_source, "structured") == 0;
    bool b_structured = b->retrieval_source && strcmp(b->retrieval_source, "structured") == 0;
    if (!(a_structured && b_structured)) {
        /* fts/vector involved: relevance leads. */
        if (a->retrieval_score != b->retrieval_score) {
            return a->retrieval_score > b->retrieval_score ? 1 : -1;
        }
    }
    int as = memory_scope_score(a, query);
    int bs = memory_scope_score(b, query);
    if (as != bs) {
        return as > bs ? 1 : -1;
    }
    if (a->confidence != b->confidence) {
        return a->confidence > b->confidence ? 1 : -1;
    }
    if (a->updated_at != b->updated_at) {
        return a->updated_at > b->updated_at ? 1 : -1;
    }
    if (a->hit_count != b->hit_count) {
        return a->hit_count > b->hit_count ? 1 : -1;
    }
    return strcmp(safe_str(a->id), safe_str(b->id)) <= 0 ? 1 : -1;
}

static bool memory_items_contradict(cbm_store_t *s, const char *a, const char *b) {
    if (!s || !s->db || !a || !b) {
        return false;
    }
    const char *sql = "SELECT 1 FROM memory_edge WHERE type='contradicts' AND "
                      "((src_id=?1 AND dst_id=?2) OR (src_id=?2 AND dst_id=?1)) LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    bind_text(stmt, 1, a);
    bind_text(stmt, 2, b);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

static void memory_append_conflict_id(cbm_memory_item_t *winner, const char *id) {
    if (!winner || !id || !id[0]) {
        return;
    }
    const char *old = winner->conflict_ids;
    size_t old_len = old ? strlen(old) : 0;
    size_t add_len = strlen(id);
    size_t sep_len = old_len > 0 ? 1 : 0;
    char *next = malloc(old_len + sep_len + add_len + 1);
    if (!next) {
        return;
    }
    if (old_len > 0) {
        memcpy(next, old, old_len);
        next[old_len] = ',';
        memcpy(next + old_len + 1, id, add_len + 1);
    } else {
        memcpy(next, id, add_len + 1);
    }
    safe_str_free(&winner->conflict_ids);
    winner->conflict_ids = next;
    winner->conflict_count++;
}

static const char *memory_conflict_resolution_reason(const cbm_memory_item_t *winner,
                                                     const cbm_memory_item_t *loser,
                                                     const cbm_memory_query_t *query) {
    int ws = memory_scope_score(winner, query);
    int ls = memory_scope_score(loser, query);
    if (ws != ls)
        return "winner_by_scope";
    if (winner && loser && winner->confidence != loser->confidence)
        return "winner_by_confidence";
    if (winner && loser && winner->updated_at != loser->updated_at)
        return "winner_by_recency";
    if (winner && loser && winner->hit_count != loser->hit_count)
        return "winner_by_hits";
    return "winner_by_stable_id";
}

static void memory_append_conflict_resolution(cbm_memory_item_t *winner, const char *reason) {
    if (!winner || !reason || !reason[0]) {
        return;
    }
    if (winner->conflict_resolution && strstr(winner->conflict_resolution, reason)) {
        return;
    }
    const char *old = winner->conflict_resolution;
    size_t old_len = old ? strlen(old) : 0;
    size_t add_len = strlen(reason);
    size_t sep_len = old_len > 0 ? 1 : 0;
    char *next = malloc(old_len + sep_len + add_len + 1);
    if (!next) {
        return;
    }
    if (old_len > 0) {
        memcpy(next, old, old_len);
        next[old_len] = ',';
        memcpy(next + old_len + 1, reason, add_len + 1);
    } else {
        memcpy(next, reason, add_len + 1);
    }
    safe_str_free(&winner->conflict_resolution);
    winner->conflict_resolution = next;
}
static bool memory_evidence_edge_type(const char *type) {
    return type && (strcmp(type, "supports") == 0 || strcmp(type, "derived_from") == 0 ||
                    strcmp(type, "used_in") == 0 || strcmp(type, "belongs_to") == 0 ||
                    strcmp(type, "contradicts") == 0);
}

static void memory_json_append_escaped(char *buf, size_t sz, size_t *pos, const char *s) {
    if (!buf || !pos || *pos >= sz) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && *pos + 2 < sz; p++) {
        /* Loop guard guarantees *pos + 2 < sz on entry and *pos is unchanged
         * until the writes below, so each 2-byte escape sequence fits without
         * a per-branch recheck. */
        if (*p == '\\' || *p == '"') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = (char)*p;
        } else if (*p == '\n') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = 'n';
        } else if (*p == '\r') {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = 'r';
        } else if (*p >= 0x20) {
            buf[(*pos)++] = (char)*p;
        }
    }
    buf[*pos < sz ? *pos : sz - 1] = '\0';
}

static void memory_evidence_add_json_edge(char *buf, size_t sz, size_t *pos, bool *first, int hop,
                                          const char *src, const char *dst, const char *type,
                                          const char *origin, double confidence) {
    if (!buf || !pos || *pos >= sz || !memory_evidence_edge_type(type)) {
        return;
    }
    int n = snprintf(buf + *pos, sz - *pos, "%s{\"hop\":%d,\"src_id\":\"", *first ? "" : ",", hop);
    if (n < 0 || (size_t)n >= sz - *pos)
        return;
    *pos += (size_t)n;
    memory_json_append_escaped(buf, sz, pos, src);
    n = snprintf(buf + *pos, sz - *pos, "\",\"dst_id\":\"");
    if (n < 0 || (size_t)n >= sz - *pos)
        return;
    *pos += (size_t)n;
    memory_json_append_escaped(buf, sz, pos, dst);
    n = snprintf(buf + *pos, sz - *pos, "\",\"type\":\"");
    if (n < 0 || (size_t)n >= sz - *pos)
        return;
    *pos += (size_t)n;
    memory_json_append_escaped(buf, sz, pos, type);
    n = snprintf(buf + *pos, sz - *pos, "\",\"origin\":\"");
    if (n < 0 || (size_t)n >= sz - *pos)
        return;
    *pos += (size_t)n;
    memory_json_append_escaped(buf, sz, pos, origin);
    n = snprintf(buf + *pos, sz - *pos, "\",\"confidence\":%.3f}", confidence);
    if (n < 0 || (size_t)n >= sz - *pos)
        return;
    *pos += (size_t)n;
    *first = false;
}

static void memory_fill_evidence(cbm_store_t *s, cbm_memory_item_t *item) {
    if (!s || !s->db || !item || !item->id) {
        return;
    }
    char buf[CBM_SZ_4K];
    size_t pos = 0;
    bool first = true;
    int n = snprintf(buf, sizeof(buf), "[");
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return;
    }
    pos = (size_t)n;
    const char *sql =
        "WITH RECURSIVE walk(id, depth) AS ("
        "  SELECT ?1, 0 UNION "
        "  SELECT CASE WHEN e.src_id = walk.id THEN e.dst_id ELSE e.src_id END, walk.depth + 1 "
        "  FROM memory_edge e JOIN walk ON (e.src_id = walk.id OR e.dst_id = walk.id) "
        "  WHERE walk.depth < 2 AND e.type IN "
        "('supports','derived_from','used_in','belongs_to','contradicts')"
        ") "
        "SELECT DISTINCT e.src_id,e.dst_id,e.type,e.origin,e.confidence,MIN(w.depth)+1 AS hop "
        "FROM memory_edge e JOIN walk w ON (e.src_id = w.id OR e.dst_id = w.id) "
        "WHERE e.type IN ('supports','derived_from','used_in','belongs_to','contradicts') "
        "GROUP BY e.src_id,e.dst_id,e.type,e.origin,e.confidence "
        "ORDER BY hop, CASE e.type WHEN 'derived_from' THEN 0 WHEN 'supports' THEN 1 WHEN "
        "'used_in' THEN 2 WHEN 'belongs_to' THEN 3 ELSE 4 END "
        "LIMIT 24;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return;
    }
    bind_text(stmt, 1, item->id);
    while (sqlite3_step(stmt) == SQLITE_ROW && pos + 128 < sizeof(buf)) {
        const char *src = (const char *)sqlite3_column_text(stmt, 0);
        const char *dst = (const char *)sqlite3_column_text(stmt, 1);
        const char *type = (const char *)sqlite3_column_text(stmt, 2);
        const char *origin = (const char *)sqlite3_column_text(stmt, 3);
        double confidence = sqlite3_column_double(stmt, 4);
        int hop = sqlite3_column_int(stmt, 5);
        memory_evidence_add_json_edge(buf, sizeof(buf), &pos, &first, hop, src, dst, type, origin,
                                      confidence);
    }
    sqlite3_finalize(stmt);
    if (pos + 2 < sizeof(buf)) {
        buf[pos++] = ']';
        buf[pos] = '\0';
    } else {
        snprintf(buf + sizeof(buf) - 3, 3, "]");
    }
    item->evidence_json = heap_strdup(buf);
}

static void memory_fill_result_evidence(cbm_store_t *s, cbm_memory_result_t *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < out->count; i++) {
        memory_fill_evidence(s, &out->items[i]);
    }
}

/* ── P3-a: graph-signal scoring for confidence/reusability ─────────
 * When a memory is written with about_code anchors, derive confidence and
 * reusability from the code graph instead of trusting a self-reported (often
 * all-0.5) value. Signals, all objective and queryable from the borrowed graph
 * handle: anchor symbol EXISTS (real, not hallucinated), its in_degree (how
 * widely it's called → how load-bearing → confidence), its out_degree +
 * is_entry_point + #anchors (how central/architectural → reusability).
 *
 * These are intentionally configurable constants, not magic numbers baked into
 * the formula: the architecture fixes the SIGNAL SOURCES, the weights get tuned
 * against real recall data (see [[memory-lifecycle-architecture]]). */
#define MEMORY_L1_CONF_BASE 0.5         /* confidence when an anchor exists but has no callers */
#define MEMORY_L1_CONF_PER_INDEG 0.04   /* each inbound CALLS edge adds this to confidence */
#define MEMORY_L1_CONF_CAP 0.95         /* never let pure graph signal claim certainty */
#define MEMORY_L1_REUSE_BASE 0.4        /* reusability for a leaf symbol with one anchor */
#define MEMORY_L1_REUSE_ENTRY 0.3       /* bonus when any anchor is an entry point */
#define MEMORY_L1_REUSE_PER_OUTDEG 0.02 /* each outbound CALLS edge (breadth) adds this */
#define MEMORY_L1_REUSE_PER_ANCHOR 0.05 /* each additional existing anchor adds this */
#define MEMORY_L1_REUSE_CAP 1.0

/* Compute in/out CALLS degree + is_entry_point for one symbol via the borrowed
 * graph handle. Returns false if the symbol is absent (stale/hallucinated). */
static bool memory_graph_symbol_signals(sqlite3 *graph_db, const char *project, const char *qn,
                                        int *in_deg, int *out_deg, bool *is_entry) {
    *in_deg = 0;
    *out_deg = 0;
    *is_entry = false;
    if (!graph_db || !qn || !qn[0]) {
        return false;
    }
    sqlite3_stmt *st = NULL;
    int64_t node_id = -1;
    const char *node_sql =
        project ? "SELECT id, COALESCE(json_extract(properties,'$.is_entry_point'),0) "
                  "FROM nodes WHERE project=?1 AND qualified_name=?2 LIMIT 1;"
                : "SELECT id, COALESCE(json_extract(properties,'$.is_entry_point'),0) "
                  "FROM nodes WHERE qualified_name=?2 LIMIT 1;";
    if (sqlite3_prepare_v2(graph_db, node_sql, CBM_NOT_FOUND, &st, NULL) == SQLITE_OK) {
        memory_bind_nullable(st, 1, project);
        bind_text(st, 2, qn);
        if (sqlite3_step(st) == SQLITE_ROW) {
            node_id = sqlite3_column_int64(st, 0);
            /* json_extract yields 1/true or 0/false/NULL; treat any nonzero as entry. */
            *is_entry = sqlite3_column_int(st, 1) != 0;
        }
    }
    sqlite3_finalize(st);
    if (node_id < 0) {
        return false; /* symbol not in graph */
    }
    st = NULL;
    /* Degree counts ALL inbound/outbound edges, not just CALLS: a symbol's
     * load-bearing-ness comes from every dependency on it (USAGE references
     * usually dwarf direct CALLS for utility symbols), matching search_graph's
     * in_degree. The near-constant DEFINES self-edge is uniform noise the
     * scoring constants absorb. */
    if (sqlite3_prepare_v2(graph_db, "SELECT COUNT(*) FROM edges WHERE target_id=?1;",
                           CBM_NOT_FOUND, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, node_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *in_deg = sqlite3_column_int(st, 0);
        }
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(graph_db, "SELECT COUNT(*) FROM edges WHERE source_id=?1;",
                           CBM_NOT_FOUND, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, node_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *out_deg = sqlite3_column_int(st, 0);
        }
    }
    sqlite3_finalize(st);
    return true;
}

/* Derive confidence/reusability signals from a memory's about_code anchors.
 * Reads the item's about_code edges from the memory DB (s->db) and the symbol
 * topology from the borrowed graph handle. Returns the number of anchors that
 * resolve to a real graph symbol; 0 means "no usable graph signal" (caller then
 * keeps the declared values — L3 fallback). Aggregates by taking the strongest
 * signal across anchors (max in/out degree, any entry point) plus a small
 * per-anchor breadth bonus. */
int cbm_store_memory_score_from_anchors(cbm_store_t *s, sqlite3 *graph_db, const char *item_id,
                                        const char *project, double *out_conf, double *out_reuse) {
    if (out_conf) {
        *out_conf = 0.0;
    }
    if (out_reuse) {
        *out_reuse = 0.0;
    }
    if (!s || !s->db || !graph_db || !item_id || !item_id[0]) {
        return 0;
    }
    const char *anchor_sql = "SELECT substr(dst_id, 6) AS qn FROM memory_edge "
                             "WHERE src_id=?1 AND type='about_code' AND dst_id LIKE 'code:%';";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, anchor_sql, CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    bind_text(st, 1, item_id);
    int resolved = 0;
    int max_in = 0;
    int max_out = 0;
    bool any_entry = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(st, 0);
        if (!qn || !qn[0]) {
            continue;
        }
        int in_deg = 0;
        int out_deg = 0;
        bool is_entry = false;
        if (!memory_graph_symbol_signals(graph_db, project, qn, &in_deg, &out_deg, &is_entry)) {
            continue; /* stale/hallucinated anchor contributes nothing */
        }
        resolved++;
        if (in_deg > max_in) {
            max_in = in_deg;
        }
        if (out_deg > max_out) {
            max_out = out_deg;
        }
        any_entry = any_entry || is_entry;
    }
    sqlite3_finalize(st);
    if (resolved == 0) {
        return 0;
    }
    double conf = MEMORY_L1_CONF_BASE + (double)max_in * MEMORY_L1_CONF_PER_INDEG;
    if (conf > MEMORY_L1_CONF_CAP) {
        conf = MEMORY_L1_CONF_CAP;
    }
    double reuse = MEMORY_L1_REUSE_BASE + (double)max_out * MEMORY_L1_REUSE_PER_OUTDEG +
                   (double)(resolved - 1) * MEMORY_L1_REUSE_PER_ANCHOR;
    if (any_entry) {
        reuse += MEMORY_L1_REUSE_ENTRY;
    }
    if (reuse > MEMORY_L1_REUSE_CAP) {
        reuse = MEMORY_L1_REUSE_CAP;
    }
    if (out_conf) {
        *out_conf = conf;
    }
    if (out_reuse) {
        *out_reuse = reuse;
    }
    return resolved;
}

/* ── L2 (kind/type prior) + L3 (declared) composition ───────────────────────
 * Reusability correlates with memory kind: ADR-class rationale and cross-project
 * preferences are broadly reusable; episodic events are not. These priors are
 * BASELINES — floored by the L1 graph signal when anchors resolve, and offset by
 * the caller's declared value. Numbers are tunable; the architecture (which kind
 * maps where, monotonic composition) is fixed. */
#define MEMORY_L2_REUSE_DECISION 0.7    /* decision / constraint: architectural rationale */
#define MEMORY_L2_REUSE_PREFERENCE 0.75 /* user preference: applies across every project */
#define MEMORY_L2_REUSE_REFERENCE 0.6
#define MEMORY_L2_REUSE_LESSON 0.55
#define MEMORY_L2_REUSE_FACT 0.5
#define MEMORY_L2_REUSE_EVENT 0.4    /* episodic: least reusable */
#define MEMORY_L2_REUSE_DEFAULT 0.5  /* unknown kind: neutral, no opinion */
#define MEMORY_L3_DECLARED_OFFSET_WEIGHT 1.0

static double memory_kind_reuse_prior(const char *kind) {
    if (!kind || !kind[0]) {
        return MEMORY_L2_REUSE_DEFAULT;
    }
    if (strcmp(kind, "decision") == 0 || strcmp(kind, "constraint") == 0) {
        return MEMORY_L2_REUSE_DECISION;
    }
    if (strcmp(kind, "preference") == 0) {
        return MEMORY_L2_REUSE_PREFERENCE;
    }
    if (strcmp(kind, "reference") == 0) {
        return MEMORY_L2_REUSE_REFERENCE;
    }
    if (strcmp(kind, "lesson") == 0) {
        return MEMORY_L2_REUSE_LESSON;
    }
    if (strcmp(kind, "fact") == 0) {
        return MEMORY_L2_REUSE_FACT;
    }
    if (strcmp(kind, "event") == 0) {
        return MEMORY_L2_REUSE_EVENT;
    }
    return MEMORY_L2_REUSE_DEFAULT;
}

/* L3 blend: a declared value of 0.5 is the schema default ("unset") → keep the
 * base; a non-0.5 declared value is an explicit OFFSET from 0.5 the writer sees
 * that the tiers cannot (e.g. a low-degree symbol that is a future linchpin) →
 * base + (declared-0.5)*weight, clamped to [0,1]. (Moved here from the MCP
 * handler so the whole 3-tier composition lives in one place.) */
static double memory_apply_declared(double base, double declared) {
    double eps = 1e-9;
    if (declared > 0.5 - eps && declared < 0.5 + eps) {
        return base;
    }
    double v = base + (declared - 0.5) * MEMORY_L3_DECLARED_OFFSET_WEIGHT;
    if (v < 0.0) {
        v = 0.0;
    }
    if (v > 1.0) {
        v = 1.0;
    }
    return v;
}

cbm_memory_score_t cbm_memory_score_item(const char *kind, int l1_resolved, double l1_conf,
                                         double l1_reuse, double declared_conf,
                                         double declared_reuse) {
    /* reusability: L2 kind prior is the baseline; the L1 graph signal can only
     * RAISE it (monotonic max), never sink an anchored memory below its type
     * prior — which is what let a low-degree anchored ADR (0.4) score under an
     * unanchored one (0.7) and decay out first. */
    double reuse_base = memory_kind_reuse_prior(kind);
    if (l1_resolved > 0 && l1_reuse > reuse_base) {
        reuse_base = l1_reuse;
    }
    /* confidence: no kind prior (kind != correctness); 0.5 baseline raised only
     * by L1 graph evidence, then offset by the declared value. */
    double conf_base = MEMORY_L1_CONF_BASE;
    if (l1_resolved > 0 && l1_conf > conf_base) {
        conf_base = l1_conf;
    }
    cbm_memory_score_t out;
    out.confidence = memory_apply_declared(conf_base, declared_conf);
    out.reusability = memory_apply_declared(reuse_base, declared_reuse);
    return out;
}

/* Retrieval boost added to a memory anchored (via an about_code edge) to the
 * code symbol the agent is currently looking at — or to a sibling symbol in the
 * same file. Same-symbol scores higher than same-file. The boost is added to
 * retrieval_score and the result is re-sorted; it never adds or removes
 * candidates (contract: about_code is a ranking signal, not a recall path). */
#define MEMORY_ANCHOR_BOOST_SYMBOL 0.5
#define MEMORY_ANCHOR_BOOST_FILE 0.2

/* Does code symbol `qn` still exist in this project's code graph? Used to
 * lazily skip the boost for memories whose anchor target was renamed/deleted
 * since indexing (stale anchor → silent de-weight, memory itself is kept).
 * Queries the borrowed graph handle (code graph now lives in a separate DB). */
static bool memory_code_symbol_exists(sqlite3 *graph_db, const char *project, const char *qn) {
    if (!graph_db || !qn || !qn[0]) {
        return false;
    }
    sqlite3_stmt *st = NULL;
    bool exists = false;
    const char *sql = project
                          ? "SELECT 1 FROM nodes WHERE project=?1 AND qualified_name=?2 LIMIT 1;"
                          : "SELECT 1 FROM nodes WHERE qualified_name=?2 LIMIT 1;";
    if (sqlite3_prepare_v2(graph_db, sql, CBM_NOT_FOUND, &st, NULL) == SQLITE_OK) {
        memory_bind_nullable(st, 1, project);
        bind_text(st, 2, qn);
        exists = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return exists;
}

/* Apply about_code anchoring boosts to the result set, then stable-sort by the
 * adjusted retrieval_score (desc). No-op unless query->code_context is set AND a
 * borrowed graph handle is supplied, so default retrieval order is unchanged and
 * a missing/unindexed graph degrades to "no boost" rather than failing. Must run
 * before the limit truncation in memory_resolve_conflicts so a boosted memory
 * can actually rise into the cut. */
static void memory_apply_anchor_boost(cbm_store_t *s, const cbm_memory_query_t *query,
                                      cbm_memory_result_t *out) {
    if (!query || !query->code_context || !query->code_context[0] || !out || out->count <= 0) {
        return;
    }
    sqlite3 *graph_db = query->graph_db;
    /* No graph handle → anchor data is unreachable; leave scores untouched. */
    if (!graph_db) {
        return;
    }
    const char *ctx_qn = query->code_context;
    /* Resolve the file of the current symbol once, for same-file sibling boosts. */
    char ctx_file[CBM_SZ_512];
    ctx_file[0] = '\0';
    {
        sqlite3_stmt *st = NULL;
        const char *sql =
            query->project
                ? "SELECT file_path FROM nodes WHERE project=?1 AND qualified_name=?2 LIMIT 1;"
                : "SELECT file_path FROM nodes WHERE qualified_name=?2 LIMIT 1;";
        if (sqlite3_prepare_v2(graph_db, sql, CBM_NOT_FOUND, &st, NULL) == SQLITE_OK) {
            memory_bind_nullable(st, 1, query->project);
            bind_text(st, 2, ctx_qn);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *fp = (const char *)sqlite3_column_text(st, 0);
                if (fp)
                    snprintf(ctx_file, sizeof(ctx_file), "%s", fp);
            }
        }
        sqlite3_finalize(st);
    }

    /* For each result, find its strongest about_code anchor relative to ctx.
     * about_code edges live in the memory DB (s->db); node existence/file checks
     * go to the borrowed graph handle. */
    const char *anchor_sql = "SELECT substr(dst_id, 6) AS qn FROM memory_edge "
                             "WHERE src_id=?1 AND type='about_code' AND dst_id LIKE 'code:%';";
    for (int i = 0; i < out->count; i++) {
        if (!out->items[i].id)
            continue;
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(s->db, anchor_sql, CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
            continue;
        }
        bind_text(st, 1, out->items[i].id);
        double best = 0.0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *qn = (const char *)sqlite3_column_text(st, 0);
            if (!qn || !qn[0])
                continue;
            /* Stale anchor (symbol gone since indexing) → no boost, keep memory. */
            if (!memory_code_symbol_exists(graph_db, query->project, qn))
                continue;
            if (strcmp(qn, ctx_qn) == 0) {
                if (MEMORY_ANCHOR_BOOST_SYMBOL > best)
                    best = MEMORY_ANCHOR_BOOST_SYMBOL;
            } else if (ctx_file[0]) {
                /* Same-file sibling: does this anchor's symbol share ctx_file? */
                sqlite3_stmt *fst = NULL;
                const char *fsql =
                    query->project
                        ? "SELECT 1 FROM nodes WHERE project=?1 AND qualified_name=?2 AND "
                          "file_path=?3 LIMIT 1;"
                        : "SELECT 1 FROM nodes WHERE qualified_name=?2 AND file_path=?3 LIMIT 1;";
                if (sqlite3_prepare_v2(graph_db, fsql, CBM_NOT_FOUND, &fst, NULL) == SQLITE_OK) {
                    memory_bind_nullable(fst, 1, query->project);
                    bind_text(fst, 2, qn);
                    bind_text(fst, 3, ctx_file);
                    if (sqlite3_step(fst) == SQLITE_ROW && MEMORY_ANCHOR_BOOST_FILE > best) {
                        best = MEMORY_ANCHOR_BOOST_FILE;
                    }
                }
                sqlite3_finalize(fst);
            }
        }
        sqlite3_finalize(st);
        out->items[i].retrieval_score += best;
    }

    /* Stable insertion sort by adjusted score (desc). Result sets are small
     * (<= limit*4); stability preserves the prior order among equal scores. */
    for (int i = 1; i < out->count; i++) {
        cbm_memory_item_t key = out->items[i];
        int j = i - 1;
        while (j >= 0 && out->items[j].retrieval_score < key.retrieval_score) {
            out->items[j + 1] = out->items[j];
            j--;
        }
        out->items[j + 1] = key;
    }
}

static void memory_resolve_conflicts(cbm_store_t *s, const cbm_memory_query_t *query,
                                     cbm_memory_result_t *out, int limit) {
    if (!s || !out || out->count <= 1) {
        return;
    }
    int n = out->count;
    int *parent = malloc((size_t)n * sizeof(int));
    bool *hidden = calloc((size_t)n, sizeof(bool));
    if (!parent || !hidden) {
        free(parent);
        free(hidden);
        return;
    }
    for (int i = 0; i < n; i++) {
        parent[i] = i;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (!memory_items_contradict(s, out->items[i].id, out->items[j].id)) {
                continue;
            }
            int ri = i;
            while (parent[ri] != ri)
                ri = parent[ri];
            int rj = j;
            while (parent[rj] != rj)
                rj = parent[rj];
            if (ri != rj) {
                parent[rj] = ri;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        int r = i;
        while (parent[r] != r)
            r = parent[r];
        parent[i] = r;
    }
    for (int r = 0; r < n; r++) {
        int winner = -1;
        for (int i = 0; i < n; i++) {
            if (parent[i] != r) {
                continue;
            }
            if (winner < 0 ||
                memory_compare_for_conflict(&out->items[i], &out->items[winner], query) > 0) {
                winner = i;
            }
        }
        if (winner < 0) {
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (parent[i] != r || i == winner) {
                continue;
            }
            hidden[i] = true;
            memory_append_conflict_id(&out->items[winner], out->items[i].id);
            memory_append_conflict_resolution(
                &out->items[winner],
                memory_conflict_resolution_reason(&out->items[winner], &out->items[i], query));
        }
    }
    int write = 0;
    int cap = limit > 0 ? limit : n;
    for (int read = 0; read < n; read++) {
        if (hidden[read] || write >= cap) {
            cbm_store_memory_item_free(&out->items[read]);
            continue;
        }
        if (write != read) {
            out->items[write] = out->items[read];
            memset(&out->items[read], 0, sizeof(out->items[read]));
        }
        write++;
    }
    out->count = write;
    out->total = write;
    free(parent);
    free(hidden);
}

/* Memory embedding dimension. Memory now reuses the same 768-d dense space as
 * the code-graph semantic module (CBM_SEM_DIM), so a memory vector and a code
 * token vector live in one comparable geometry. Stored quantized to int8. */
/* Memory embeddings are stored at bge-m3's native width (1024) so the optional
 * sidecar backend can write its real sentence vectors directly. The in-binary
 * static fallback only produces CBM_SEM_DIM (768) meaningful dimensions and
 * zero-pads the rest — it is a degraded path (no true semantics; see embed.h),
 * but the storage/cosine format stays uniform so a DB can mix neither dim. The
 * code-graph path (node_vectors, VS_VEC_DIM) is independent and unaffected. */
enum { MEMORY_VEC_DIM = 1024 };
enum { MEMORY_VEC_STATIC_DIM = CBM_SEM_DIM }; /* meaningful dims from static path */

/* Build a dense int8 embedding from arbitrary natural-language content by
 * mean-pooling per-token nomic-embed-code vectors (768-d, distilled from the
 * 7B model). This replaces the old 256-bucket signed feature-hashing: tokens
 * that share *meaning* — not just spelling — now land close in cosine space,
 * which is what makes "semantic" memory recall actually semantic.
 *
 * Tokenization mirrors the previous scheme so CJK keeps working:
 *   - ASCII runs are lowercased and split on non-alphanumeric boundaries; each
 *     token is looked up in the nomic vocab (real vector) or, if absent, mapped
 *     to a deterministic sparse random vector (cbm_sem_random_index handles both).
 *   - CJK / multibyte text has no spaces, so each codepoint is one token plus a
 *     bigram with its predecessor; these miss the (code-oriented) nomic vocab and
 *     fall through to sparse random vectors — i.e. the same overlap property the
 *     old hashing gave, now in the shared 768-d space.
 * Each token vector is unit-normalized before pooling so a single high-magnitude
 * token can't dominate; the pooled vector is re-normalized, then quantized x127.
 *
 * This is the STATIC backend: it fills MEMORY_VEC_STATIC_DIM (768) meaningful
 * dimensions and the dispatcher zero-pads to MEMORY_VEC_DIM (1024). */
static void memory_feature_vec_static(const char *content, int8_t vec[MEMORY_VEC_DIM]) {
    cbm_sem_vec_t acc;
    memset(&acc, 0, sizeof(acc));
    cbm_sem_vec_t tv;

    const unsigned char *p = (const unsigned char *)(content ? content : "");
    char tok[64];
    int tlen = 0;
    char prev_cp[8];
    int prev_len = 0;
    bool have_prev = false;

/* Pool one token's unit vector into the accumulator. */
#define MEM_POOL_TOKEN(str)                      \
    do {                                         \
        cbm_sem_random_index((str), &tv);        \
        cbm_sem_normalize(&tv);                  \
        cbm_sem_vec_add_scaled(&acc, &tv, 1.0F); \
    } while (0)

    while (*p) {
        unsigned char c = *p;
        if (c < 0x80) {
            if (isalnum(c)) {
                if (tlen < (int)sizeof(tok) - 1) {
                    tok[tlen++] = (char)tolower(c);
                }
                p++;
            } else {
                if (tlen > 0) {
                    tok[tlen] = '\0';
                    MEM_POOL_TOKEN(tok);
                    tlen = 0;
                }
                p++;
            }
            have_prev = false;
            continue;
        }
        /* Multibyte (CJK etc.): flush any pending ASCII token first. */
        if (tlen > 0) {
            tok[tlen] = '\0';
            MEM_POOL_TOKEN(tok);
            tlen = 0;
        }
        int n = 1;
        if ((c & 0xE0) == 0xC0)
            n = 2;
        else if ((c & 0xF0) == 0xE0)
            n = 3;
        else if ((c & 0xF8) == 0xF0)
            n = 4;
        for (int k = 1; k < n; k++) {
            if (!p[k]) {
                n = k;
                break;
            }
        }
        if (n < 1)
            n = 1;
        /* Unigram: the codepoint bytes as a NUL-terminated token. */
        char cp_tok[8];
        int cp_len = n < (int)sizeof(cp_tok) ? n : (int)sizeof(cp_tok) - 1;
        memcpy(cp_tok, p, (size_t)cp_len);
        cp_tok[cp_len] = '\0';
        MEM_POOL_TOKEN(cp_tok);
        /* Bigram with previous codepoint: prev+curr bytes as one token. */
        if (have_prev) {
            char bg[16];
            int bl = 0;
            for (int k = 0; k < prev_len && bl < (int)sizeof(bg) - 1; k++)
                bg[bl++] = prev_cp[k];
            for (int k = 0; k < cp_len && bl < (int)sizeof(bg) - 1; k++)
                bg[bl++] = cp_tok[k];
            bg[bl] = '\0';
            MEM_POOL_TOKEN(bg);
        }
        memcpy(prev_cp, cp_tok, (size_t)cp_len);
        prev_len = cp_len;
        have_prev = true;
        p += n;
    }
    if (tlen > 0) {
        tok[tlen] = '\0';
        MEM_POOL_TOKEN(tok);
    }
#undef MEM_POOL_TOKEN

    /* Re-normalize the pooled vector, then quantize to int8 (x127). cosine is
     * scale-invariant, so quantization only affects storage, not ranking. The
     * accumulator is CBM_SEM_DIM (768) wide; zero-pad the remaining dimensions
     * up to MEMORY_VEC_DIM (1024) so the stored width matches the sidecar's. */
    cbm_sem_normalize(&acc);
    for (int i = 0; i < MEMORY_VEC_STATIC_DIM; i++) {
        double v = (double)acc.v[i] * 127.0;
        if (v > 127.0)
            v = 127.0;
        if (v < -127.0)
            v = -127.0;
        vec[i] = (int8_t)lround(v);
    }
    for (int i = MEMORY_VEC_STATIC_DIM; i < MEMORY_VEC_DIM; i++) {
        vec[i] = 0;
    }
}

/* Embedding dispatcher: prefer the sidecar (real multilingual sentence model)
 * when enabled and healthy, else fall back to the static in-binary embedder.
 * Both produce a MEMORY_VEC_DIM int8 unit vector. */
static void memory_feature_vec(const char *content, int8_t vec[MEMORY_VEC_DIM]) {
    if (cbm_embed_backend() == CBM_EMBED_SIDECAR &&
        cbm_embed_text(content ? content : "", vec, MEMORY_VEC_DIM) == 0) {
        return;
    }
    memory_feature_vec_static(content, vec);
}

static bool memory_result_has_id(const cbm_memory_result_t *out, const char *id) {
    if (!out || !id) {
        return false;
    }
    for (int i = 0; i < out->count; i++) {
        if (out->items[i].id && strcmp(out->items[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

static bool memory_result_append_item(cbm_memory_result_t *out, int *cap,
                                      const cbm_memory_item_t *src, const char *source,
                                      double score) {
    if (!out || !cap || !src || !src->id || memory_result_has_id(out, src->id)) {
        return false;
    }
    if (out->count >= *cap) {
        int next_cap = *cap > 0 ? *cap * 2 : 8;
        cbm_memory_item_t *next = realloc(out->items, (size_t)next_cap * sizeof(cbm_memory_item_t));
        if (!next) {
            return false;
        }
        memset(next + *cap, 0, (size_t)(next_cap - *cap) * sizeof(cbm_memory_item_t));
        out->items = next;
        *cap = next_cap;
    }
    out->items[out->count] = *src;
    out->items[out->count].retrieval_source = heap_strdup(source ? source : "structured");
    out->items[out->count].retrieval_score = score;
    out->count++;
    out->total = out->count;
    return true;
}

static int memory_append_vector_candidates(cbm_store_t *s, const cbm_memory_query_t *query,
                                           cbm_memory_result_t *out, int *cap, int fetch_limit) {
    if (!s || !s->db || !query || !query->query || !query->query[0] || !out || !cap) {
        return CBM_STORE_OK;
    }
    int8_t qvec[MEMORY_VEC_DIM];
    memory_feature_vec(query->query, qvec);
    const char *sql =
        "SELECT " MEMORY_SELECT_RAW ", cbm_cosine_i8(v.embedding, ?1) AS vscore "
        "FROM memory_vec v JOIN memory_item m ON m.id = v.item_id "
        "WHERE (?2 IS NULL OR m.scope_project=?2) AND (?3 IS NULL OR m.scope_user=?3 OR "
        "m.scope_user IS NULL) AND "
        "(?4 IS NULL OR m.scope_task=?4 OR m.scope_task IS NULL) AND (?5 IS NULL OR "
        "m.entity_key=?5) AND "
        "(?6 IS NULL OR m.kind=?6) AND (?7 != 0 OR m.status IN ('active','candidate')) "
        "AND m.deleted_at IS NULL "
        "ORDER BY vscore DESC, (m.importance + m.confidence + m.reusability + m.specificity + "
        "m.hit_count - m.decay) DESC "
        "LIMIT ?8;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_OK;
    }
    sqlite3_bind_blob(stmt, 1, qvec, (int)sizeof(qvec), SQLITE_TRANSIENT);
    memory_bind_nullable(stmt, 2, query->project);
    memory_bind_nullable(stmt, 3, query->user);
    memory_bind_nullable(stmt, 4, query->task);
    memory_bind_nullable(stmt, 5, query->entity_key);
    memory_bind_nullable(stmt, 6, query->kind);
    sqlite3_bind_int(stmt, 7, query->include_inactive ? 1 : 0);
    sqlite3_bind_int(stmt, 8, fetch_limit > 0 ? fetch_limit : 40);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cbm_memory_item_t item = {0};
        memory_scan_item(stmt, &item);
        double score = sqlite3_column_double(stmt, 24);
        if (!memory_result_append_item(out, cap, &item, "vector", score)) {
            cbm_store_memory_item_free(&item);
        }
    }
    sqlite3_finalize(stmt);
    return CBM_STORE_OK;
}

static int memory_retrieve_vector_only(cbm_store_t *s, const cbm_memory_query_t *query,
                                       cbm_memory_result_t *out, int limit) {
    int cap = limit > 0 ? limit * 4 : 40;
    out->items = calloc((size_t)cap, sizeof(cbm_memory_item_t));
    out->count = 0;
    out->total = 0;
    if (!out->items) {
        return CBM_STORE_ERR;
    }
    (void)memory_append_vector_candidates(s, query, out, &cap, cap);
    memory_apply_anchor_boost(s, query, out);
    memory_resolve_conflicts(s, query, out, limit);
    memory_fill_result_evidence(s, out);
    return CBM_STORE_OK;
}

/* Minimum fraction of query tokens that must appear in a document for an
 * OR-joined FTS hit to count, for multi-token queries. Below this it's
 * incidental single-bigram overlap (noise), not a real match. */
#define MEMORY_FTS_MIN_OVERLAP 0.30

/* Count how many space-separated tokens of seg_query also appear (as whole
 * space-delimited tokens) in the segmented form of content. Used to gate
 * OR-joined FTS hits: a query that shares only one common bigram (e.g. "用户")
 * with a document is noise, not a match. Returns matched/total in *ratio. */
static int memory_token_overlap(const char *seg_query, const char *content, double *ratio) {
    if (ratio)
        *ratio = 0.0;
    if (!seg_query || !seg_query[0] || !content)
        return 0;
    char *seg_c = memory_segment_cjk(content);
    if (!seg_c)
        return 0;
    /* Wrap content tokens in spaces so " tok " substring search is boundary-safe. */
    size_t cl = strlen(seg_c);
    char *padded = malloc(cl + 3);
    if (!padded) {
        free(seg_c);
        return 0;
    }
    padded[0] = ' ';
    memcpy(padded + 1, seg_c, cl);
    padded[cl + 1] = ' ';
    padded[cl + 2] = '\0';
    int total = 0, matched = 0;
    const char *p = seg_query;
    char tok[64];
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        int tl = 0;
        while (*p && *p != ' ' && tl < (int)sizeof(tok) - 3)
            tok[tl++] = *p++;
        while (*p && *p != ' ')
            p++; /* skip overflow tail */
        if (tl == 0)
            continue;
        char needle[68];
        needle[0] = ' ';
        memcpy(needle + 1, tok, (size_t)tl);
        needle[tl + 1] = ' ';
        needle[tl + 2] = '\0';
        total++;
        if (strstr(padded, needle))
            matched++;
    }
    free(seg_c);
    free(padded);
    if (ratio && total > 0)
        *ratio = (double)matched / (double)total;
    return matched;
}

int cbm_store_memory_retrieve(cbm_store_t *s, const cbm_memory_query_t *query,
                              cbm_memory_result_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    int limit = (query && query->limit > 0) ? query->limit : 10;
    bool has_text = query && query->query && query->query[0];
    const char *project = query ? query->project : NULL;

    /* --- FTS5 path: text query with FTS5 BM25 ranking --- */
    if (has_text) {
        const char *fts_sql =
            "SELECT " MEMORY_SELECT_RAW " FROM ("
            "  SELECT item_id, bm25(memory_fts) AS rank "
            "  FROM memory_fts WHERE memory_fts MATCH ?1 "
            "  ORDER BY rank LIMIT ?8"
            ") fts "
            "JOIN memory_item m ON m.id = fts.item_id "
            "WHERE (?2 IS NULL OR m.scope_project=?2) AND (?3 IS NULL OR m.scope_user=?3 OR "
            "m.scope_user IS NULL) AND "
            "  (?4 IS NULL OR m.scope_task=?4 OR m.scope_task IS NULL) AND (?5 IS NULL OR "
            "m.entity_key=?5) AND "
            "  (?6 IS NULL OR m.kind=?6) AND (?7 != 0 OR m.status IN ('active','candidate')) "
            "  AND m.deleted_at IS NULL "
            "ORDER BY fts.rank LIMIT ?9;";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(s->db, fts_sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
            return memory_retrieve_vector_only(s, query, out, limit);
        }
        /* Segment the query the same way the index was built: ASCII words kept
         * whole, CJK split into character bigrams. This aligns query tokens with
         * the bigram-indexed FTS content so Chinese MATCH works. The segmenter
         * emits only alnum/CJK tokens separated by spaces — no FTS5 operators.
         * We OR-join the tokens: a space (implicit AND) requires every bigram to
         * be present, which is too strict for multi-word queries (e.g. "包管理项目"
         * shares no single doc holding all bigrams). OR maximizes recall and bm25
         * ranking floats the best overlap to the top. */
        char fts_buf[CBM_SZ_1K];
        char *seg_q = memory_segment_cjk(query->query);
        if (seg_q && seg_q[0]) {
            size_t pos = 0;
            for (const char *sp = seg_q; *sp && pos < sizeof(fts_buf) - 5;) {
                if (*sp == ' ') {
                    /* token separator -> " OR " */
                    if (pos > 0 && pos < sizeof(fts_buf) - 5) {
                        memcpy(fts_buf + pos, " OR ", 4);
                        pos += 4;
                    }
                    while (*sp == ' ')
                        sp++;
                } else {
                    fts_buf[pos++] = *sp++;
                }
            }
            fts_buf[pos] = '\0';
            if (pos == 0)
                snprintf(fts_buf, sizeof(fts_buf), "%s", query->query);
        } else {
            snprintf(fts_buf, sizeof(fts_buf), "%s", query->query);
        }
        /* seg_q is kept alive past the MATCH binding: the fetch loop uses it to
         * gate each hit by token overlap (drops single-shared-bigram noise). */
        bind_text(stmt, 1, fts_buf);
        memory_bind_nullable(stmt, 2, project);
        memory_bind_nullable(stmt, 3, query->user);
        memory_bind_nullable(stmt, 4, query->task);
        memory_bind_nullable(stmt, 5, query->entity_key);
        memory_bind_nullable(stmt, 6, query->kind);
        sqlite3_bind_int(stmt, 7, query->include_inactive ? 1 : 0);
        sqlite3_bind_int(stmt, 8, 500); /* inner FTS candidate limit */
        int fetch_limit = limit > 0 ? limit * 4 : 40;
        if (fetch_limit < limit)
            fetch_limit = limit;
        sqlite3_bind_int(stmt, 9, fetch_limit);
        int cap = fetch_limit > 0 ? fetch_limit : 10;
        cbm_memory_item_t *items = calloc((size_t)cap, sizeof(cbm_memory_item_t));
        int n = 0;
        int step_rc = SQLITE_ROW;
        /* Count query tokens: single-token queries (e.g. "pnpm") must always pass
         * the overlap gate, since one token at 100% overlap is a legitimate match. */
        int qtok = 0;
        for (const char *sp = seg_q ? seg_q : ""; *sp;) {
            while (*sp == ' ')
                sp++;
            if (!*sp)
                break;
            qtok++;
            while (*sp && *sp != ' ')
                sp++;
        }
        while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW && n < cap) {
            memory_scan_item(stmt, &items[n]);
            /* Relevance score = query-normalized token overlap, NOT a flat 1.0.
             * memory_token_overlap returns matched_query_bigrams/total_query_bigrams
             * (independent of doc length). Compute it against title/summary/content
             * and take the MAX: summary is the distilled, query-like field (high
             * signal); content is a long ADR body where a query bigram can land in
             * an unrelated section (noise) — max lets a clean summary match win
             * without being diluted by the long body. A flat 1.0 made an incidental
             * single-bigram match tie a full match, letting long/global memories
             * crowd out the real target (see recall-eval pollution finding). */
            double ov = 0.0, ov_t = 0.0, ov_s = 0.0, ov_c = 0.0;
            if (seg_q) {
                if (items[n].title && items[n].title[0])
                    (void)memory_token_overlap(seg_q, items[n].title, &ov_t);
                if (items[n].summary && items[n].summary[0])
                    (void)memory_token_overlap(seg_q, items[n].summary, &ov_s);
                (void)memory_token_overlap(seg_q, items[n].content, &ov_c);
                ov = ov_t;
                if (ov_s > ov)
                    ov = ov_s;
                if (ov_c > ov)
                    ov = ov_c;
            }
            /* Overlap gate: with OR-join a doc sharing just one common bigram
             * (e.g. "用户") matches but is noise. Require >=30% of query tokens
             * present (in the best-matching field) for multi-token queries;
             * single-token queries always pass (one token at 100% is legitimate). */
            if (qtok > 1 && ov < MEMORY_FTS_MIN_OVERLAP) {
                cbm_store_memory_item_free(&items[n]);
                memset(&items[n], 0, sizeof(items[n]));
                continue;
            }
            items[n].retrieval_source = heap_strdup("fts");
            /* Band the overlap into [0.5,1.0]: a lexical FTS hit is a CONFIRMED
             * match and must rank above pure-vector (semantic-guess) candidates
             * (~0.1-0.4) — comparing the raw overlap against cosine on the same
             * scale would sink a stopword-only lexical match below a strong
             * semantic neighbor. Within FTS hits, overlap still orders them so a
             * full match outranks an incidental single-bigram one (the global-
             * pollution fix). Single-token matches FTS-confirmed but with 0 literal
             * overlap (stemming/diacritics) are treated as full (1.0). */
            double ov_for_score = ov > 0.0 ? ov : (qtok <= 1 ? 1.0 : MEMORY_FTS_MIN_OVERLAP);
            items[n].retrieval_score = 0.5 + 0.5 * ov_for_score;
            n++;
        }
        free(seg_q);
        if (step_rc != SQLITE_DONE && step_rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            free(items);
            return memory_retrieve_vector_only(s, query, out, limit);
        }
        sqlite3_finalize(stmt);
        out->items = items;
        out->count = n;
        out->total = n;
        (void)memory_append_vector_candidates(s, query, out, &cap, fetch_limit);
        memory_apply_anchor_boost(s, query, out);
        memory_resolve_conflicts(s, query, out, limit);
        memory_fill_result_evidence(s, out);
        return CBM_STORE_OK;
    }

    /* --- Structured path: no text query, filter-only --- */
    char sql[CBM_SZ_4K];
    snprintf(
        sql, sizeof(sql),
        "SELECT %s FROM memory_item WHERE "
        "(?1 IS NULL OR scope_project=?1) AND (?2 IS NULL OR scope_user=?2 OR scope_user IS NULL) "
        "AND "
        "(?3 IS NULL OR scope_task=?3 OR scope_task IS NULL) AND (?4 IS NULL OR entity_key=?4) AND "
        "(?5 IS NULL OR kind=?5) AND (?6 != 0 OR status IN ('active','candidate')) "
        "AND deleted_at IS NULL "
        "ORDER BY (importance + confidence + reusability + specificity + hit_count - decay) DESC, "
        "updated_at DESC "
        "LIMIT %d;",
        memory_select_cols, limit > 0 ? limit * 4 : 40);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_retrieve_prepare");
        return CBM_STORE_ERR;
    }
    memory_bind_nullable(stmt, 1, project);
    memory_bind_nullable(stmt, 2, query ? query->user : NULL);
    memory_bind_nullable(stmt, 3, query ? query->task : NULL);
    memory_bind_nullable(stmt, 4, query ? query->entity_key : NULL);
    memory_bind_nullable(stmt, 5, query ? query->kind : NULL);
    sqlite3_bind_int(stmt, 6, query && query->include_inactive ? 1 : 0);
    int cap = limit > 0 ? limit * 4 : 40;
    cbm_memory_item_t *items = calloc((size_t)cap, sizeof(cbm_memory_item_t));
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < cap) {
        memory_scan_item(stmt, &items[n]);
        items[n].retrieval_source = heap_strdup("structured");
        items[n].retrieval_score = items[n].importance + items[n].confidence +
                                   items[n].reusability + items[n].specificity +
                                   items[n].hit_count - items[n].decay;
        n++;
    }
    sqlite3_finalize(stmt);
    out->items = items;
    out->count = n;
    out->total = n;
    memory_apply_anchor_boost(s, query, out);
    memory_resolve_conflicts(s, query, out, limit);
    memory_fill_result_evidence(s, out);
    return CBM_STORE_OK;
}

int cbm_store_memory_mark_hits(cbm_store_t *s, const char **ids, int count, int64_t now_ms) {
    if (!s || !s->db || !ids || count <= 0) {
        return CBM_STORE_OK;
    }
    /* A successful recall is the strongest "still useful" signal (framework §6 principle 6,
     * §11.2): bump the hit counter, refresh recency, and let accumulated decay fall back so a
     * repeatedly-recalled item climbs back from the archival threshold.
     *
     * P2: ADR items (kind='decision','constraint') use a gentler decay penalty (-0.07 instead
     * of -0.10). ADRs encode "why" knowledge whose half-life is inherently longer than general
     * episodic entries, so a recall should not erase as much accumulated decay. The hit is
     * still registered — only the decay-recovery gradient differs. */
    const char *sql_adr =
        "UPDATE memory_item SET hit_count=hit_count+1,last_hit_at=?1,"
        "decay=MAX(0.0,decay-0.07),updated_at=?1 WHERE id=?2 AND kind IN ('decision','constraint');";
    const char *sql_gen =
        "UPDATE memory_item SET hit_count=hit_count+1,last_hit_at=?1,"
        "decay=MAX(0.0,decay-0.10),updated_at=?1 WHERE id=?2;";
    sqlite3_stmt *stmt_adr = NULL;
    sqlite3_stmt *stmt_gen = NULL;
    if (sqlite3_prepare_v2(s->db, sql_adr, CBM_NOT_FOUND, &stmt_adr, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_hit_prepare_adr");
        return CBM_STORE_ERR;
    }
    if (sqlite3_prepare_v2(s->db, sql_gen, CBM_NOT_FOUND, &stmt_gen, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt_adr);
        store_set_error_sqlite(s, "memory_hit_prepare_gen");
        return CBM_STORE_ERR;
    }
    int64_t ts = now_ms > 0 ? now_ms : memory_now_ms();
    for (int i = 0; i < count; i++) {
        if (!ids[i]) {
            continue;
        }
        /* Try the ADR-preferring statement first; if it matches no row (the item is
         * not a decision/constraint), the generic statement catches it on the next try. */
        sqlite3_reset(stmt_adr);
        sqlite3_clear_bindings(stmt_adr);
        sqlite3_bind_int64(stmt_adr, 1, ts);
        bind_text(stmt_adr, 2, ids[i]);
        int rc = sqlite3_step(stmt_adr);
        if (rc == SQLITE_DONE && sqlite3_changes(s->db) == 0) {
            /* Not an ADR item — apply the generic decay penalty. */
            sqlite3_reset(stmt_gen);
            sqlite3_clear_bindings(stmt_gen);
            sqlite3_bind_int64(stmt_gen, 1, ts);
            bind_text(stmt_gen, 2, ids[i]);
            (void)sqlite3_step(stmt_gen);
        }
    }
    sqlite3_finalize(stmt_adr);
    sqlite3_finalize(stmt_gen);
    return CBM_STORE_OK;
}

static bool memory_status_allowed(const char *status) {
    return status && (strcmp(status, "candidate") == 0 || strcmp(status, "active") == 0 ||
                      strcmp(status, "deprecated") == 0 || strcmp(status, "archived") == 0 ||
                      strcmp(status, "retracted") == 0);
}

int cbm_store_memory_update_status(cbm_store_t *s, const char *id, const char *project,
                                   const char *status) {
    if (!s || !s->db || !id || !id[0] || !memory_status_allowed(status)) {
        return CBM_STORE_ERR;
    }
    const char *sql = "UPDATE memory_item SET status=?1,updated_at=?2 WHERE id=?3 AND (?4 IS NULL "
                      "OR scope_project=?4);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_update_status_prepare");
        return CBM_STORE_ERR;
    }
    /* The status UPDATE and its audit event commit together (P0-4): a crash
     * between them would otherwise leave a lifecycle change with no trace. */
    if (cbm_store_begin(s) != CBM_STORE_OK) {
        sqlite3_finalize(stmt);
        store_set_error_sqlite(s, "memory_update_status_begin");
        return CBM_STORE_ERR;
    }
    bind_text(stmt, 1, status);
    sqlite3_bind_int64(stmt, 2, memory_now_ms());
    bind_text(stmt, 3, id);
    memory_bind_nullable(stmt, 4, project);
    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(s->db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_update_status");
        return CBM_STORE_ERR;
    }
    if (changed <= 0) {
        /* No matching item: nothing changed, so don't record an audit event. */
        cbm_store_rollback(s);
        return CBM_STORE_NOT_FOUND;
    }
    if (memory_delete_audit(s, "status_change", status, id, project, NULL) != CBM_STORE_OK) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_update_status_audit");
        return CBM_STORE_ERR;
    }
    return cbm_store_commit(s);
}
static bool memory_feedback_allowed(const char *feedback) {
    return feedback && (strcmp(feedback, "useful") == 0 || strcmp(feedback, "not_useful") == 0 ||
                        strcmp(feedback, "wrong") == 0 || strcmp(feedback, "stale") == 0);
}

int cbm_store_memory_feedback(cbm_store_t *s, const char *id, const char *project,
                              const char *feedback, const char *note, const char *user,
                              char **out_event_id) {
    if (out_event_id)
        *out_event_id = NULL;
    if (!s || !s->db || !id || !id[0] || !memory_feedback_allowed(feedback)) {
        return CBM_STORE_ERR;
    }

    const char *sql = NULL;
    if (strcmp(feedback, "useful") == 0) {
        sql = "UPDATE memory_item SET hit_count=hit_count+1,last_hit_at=?1,"
              "confidence=MIN(1.0,confidence+0.05),reusability=MIN(1.0,reusability+0.05),"
              "decay=MAX(0.0,decay-0.10),updated_at=?1 WHERE id=?2 AND (?3 IS NULL OR "
              "scope_project=?3);";
    } else if (strcmp(feedback, "not_useful") == 0) {
        sql = "UPDATE memory_item SET "
              "confidence=MAX(0.0,confidence-0.05),decay=decay+0.20,updated_at=?1 "
              "WHERE id=?2 AND (?3 IS NULL OR scope_project=?3);";
    } else if (strcmp(feedback, "wrong") == 0) {
        /* P4 zombie fix: importance is an independent positive term in the recall
         * ranking (importance+confidence+reusability+...-decay), so dropping only
         * confidence/decay leaves a falsified-but-high-importance item squatting
         * near the top. A `wrong` verdict must also collapse importance. */
        sql = "UPDATE memory_item SET "
              "confidence=MAX(0.0,confidence-0.20),importance=MAX(0.0,importance-0.50),"
              "decay=decay+1.0,status='retracted',updated_at=?1 "
              "WHERE id=?2 AND (?3 IS NULL OR scope_project=?3);";
    } else {
        /* stale: was once right, now outdated → archive and ease importance down
         * (P4 zombie fix) so it can't keep ranking high from beyond the mainline. */
        sql = "UPDATE memory_item SET decay=MAX(decay,1.0),importance=MAX(0.0,importance-0.20),"
              "status='archived',updated_at=?1 "
              "WHERE id=?2 AND (?3 IS NULL OR scope_project=?3);";
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_feedback_prepare");
        return CBM_STORE_ERR;
    }

    /* The item UPDATE and its audit event must commit together: a crash between
     * them would otherwise leave an item permanently retracted/archived with no
     * trace of who/why. Wrap both in one transaction. */
    if (cbm_store_begin(s) != CBM_STORE_OK) {
        sqlite3_finalize(stmt);
        store_set_error_sqlite(s, "memory_feedback_begin");
        return CBM_STORE_ERR;
    }

    sqlite3_bind_int64(stmt, 1, memory_now_ms());
    bind_text(stmt, 2, id);
    memory_bind_nullable(stmt, 3, project);
    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(s->db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_feedback_update");
        return CBM_STORE_ERR;
    }
    if (changed <= 0) {
        /* No matching item: nothing changed, so don't record an audit event. */
        cbm_store_rollback(s);
        return CBM_STORE_NOT_FOUND;
    }

    char payload[CBM_SZ_1K];
    snprintf(payload, sizeof(payload), "feedback=%s item_id=%s note=%s", feedback, id,
             note ? note : "");
    char context[CBM_SZ_512];
    snprintf(context, sizeof(context), "{\"item_id\":\"%s\",\"feedback\":\"%s\"}", id, feedback);
    cbm_memory_event_t ev = {0};
    ev.type = "feedback";
    ev.source = "mcp.memory_feedback";
    ev.project = project;
    ev.user = user;
    ev.payload = payload;
    ev.confidence = 1.0;
    ev.context_json = context;
    if (cbm_store_memory_append_event(s, &ev, out_event_id) != CBM_STORE_OK) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_feedback_event");
        return CBM_STORE_ERR;
    }
    return cbm_store_commit(s);
}

/* ── Hard delete + soft delete + retention sweep (P0-2) ──────────────
 * Three deletion semantics, all transaction-wrapped (mirroring the P0-1
 * feedback pattern) so a crash mid-delete never leaves an item stripped of
 * some-but-not-all satellite rows:
 *   - soft  : set deleted_at; hidden from retrieval, undoable until the sweep
 *             physically purges it past the grace window.
 *   - hard  : delete item + vec + fts + edges now; source events KEPT (audit).
 *   - purge : hard, plus delete the item's own source events (GDPR erasure).
 * Every path leaves a tombstone audit event (the tombstone itself survives
 * even in purge mode — it records who/when/why, not the erased content). */

/* Delete the item's satellite rows (vec, fts, edges) and the item itself.
 * Caller must already hold an open transaction. When purge_event_ids is non-NULL
 * it is the item's source_event_ids JSON; every quoted id in it is deleted from
 * memory_event too. Returns CBM_STORE_OK / CBM_STORE_ERR. */
static int memory_delete_rows(cbm_store_t *s, const char *id, const char *project,
                              const char *purge_event_ids) {
    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(s->db, "DELETE FROM memory_fts WHERE item_id=?1;", CBM_NOT_FOUND, &st,
                           NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_delete_fts");
        return CBM_STORE_ERR;
    }
    bind_text(st, 1, id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        store_set_error_sqlite(s, "memory_delete_fts_step");
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(s->db, "DELETE FROM memory_vec WHERE item_id=?1;", CBM_NOT_FOUND, &st,
                           NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_delete_vec");
        return CBM_STORE_ERR;
    }
    bind_text(st, 1, id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        store_set_error_sqlite(s, "memory_delete_vec_step");
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(st);
    st = NULL;

    /* Cascade every edge touching this item, in either direction — covers
     * about_code anchors, contradicts/supersedes links, evidence edges. */
    if (sqlite3_prepare_v2(s->db, "DELETE FROM memory_edge WHERE src_id=?1 OR dst_id=?1;",
                           CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_delete_edge");
        return CBM_STORE_ERR;
    }
    bind_text(st, 1, id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        store_set_error_sqlite(s, "memory_delete_edge_step");
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(st);
    st = NULL;

    /* GDPR erasure: delete the item's own source events. source_event_ids is a
     * JSON array of quoted ids; walk every "..."-quoted token and delete it. */
    if (purge_event_ids && purge_event_ids[0]) {
        sqlite3_stmt *del = NULL;
        if (sqlite3_prepare_v2(s->db, "DELETE FROM memory_event WHERE id=?1;", CBM_NOT_FOUND, &del,
                               NULL) == SQLITE_OK) {
            const char *p = purge_event_ids;
            while ((p = strchr(p, '"')) != NULL) {
                p++;
                const char *e = strchr(p, '"');
                if (!e)
                    break;
                size_t n = (size_t)(e - p);
                if (n > 0 && n < CBM_SZ_128) {
                    char evid[CBM_SZ_128];
                    memcpy(evid, p, n);
                    evid[n] = '\0';
                    sqlite3_reset(del);
                    sqlite3_clear_bindings(del);
                    bind_text(del, 1, evid);
                    (void)sqlite3_step(del);
                }
                p = e + 1;
            }
            sqlite3_finalize(del);
        }
    }

    /* The item row last, scope-guarded so a delete can't cross project bounds. */
    if (sqlite3_prepare_v2(
            s->db, "DELETE FROM memory_item WHERE id=?1 AND (?2 IS NULL OR scope_project=?2);",
            CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_delete_item");
        return CBM_STORE_ERR;
    }
    bind_text(st, 1, id);
    memory_bind_nullable(st, 2, project);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        store_set_error_sqlite(s, "memory_delete_item_step");
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(st);
    return CBM_STORE_OK;
}

/* Write a tombstone audit event for a delete/soft-delete/restore. Caller holds
 * an open transaction. Best-effort id capture; returns the append result. */
static int memory_delete_audit(cbm_store_t *s, const char *type, const char *mode, const char *id,
                               const char *project, const char *user) {
    char payload[CBM_SZ_512];
    snprintf(payload, sizeof(payload), "mode=%s item_id=%s", mode ? mode : "", id ? id : "");
    char context[CBM_SZ_512];
    snprintf(context, sizeof(context), "{\"item_id\":\"%s\",\"mode\":\"%s\"}", id ? id : "",
             mode ? mode : "");
    cbm_memory_event_t ev = {0};
    ev.type = type;
    ev.source = "mcp.memory_delete";
    ev.project = project;
    ev.user = user;
    ev.payload = payload;
    ev.confidence = 1.0;
    ev.context_json = context;
    return cbm_store_memory_append_event(s, &ev, NULL);
}

int cbm_store_memory_delete(cbm_store_t *s, const char *id, const char *project, const char *mode,
                            const char *user) {
    if (!s || !s->db || !id || !id[0]) {
        return CBM_STORE_ERR;
    }
    if (!mode || !mode[0]) {
        mode = "soft";
    }
    bool is_soft = strcmp(mode, "soft") == 0;
    bool is_hard = strcmp(mode, "hard") == 0;
    bool is_purge = strcmp(mode, "purge") == 0;
    if (!is_soft && !is_hard && !is_purge) {
        return CBM_STORE_ERR;
    }

    /* Pre-flight outside any transaction: load the item (scope-guarded) so we can
     * return NOT_FOUND cleanly, and capture source_event_ids for purge. For soft,
     * an already-soft-deleted item is treated as not-found (idempotent). */
    cbm_memory_item_t item = {0};
    int grc = cbm_store_memory_get_item(s, id, &item);
    if (grc != CBM_STORE_OK) {
        return CBM_STORE_NOT_FOUND;
    }
    if (project && item.scope_project && strcmp(project, item.scope_project) != 0) {
        cbm_store_memory_item_free(&item);
        return CBM_STORE_NOT_FOUND;
    }
    if (project && !item.scope_project) {
        /* scoped delete against an unscoped item: treat as out-of-scope. */
        cbm_store_memory_item_free(&item);
        return CBM_STORE_NOT_FOUND;
    }
    char *source_ids =
        (is_purge && item.source_event_ids) ? heap_strdup(item.source_event_ids) : NULL;
    cbm_store_memory_item_free(&item);

    if (cbm_store_begin(s) != CBM_STORE_OK) {
        free(source_ids);
        store_set_error_sqlite(s, "memory_delete_begin");
        return CBM_STORE_ERR;
    }

    int rc;
    if (is_soft) {
        sqlite3_stmt *st = NULL;
        const char *sql =
            "UPDATE memory_item SET deleted_at=?1,updated_at=?1 "
            "WHERE id=?2 AND deleted_at IS NULL AND (?3 IS NULL OR scope_project=?3);";
        if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
            cbm_store_rollback(s);
            free(source_ids);
            store_set_error_sqlite(s, "memory_soft_delete_prepare");
            return CBM_STORE_ERR;
        }
        sqlite3_bind_int64(st, 1, memory_now_ms());
        bind_text(st, 2, id);
        memory_bind_nullable(st, 3, project);
        rc = sqlite3_step(st);
        int changed = sqlite3_changes(s->db);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            cbm_store_rollback(s);
            free(source_ids);
            store_set_error_sqlite(s, "memory_soft_delete_step");
            return CBM_STORE_ERR;
        }
        if (changed <= 0) {
            /* Already soft-deleted (deleted_at not NULL): idempotent no-op. */
            cbm_store_rollback(s);
            free(source_ids);
            return CBM_STORE_NOT_FOUND;
        }
        if (memory_delete_audit(s, "soft_delete", mode, id, project, user) != CBM_STORE_OK) {
            cbm_store_rollback(s);
            free(source_ids);
            store_set_error_sqlite(s, "memory_soft_delete_audit");
            return CBM_STORE_ERR;
        }
    } else {
        rc = memory_delete_rows(s, id, project, source_ids);
        if (rc != CBM_STORE_OK) {
            cbm_store_rollback(s);
            free(source_ids);
            return CBM_STORE_ERR;
        }
        /* The tombstone audit event must outlive the erased content. */
        if (memory_delete_audit(s, "delete", mode, id, project, user) != CBM_STORE_OK) {
            cbm_store_rollback(s);
            free(source_ids);
            store_set_error_sqlite(s, "memory_delete_audit");
            return CBM_STORE_ERR;
        }
    }

    free(source_ids);
    return cbm_store_commit(s);
}

int cbm_store_memory_restore(cbm_store_t *s, const char *id, const char *project,
                             const char *user) {
    if (!s || !s->db || !id || !id[0]) {
        return CBM_STORE_ERR;
    }
    if (cbm_store_begin(s) != CBM_STORE_OK) {
        store_set_error_sqlite(s, "memory_restore_begin");
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *st = NULL;
    const char *sql =
        "UPDATE memory_item SET deleted_at=NULL,updated_at=?1 "
        "WHERE id=?2 AND deleted_at IS NOT NULL AND (?3 IS NULL OR scope_project=?3);";
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_restore_prepare");
        return CBM_STORE_ERR;
    }
    sqlite3_bind_int64(st, 1, memory_now_ms());
    bind_text(st, 2, id);
    memory_bind_nullable(st, 3, project);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(s->db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_restore_step");
        return CBM_STORE_ERR;
    }
    if (changed <= 0) {
        /* Not soft-deleted (or wrong scope): nothing to restore. */
        cbm_store_rollback(s);
        return CBM_STORE_NOT_FOUND;
    }
    if (memory_delete_audit(s, "restore", "restore", id, project, user) != CBM_STORE_OK) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_restore_audit");
        return CBM_STORE_ERR;
    }
    return cbm_store_commit(s);
}

int cbm_store_memory_purge_expired(cbm_store_t *s, const char *project, int64_t grace_ms,
                                   int *purged) {
    if (purged)
        *purged = 0;
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    if (grace_ms < 0)
        grace_ms = 0;
    int64_t cutoff = memory_now_ms() - grace_ms;

    /* Collect (id, source_event_ids) for every item soft-deleted before the
     * cutoff FIRST (finalize the read cursor), then delete in one batch txn —
     * mirrors the v3 migration's collect-then-act to avoid a cursor-vs-write
     * SQLITE_BUSY on commit. Expired soft-deletes are always full purges
     * (source events go too): the grace window was the user's undo chance.
     *
     * P4 RED LINE: a code-anchored decision/constraint/lesson ADR is NEVER
     * physically purged — even soft-deleted, even past grace. It stays as the
     * head of a supersede chain so a future branch-revival can still read the
     * original rejection rationale ([[memory-lifecycle-architecture]]). Such an
     * item simply remains soft-deleted (hidden from recall) rather than erased.
     * Unanchored notes and non-ADR kinds purge normally. */
    char **ids = NULL;
    char **srcs = NULL;
    int count = 0, cap = 0;
    sqlite3_stmt *sel = NULL;
    const char *sel_sql =
        "SELECT id, source_event_ids FROM memory_item m "
        "WHERE deleted_at IS NOT NULL AND deleted_at <= ?1 "
        "AND (?2 IS NULL OR scope_project=?2) "
        "AND NOT (kind IN ('decision','constraint','lesson') AND EXISTS("
        "  SELECT 1 FROM memory_edge e WHERE e.src_id=m.id AND e.type='about_code'));";
    if (sqlite3_prepare_v2(s->db, sel_sql, CBM_NOT_FOUND, &sel, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_purge_select");
        return CBM_STORE_ERR;
    }
    sqlite3_bind_int64(sel, 1, cutoff);
    memory_bind_nullable(sel, 2, project);
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *iid = (const char *)sqlite3_column_text(sel, 0);
        const char *src = (const char *)sqlite3_column_text(sel, 1);
        if (!iid)
            continue;
        if (count == cap) {
            int ncap = cap ? cap * 2 : 16;
            char **ni = realloc(ids, (size_t)ncap * sizeof(*ni));
            char **ns = realloc(srcs, (size_t)ncap * sizeof(*ns));
            if (!ni || !ns) {
                free(ni ? ni : ids);
                free(ns ? ns : srcs);
                ids = NULL;
                srcs = NULL;
                count = 0;
                break;
            }
            ids = ni;
            srcs = ns;
            cap = ncap;
        }
        ids[count] = heap_strdup(iid);
        srcs[count] = src ? heap_strdup(src) : NULL;
        count++;
    }
    sqlite3_finalize(sel);

    if (count == 0) {
        free(ids);
        free(srcs);
        return CBM_STORE_OK;
    }

    if (cbm_store_begin(s) != CBM_STORE_OK) {
        for (int i = 0; i < count; i++) {
            free(ids[i]);
            free(srcs[i]);
        }
        free(ids);
        free(srcs);
        store_set_error_sqlite(s, "memory_purge_begin");
        return CBM_STORE_ERR;
    }
    int ok = 1;
    int done = 0;
    for (int i = 0; i < count && ok; i++) {
        if (memory_delete_rows(s, ids[i], project, srcs[i]) != CBM_STORE_OK) {
            ok = 0;
            break;
        }
        if (memory_delete_audit(s, "delete", "sweep", ids[i], project, NULL) != CBM_STORE_OK) {
            ok = 0;
            break;
        }
        done++;
    }
    for (int i = 0; i < count; i++) {
        free(ids[i]);
        free(srcs[i]);
    }
    free(ids);
    free(srcs);
    if (!ok) {
        cbm_store_rollback(s);
        return CBM_STORE_ERR;
    }
    int crc = cbm_store_commit(s);
    if (crc == CBM_STORE_OK && purged) {
        *purged = done;
    }
    return crc;
}

static char *memory_summary_from_content(const char *content) {
    if (!content) {
        return heap_strdup("");
    }
    size_t len = strlen(content);
    size_t n = len > 240 ? 240 : len;
    char *out = malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, content, n);
    out[n] = '\0';
    return out;
}
static void memory_lower_token(const char *src, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0)
        return;
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c >= 0x80)
            dst[j++] = (char)c;
        else if (j > 0 && dst[j - 1] != '-')
            dst[j++] = '-';
    }
    while (j > 0 && dst[j - 1] == '-')
        j--;
    dst[j] = '\0';
}

static bool memory_contains_i(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0])
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i]) {
            unsigned char a = (unsigned char)p[i], b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b - 'A' + 'a');
            if (a != b)
                break;
            i++;
        }
        if (i == nlen)
            return true;
    }
    return false;
}

static const char *memory_infer_predicate(const char *kind, const char *content) {
    if (kind && strcmp(kind, "preference") == 0)
        return "prefers";
    if (kind && strcmp(kind, "constraint") == 0)
        return "forbids";
    if (kind && strcmp(kind, "decision") == 0)
        return "decides";
    if (memory_contains_i(content, "prefer"))
        return "prefers";
    if (memory_contains_i(content, "forbid"))
        return "forbids";
    if (memory_contains_i(content, "decision"))
        return "decides";
    if (memory_contains_i(content, "deprecated"))
        return "deprecates";
    return "mentions";
}

static void memory_infer_entity(const char *scope_project, const char *scope_user,
                                const char *content, char *out, size_t out_sz) {
    char token[CBM_SZ_256];
    const char *base = scope_project && scope_project[0]
                           ? scope_project
                           : (scope_user && scope_user[0] ? scope_user : "global");
    memory_lower_token(base, token, sizeof(token));
    const char *prefix = scope_project && scope_project[0]
                             ? "project"
                             : (scope_user && scope_user[0] ? "user" : "memory");
    if (memory_contains_i(content, "SQLite"))
        snprintf(out, out_sz, "tool:sqlite");
    else if (memory_contains_i(content, "MCP"))
        snprintf(out, out_sz, "tool:mcp");
    else if (memory_contains_i(content, "FTS5"))
        snprintf(out, out_sz, "tool:fts5");
    else if (memory_contains_i(content, "embedding"))
        snprintf(out, out_sz, "concept:embedding");
    else
        snprintf(out, out_sz, "%s:%s", prefix, token[0] ? token : "global");
}

static int memory_edge_insert(cbm_store_t *s, const char *src, const char *dst, const char *type,
                              const char *origin, double confidence) {
    if (!s || !s->db || !src || !dst || !type || !origin)
        return CBM_STORE_OK;
    sqlite3_stmt *check = NULL;
    const char *check_sql =
        "SELECT 1 FROM memory_edge WHERE src_id=?1 AND dst_id=?2 AND type=?3 LIMIT 1;";
    if (sqlite3_prepare_v2(s->db, check_sql, CBM_NOT_FOUND, &check, NULL) == SQLITE_OK) {
        bind_text(check, 1, src);
        bind_text(check, 2, dst);
        bind_text(check, 3, type);
        int exists = sqlite3_step(check) == SQLITE_ROW;
        sqlite3_finalize(check);
        if (exists)
            return CBM_STORE_OK;
    }
    char idbuf[CBM_SZ_128];
    memory_make_id(idbuf, sizeof(idbuf), "medge");
    const char *sql =
        "INSERT INTO memory_edge (id,src_id,dst_id,type,weight,origin,confidence,created_at) "
        "VALUES (?1,?2,?3,?4,1.0,?5,?6,?7);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    bind_text(stmt, 1, idbuf);
    bind_text(stmt, 2, src);
    bind_text(stmt, 3, dst);
    bind_text(stmt, 4, type);
    bind_text(stmt, 5, origin);
    sqlite3_bind_double(stmt, 6, confidence > 0.0 ? confidence : 0.5);
    sqlite3_bind_int64(stmt, 7, memory_now_ms());
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
}

/* Public: anchor a memory item to a code symbol via an about_code edge.
 * dst is "code:<qualified_name>" — a stable address that survives re-indexing
 * (node integer ids do not). Reuses memory_edge_insert (idempotent dedup). */
int cbm_store_memory_link_code(cbm_store_t *s, const char *item_id, const char *qualified_name,
                               const char *origin) {
    if (!s || !s->db || !item_id || !qualified_name || !qualified_name[0]) {
        return CBM_STORE_OK;
    }
    char dst[CBM_SZ_512];
    snprintf(dst, sizeof(dst), "code:%s", qualified_name);
    return memory_edge_insert(s, item_id, dst, "about_code", origin && origin[0] ? origin : "user",
                              1.0);
}

static void memory_first_source_id(const char *source_ids, char *out, size_t out_sz) {
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    const char *p = source_ids ? strchr(source_ids, '"') : NULL;
    if (!p)
        return;
    p++;
    const char *e = strchr(p, '"');
    if (!e || e <= p)
        return;
    size_t n = (size_t)(e - p);
    if (n >= out_sz)
        n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static int memory_vec_upsert(cbm_store_t *s, const char *item_id, const char *content) {
    if (!s || !s->db || !item_id)
        return CBM_STORE_OK;
    int8_t vec[MEMORY_VEC_DIM];
    memory_feature_vec(content, vec);
    const char *sql =
        "INSERT OR REPLACE INTO memory_vec (item_id,dim,embedding,embedding_json,updated_at) "
        "VALUES (?1,?2,?3,NULL,?4);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_OK;
    bind_text(stmt, 1, item_id);
    sqlite3_bind_int(stmt, 2, MEMORY_VEC_DIM);
    sqlite3_bind_blob(stmt, 3, vec, (int)sizeof(vec), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, memory_now_ms());
    (void)sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return CBM_STORE_OK;
}

int cbm_store_memory_consolidate(cbm_store_t *s, const char *project, int limit, int *processed) {
    if (processed)
        *processed = 0;
    if (!s || !s->db)
        return CBM_STORE_ERR;
    int lim = limit > 0 ? limit : 100;
    const char *sql = "SELECT "
                      "id,kind,content,scope_project,scope_user,scope_task,entity_key,predicate,"
                      "confidence,source_event_ids "
                      "FROM memory_item WHERE status='candidate' AND deleted_at IS NULL AND (?1 IS "
                      "NULL OR scope_project=?1) LIMIT ?2;";

    /* One transaction for the whole batch. Each consolidated item touches
     * memory_item + memory_edge + memory_vec + memory_fts; a crash mid-item
     * would otherwise leave an item flipped to status='active' but missing its
     * vector/FTS/edges. Acquiring the write lock with BEGIN IMMEDIATE *before*
     * opening the read cursor avoids a begin-while-cursor-open lock ordering
     * problem (the cursor then reads the same connection's pending writes). */
    if (cbm_store_begin(s) != CBM_STORE_OK) {
        store_set_error_sqlite(s, "memory_consolidate_begin");
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_consolidate_select");
        cbm_store_rollback(s);
        return CBM_STORE_ERR;
    }
    memory_bind_nullable(stmt, 1, project);
    sqlite3_bind_int(stmt, 2, lim);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *kind = (const char *)sqlite3_column_text(stmt, 1);
        const char *content = (const char *)sqlite3_column_text(stmt, 2);
        const char *scope_project = (const char *)sqlite3_column_text(stmt, 3);
        const char *scope_user = (const char *)sqlite3_column_text(stmt, 4);
        const char *scope_task = (const char *)sqlite3_column_text(stmt, 5);
        const char *existing_entity = (const char *)sqlite3_column_text(stmt, 6);
        const char *existing_predicate = (const char *)sqlite3_column_text(stmt, 7);
        double confidence = sqlite3_column_double(stmt, 8);
        const char *source_ids = (const char *)sqlite3_column_text(stmt, 9);
        char *summary = memory_summary_from_content(content);
        char entity[CBM_SZ_512];
        char source_event_id[CBM_SZ_128];
        const char *predicate = existing_predicate && existing_predicate[0]
                                    ? existing_predicate
                                    : memory_infer_predicate(kind, content);
        if (existing_entity && existing_entity[0])
            snprintf(entity, sizeof(entity), "%s", existing_entity);
        else
            memory_infer_entity(scope_project, scope_user, content, entity, sizeof(entity));
        memory_first_source_id(source_ids, source_event_id, sizeof(source_event_id));

        sqlite3_stmt *dup = NULL;
        const char *dup_sql =
            "SELECT id,content FROM memory_item WHERE status='active' AND deleted_at IS NULL AND "
            "entity_key=?1 AND predicate=?2 "
            "AND ((?3 IS NULL AND scope_project IS NULL) OR scope_project=?3) "
            "AND ((?4 IS NULL AND scope_user IS NULL) OR scope_user=?4) "
            "AND ((?5 IS NULL AND scope_task IS NULL) OR scope_task=?5) LIMIT 20;";
        char merge_buf[CBM_SZ_128] = {0};
        const char *merge_id = NULL;

        /* Build the candidate's 768-d embedding once (mean-pooled nomic vectors),
         * reuse for all comparisons. The candidate is already SQL-filtered to the
         * same (entity, predicate, scope) bucket. Within the bucket:
         *   cosine >= 0.90 → merge (same fact, same wording or near-identical paraphrase)
         *   cosine <  0.90 → COEXIST (handled at read time by scope-aware conflict
         *                    adjudication; NOT auto-marked as a contradiction)
         * P3-c (2026-06-30): the old rule auto-inserted a `contradicts` edge for
         * cosine<0.90 within an explicit-entity_key bucket. That was a GUESS, and a
         * wrong one — real nomic embeddings cluster by topic, so two differently-
         * worded but ALIGNED memories on the same subject score < 0.90 and were
         * falsely marked contradictory. A contradicts edge is high-impact (it
         * HIDES a memory from recall entirely, see memory_resolve_conflicts), so it
         * must come from strong evidence (an explicit assertion), never a cosine
         * heuristic. We keep the merge half (high cosine = same fact) and drop the
         * auto-contradiction half; non-equivalent same-key items simply coexist and
         * are ranked/adjudicated at read time. See [[memory-lifecycle-architecture]]. */
        int8_t cand_vec[MEMORY_VEC_DIM];
        memory_feature_vec(content, cand_vec);

        if (sqlite3_prepare_v2(s->db, dup_sql, CBM_NOT_FOUND, &dup, NULL) == SQLITE_OK) {
            bind_text(dup, 1, entity);
            bind_text(dup, 2, predicate);
            memory_bind_nullable(dup, 3, scope_project);
            memory_bind_nullable(dup, 4, scope_user);
            memory_bind_nullable(dup, 5, scope_task);
            while (sqlite3_step(dup) == SQLITE_ROW) {
                const char *other_id = (const char *)sqlite3_column_text(dup, 0);
                const char *other_content = (const char *)sqlite3_column_text(dup, 1);
                if (!other_id)
                    continue;
                /* Compute cosine similarity via the same int8 vectors used at retrieve time. */
                int8_t other_vec[MEMORY_VEC_DIM];
                memory_feature_vec(other_content, other_vec);
                int32_t dot = 0, mag_a = 0, mag_b = 0;
                for (int vi = 0; vi < MEMORY_VEC_DIM; vi++) {
                    dot += (int32_t)cand_vec[vi] * (int32_t)other_vec[vi];
                    mag_a += (int32_t)cand_vec[vi] * (int32_t)cand_vec[vi];
                    mag_b += (int32_t)other_vec[vi] * (int32_t)other_vec[vi];
                }
                double denom = sqrt((double)mag_a) * sqrt((double)mag_b);
                double cosine = denom > CBM_STORE_DENOM_EPS_D ? (double)dot / denom : 0.0;
                if (cosine >= 0.90) {
                    /* High similarity: same fact expressed equivalently → merge. */
                    snprintf(merge_buf, sizeof(merge_buf), "%s", other_id);
                    merge_id = merge_buf;
                    break;
                }
                /* cosine < 0.90: NOT auto-marked contradictory (P3-c). The item
                 * coexists; read-time scope-aware adjudication ranks among peers. */
            }
            sqlite3_finalize(dup);
        }

        if (merge_id) {
            sqlite3_stmt *m1 = NULL;
            /* Boost confidence of the surviving active item and record that it now supersedes
             * the incoming candidate (supersedes points TO the item being retired, not the
             * survivor). */
            if (sqlite3_prepare_v2(s->db,
                                   "UPDATE memory_item SET confidence=MIN(1.0, confidence + 0.05), "
                                   "supersedes=?1, updated_at=?2 WHERE id=?3;",
                                   CBM_NOT_FOUND, &m1, NULL) == SQLITE_OK) {
                bind_text(m1, 1, id); /* survivor.supersedes = candidate being archived */
                sqlite3_bind_int64(m1, 2, memory_now_ms());
                bind_text(m1, 3, merge_id);
                (void)sqlite3_step(m1);
                sqlite3_finalize(m1);
            }
            sqlite3_stmt *m2 = NULL;
            /* Archive the duplicate candidate. supersedes is left NULL: it is the retired item,
             * not the one doing the superseding. */
            if (sqlite3_prepare_v2(
                    s->db, "UPDATE memory_item SET status='archived', updated_at=?1 WHERE id=?2;",
                    CBM_NOT_FOUND, &m2, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(m2, 1, memory_now_ms());
                bind_text(m2, 2, id);
                (void)sqlite3_step(m2);
                sqlite3_finalize(m2);
            }
            (void)memory_edge_insert(s, id, merge_id, "similar_to", "post", confidence);
            if (source_event_id[0])
                (void)memory_edge_insert(s, merge_id, source_event_id, "derived_from", "merge",
                                         1.0);
        } else {
            sqlite3_stmt *up = NULL;
            const char *up_sql =
                "UPDATE memory_item SET kind=COALESCE(NULLIF(kind,''),'event'), "
                "layer=COALESCE(NULLIF(layer,''),'episodic'), summary=COALESCE(summary,?1), "
                "entity_key=COALESCE(entity_key,?2), predicate=COALESCE(predicate,?3), "
                "status='active', updated_at=?4 WHERE id=?5;";
            if (sqlite3_prepare_v2(s->db, up_sql, CBM_NOT_FOUND, &up, NULL) == SQLITE_OK) {
                memory_bind_nullable(up, 1, summary);
                bind_text(up, 2, entity);
                bind_text(up, 3, predicate);
                sqlite3_bind_int64(up, 4, memory_now_ms());
                bind_text(up, 5, id);
                (void)sqlite3_step(up);
                sqlite3_finalize(up);
            }
            if (source_event_id[0])
                (void)memory_edge_insert(s, id, source_event_id, "derived_from", "rule", 1.0);
            if (scope_project && scope_project[0])
                (void)memory_edge_insert(s, id, scope_project, "belongs_to", "rule", 1.0);
            (void)memory_vec_upsert(s, id, content);
            /* P1: build FTS index now that the item is active (deferred from hot path). */
            {
                cbm_memory_item_t fts_item = {0};
                fts_item.title =
                    id; /* use id as title placeholder if title not in consolidate select */
                fts_item.summary = summary;
                fts_item.content = content;
                (void)memory_fts_upsert(s, &fts_item, id);
            }
        }
        free(summary);
        n++;
    }
    sqlite3_finalize(stmt);
    /* P0-4: one summary audit event per batch (per-merge events would be noise).
     * Written inside the open transaction so it commits atomically with the run. */
    if (n > 0) {
        char count_buf[32];
        snprintf(count_buf, sizeof(count_buf), "%d", n);
        (void)memory_delete_audit(s, "consolidate", count_buf, "", project, NULL);
    }
    if (cbm_store_commit(s) != CBM_STORE_OK) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_consolidate_commit");
        return CBM_STORE_ERR;
    }
    if (processed)
        *processed = n;
    return CBM_STORE_OK;
}

int cbm_store_memory_reindex_fts(cbm_store_t *s, const char *project, int *processed) {
    if (processed)
        *processed = 0;
    if (!s || !s->db)
        return CBM_STORE_ERR;
    /* Drop existing FTS rows for the project's items, then re-insert with current
     * segmentation. Scoped by project so reindex of one project leaves others alone. */
    const char *del_sql = "DELETE FROM memory_fts WHERE item_id IN "
                          "(SELECT id FROM memory_item WHERE ?1 IS NULL OR scope_project=?1);";
    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(s->db, del_sql, CBM_NOT_FOUND, &del, NULL) == SQLITE_OK) {
        memory_bind_nullable(del, 1, project);
        (void)sqlite3_step(del);
        sqlite3_finalize(del);
    }
    const char *sel_sql = "SELECT id,title,summary,content FROM memory_item "
                          "WHERE (?1 IS NULL OR scope_project=?1) AND deleted_at IS NULL AND "
                          "status IN ('active','candidate','deprecated');";
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(s->db, sel_sql, CBM_NOT_FOUND, &sel, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_reindex_select");
        return CBM_STORE_ERR;
    }
    memory_bind_nullable(sel, 1, project);
    int n = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        cbm_memory_item_t item = {0};
        item.title = (const char *)sqlite3_column_text(sel, 1);
        item.summary = (const char *)sqlite3_column_text(sel, 2);
        item.content = (const char *)sqlite3_column_text(sel, 3);
        const char *id = (const char *)sqlite3_column_text(sel, 0);
        if (id && memory_fts_upsert(s, &item, id) == CBM_STORE_OK)
            n++;
    }
    sqlite3_finalize(sel);
    if (processed)
        *processed = n;
    return CBM_STORE_OK;
}

int cbm_store_memory_decay(cbm_store_t *s, const char *project, int limit, int *processed) {
    if (processed)
        *processed = 0;
    if (!s || !s->db)
        return CBM_STORE_ERR;
    int lim = limit > 0 ? limit : 100;
    int64_t now = memory_now_ms();
    const char *sql =
        "SELECT id,last_hit_at,confidence,reusability,importance,decay FROM memory_item WHERE status='active' "
        "AND deleted_at IS NULL AND (?1 IS NULL OR scope_project=?1) LIMIT ?2;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    /* P0-4: the per-item decay UPDATEs and the summary audit event commit
     * together — collect the rows first (the read cursor is finalized before
     * COMMIT to avoid a cursor-vs-write SQLITE_BUSY). */
    if (cbm_store_begin(s) != CBM_STORE_OK) {
        sqlite3_finalize(stmt);
        store_set_error_sqlite(s, "memory_decay_begin");
        return CBM_STORE_ERR;
    }
    memory_bind_nullable(stmt, 1, project);
    sqlite3_bind_int(stmt, 2, lim);
    int n = 0;
    int archived = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        int64_t last_hit = sqlite3_column_int64(stmt, 1);
        double confidence = sqlite3_column_double(stmt, 2);
        double reusability = sqlite3_column_double(stmt, 3);
        double importance = sqlite3_column_double(stmt, 4);
        double old_decay = sqlite3_column_double(stmt, 5);
        int64_t age_ms = last_hit > 0 ? now - last_hit : 30LL * 24LL * 60LL * 60LL * 1000LL;
        if (age_ms < 0)
            age_ms = 0;
        double age_days = (double)age_ms / (1000.0 * 60.0 * 60.0 * 24.0);
        double next_decay =
            old_decay + (age_days / 30.0) * (1.0 - confidence) * (1.0 - reusability) * (1.0 - importance);
        const char *status = next_decay >= 1.0 ? "archived" : "active";
        sqlite3_stmt *up = NULL;
        if (sqlite3_prepare_v2(
                s->db, "UPDATE memory_item SET decay=?1,status=?2,updated_at=?3 WHERE id=?4;",
                CBM_NOT_FOUND, &up, NULL) == SQLITE_OK) {
            sqlite3_bind_double(up, 1, next_decay);
            bind_text(up, 2, status);
            sqlite3_bind_int64(up, 3, now);
            bind_text(up, 4, id);
            (void)sqlite3_step(up);
            sqlite3_finalize(up);
            n++;
            if (next_decay >= 1.0)
                archived++;
        }
    }
    sqlite3_finalize(stmt);
    /* One summary audit event per pass when anything was archived. */
    if (archived > 0) {
        char count_buf[32];
        snprintf(count_buf, sizeof(count_buf), "%d", archived);
        (void)memory_delete_audit(s, "decay", count_buf, "", project, NULL);
    }
    if (cbm_store_commit(s) != CBM_STORE_OK) {
        cbm_store_rollback(s);
        store_set_error_sqlite(s, "memory_decay_commit");
        return CBM_STORE_ERR;
    }
    if (processed)
        *processed = n;
    return CBM_STORE_OK;
}

/* Lazy auto-maintenance gate. Thresholds are overridable via env for testing
 * and benchmarking; defaults suit a single-user agent. */
int cbm_store_memory_maintain_if_due(cbm_store_t *s, const char *project,
                                     cbm_memory_maintain_report_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!s || !s->db) {
        return CBM_STORE_OK; /* never fail the caller */
    }

    /* Global off-switch (set CBM_MEMORY_AUTO_MAINTAIN=0 to disable). */
    char envbuf[8];
    cbm_safe_getenv("CBM_MEMORY_AUTO_MAINTAIN", envbuf, sizeof(envbuf), NULL);
    if (envbuf[0] == '0') {
        return CBM_STORE_OK;
    }

    enum {
        CONSOLIDATE_THRESHOLD = 8,          /* candidates needed to trigger early */
        MIN_INTERVAL_MS = 60 * 1000,        /* debounce: don't consolidate more often */
        MAX_INTERVAL_MS = 5 * 60 * 1000,    /* backstop: any pending candidate eventually */
        DECAY_INTERVAL_MS = 60 * 60 * 1000, /* decay is time-driven only */
        SWEEP_INTERVAL_MS = 60 * 60 * 1000  /* retention sweep cadence (time-driven) */
    };
    /* Grace window before a soft-deleted item is physically purged. Default 7
     * days; override with CBM_MEMORY_DELETE_GRACE_MS (e.g. 0 in tests). */
    int64_t grace_ms = 7LL * 24 * 60 * 60 * 1000;
    {
        char gbuf[32];
        cbm_safe_getenv("CBM_MEMORY_DELETE_GRACE_MS", gbuf, sizeof(gbuf), NULL);
        if (gbuf[0]) {
            long long g = atoll(gbuf);
            if (g >= 0)
                grace_ms = (int64_t)g;
        }
    }

    int64_t now = memory_now_ms();

    /* Count pending candidates (cheap: idx_memory_item_status covers status). */
    int candidates = 0;
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(s->db,
                               "SELECT COUNT(*) FROM memory_item WHERE status='candidate' "
                               "AND (?1 IS NULL OR scope_project=?1);",
                               CBM_NOT_FOUND, &st, NULL) == SQLITE_OK) {
            memory_bind_nullable(st, 1, project);
            if (sqlite3_step(st) == SQLITE_ROW) {
                candidates = sqlite3_column_int(st, 0);
            }
        }
        sqlite3_finalize(st);
    }

    /* Consolidate when: enough candidates AND past the debounce window, OR any
     * candidate has been waiting past the backstop window. */
    if (candidates > 0) {
        int64_t last = memory_meta_get_i64(s, "last_consolidate_ms", 0);
        int64_t since = now - last;
        bool due = (candidates >= CONSOLIDATE_THRESHOLD && since >= MIN_INTERVAL_MS) ||
                   (since >= MAX_INTERVAL_MS);
        if (due) {
            int processed = 0;
            if (cbm_store_memory_consolidate(s, project, 0, &processed) == CBM_STORE_OK) {
                memory_meta_set_i64(s, "last_consolidate_ms", now);
                if (out) {
                    out->consolidated = true;
                    out->consolidate_count = processed;
                }
            }
        }
    }

    /* Decay on a fixed time cadence. */
    {
        int64_t last = memory_meta_get_i64(s, "last_decay_ms", 0);
        if (now - last >= DECAY_INTERVAL_MS) {
            int processed = 0;
            if (cbm_store_memory_decay(s, project, 0, &processed) == CBM_STORE_OK) {
                memory_meta_set_i64(s, "last_decay_ms", now);
                if (out) {
                    out->decayed = true;
                    out->decay_count = processed;
                }
            }
        }
    }

    /* Retention sweep on a fixed time cadence: physically purge soft-deletes
     * past the grace window. Best-effort — a failure here never fails the
     * caller, and we only stamp the cadence marker on success. */
    {
        int64_t last = memory_meta_get_i64(s, "last_delete_sweep_ms", 0);
        if (now - last >= SWEEP_INTERVAL_MS) {
            int purged = 0;
            if (cbm_store_memory_purge_expired(s, project, grace_ms, &purged) == CBM_STORE_OK) {
                memory_meta_set_i64(s, "last_delete_sweep_ms", now);
                if (out) {
                    out->swept = true;
                    out->sweep_count = purged;
                }
            }
        }
    }

    return CBM_STORE_OK;
}

/* P1: ADR list — structured query for decision/constraint-class memories. */
#define ADR_LIST_COLUMNS                                                                    \
    "id,kind,layer,COALESCE(title,summary) AS title,summary,entity_key,status,"             \
    "importance,confidence,reusability,specificity,hit_count,decay,version,"                 \
    "supersedes,created_at,updated_at"
#define ADR_LIST_COL_COUNT 17

int cbm_store_memory_adr_list(cbm_store_t *s, const char *project, const char *kind_filter,
                              const char *status_filter, const char *entity_key_filter, int limit,
                              char **out_json) {
    if (!s || !s->db || !project || !out_json) {
        return CBM_STORE_ERR;
    }
    if (limit <= 0 || limit > 200) {
        limit = 50;
    }

    /* Build the SQL with fixed string concatenation — avoids dynstr_t
     * dependency (that type lives in the MCP/cli layer). */
    char sql[2048];
    int pos = 0;
    pos += snprintf(sql + pos, sizeof(sql) - pos,
                    "SELECT " ADR_LIST_COLUMNS " FROM memory_item "
                    "WHERE scope_project=?1 AND deleted_at IS NULL");
    int param_idx = 2;
    if (kind_filter && kind_filter[0]) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND kind=?%d", param_idx++);
    } else {
        pos += snprintf(sql + pos, sizeof(sql) - pos,
                        " AND kind IN ('decision','constraint')");
    }
    if (status_filter && status_filter[0]) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND status=?%d", param_idx++);
    }
    if (entity_key_filter && entity_key_filter[0]) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND entity_key=?%d", param_idx++);
    }
    pos += snprintf(sql + pos, sizeof(sql) - pos,
                    " ORDER BY (importance+confidence+reusability+specificity+hit_count-decay) "
                    "DESC, updated_at DESC LIMIT ?%d;",
                    param_idx);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "adr_list_prepare");
        return CBM_STORE_ERR;
    }

    bind_text(stmt, 1, project);
    int bind_idx = 2;
    if (kind_filter && kind_filter[0]) {
        bind_text(stmt, bind_idx++, kind_filter);
    }
    if (status_filter && status_filter[0]) {
        bind_text(stmt, bind_idx++, status_filter);
    }
    if (entity_key_filter && entity_key_filter[0]) {
        bind_text(stmt, bind_idx++, entity_key_filter);
    }
    sqlite3_bind_int(stmt, bind_idx, limit);

    /* Collect rows into a JSON array. */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "items", items);

    int total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, obj, "id", (const char *)sqlite3_column_text(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc, obj, "kind", (const char *)sqlite3_column_text(stmt, 1));
        yyjson_mut_obj_add_strcpy(doc, obj, "layer", (const char *)sqlite3_column_text(stmt, 2));
        yyjson_mut_obj_add_strcpy(doc, obj, "title",
                                   (const char *)sqlite3_column_text(stmt, 3));
        yyjson_mut_obj_add_strcpy(doc, obj, "summary",
                                   (const char *)sqlite3_column_text(stmt, 4));
        yyjson_mut_obj_add_strcpy(doc, obj, "entity_key",
                                   (const char *)sqlite3_column_text(stmt, 5));
        yyjson_mut_obj_add_strcpy(doc, obj, "status",
                                   (const char *)sqlite3_column_text(stmt, 6));
        yyjson_mut_obj_add_real(doc, obj, "importance", sqlite3_column_double(stmt, 7));
        yyjson_mut_obj_add_real(doc, obj, "confidence", sqlite3_column_double(stmt, 8));
        yyjson_mut_obj_add_real(doc, obj, "reusability", sqlite3_column_double(stmt, 9));
        yyjson_mut_obj_add_real(doc, obj, "specificity", sqlite3_column_double(stmt, 10));
        yyjson_mut_obj_add_int(doc, obj, "hit_count", sqlite3_column_int(stmt, 11));
        yyjson_mut_obj_add_real(doc, obj, "decay", sqlite3_column_double(stmt, 12));
        yyjson_mut_obj_add_int(doc, obj, "version", sqlite3_column_int(stmt, 13));
        {
            const char *sup = (const char *)sqlite3_column_text(stmt, 14);
            if (sup && sup[0]) {
                yyjson_mut_obj_add_strcpy(doc, obj, "supersedes", sup);
            }
        }
        yyjson_mut_obj_add_int(doc, obj, "created_at", (int64_t)sqlite3_column_int64(stmt, 15));
        yyjson_mut_obj_add_int(doc, obj, "updated_at", (int64_t)sqlite3_column_int64(stmt, 16));
        yyjson_mut_arr_append(items, obj);
        total++;
    }
    sqlite3_finalize(stmt);

    yyjson_mut_obj_add_int(doc, root, "total", total);
    {
        size_t len = 0;
        char *s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
        *out_json = heap_strdup(s ? s : "{}");
        free(s);
    }
    yyjson_mut_doc_free(doc);
    return CBM_STORE_OK;
}

/* adr_list variant for the global (cross-project) store:
 * scope_project IS NULL instead of matching a project name.
 * Otherwise identical to cbm_store_memory_adr_list. */
int cbm_store_memory_adr_list_global(cbm_store_t *s, const char *kind_filter,
                                     const char *status_filter,
                                     const char *entity_key_filter,
                                     int limit, char **out_json) {
    if (!s || !s->db || !out_json) {
        return CBM_STORE_ERR;
    }
    if (limit <= 0 || limit > 200) {
        limit = 50;
    }

    char sql[2048];
    int pos = 0;
    pos += snprintf(sql + pos, sizeof(sql) - pos,
                    "SELECT " ADR_LIST_COLUMNS " FROM memory_item "
                    "WHERE scope_project IS NULL AND deleted_at IS NULL");
    int param_idx = 1;
    if (kind_filter && kind_filter[0]) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND kind=?%d", param_idx++);
    } else {
        pos += snprintf(sql + pos, sizeof(sql) - pos,
                        " AND kind IN ('decision','constraint')");
    }
    if (status_filter && status_filter[0]) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND status=?%d", param_idx++);
    }
    if (entity_key_filter && entity_key_filter[0]) {
        pos += snprintf(sql + pos, sizeof(sql) - pos, " AND entity_key=?%d", param_idx++);
    }
    pos += snprintf(sql + pos, sizeof(sql) - pos,
                    " ORDER BY (importance+confidence+reusability+specificity+hit_count-decay) "
                    "DESC, updated_at DESC LIMIT ?%d;",
                    param_idx);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "adr_list_global_prepare");
        return CBM_STORE_ERR;
    }

    int bind_idx = 1;
    if (kind_filter && kind_filter[0]) {
        bind_text(stmt, bind_idx++, kind_filter);
    }
    if (status_filter && status_filter[0]) {
        bind_text(stmt, bind_idx++, status_filter);
    }
    if (entity_key_filter && entity_key_filter[0]) {
        bind_text(stmt, bind_idx++, entity_key_filter);
    }
    sqlite3_bind_int(stmt, bind_idx, limit);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", CBM_GLOBAL_MEMORY_PROJECT);
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "items", items);

    int total = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, obj, "id", (const char *)sqlite3_column_text(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc, obj, "kind", (const char *)sqlite3_column_text(stmt, 1));
        yyjson_mut_obj_add_strcpy(doc, obj, "layer", (const char *)sqlite3_column_text(stmt, 2));
        yyjson_mut_obj_add_strcpy(doc, obj, "title",
                                   (const char *)sqlite3_column_text(stmt, 3));
        yyjson_mut_obj_add_strcpy(doc, obj, "summary",
                                   (const char *)sqlite3_column_text(stmt, 4));
        yyjson_mut_obj_add_strcpy(doc, obj, "entity_key",
                                   (const char *)sqlite3_column_text(stmt, 5));
        yyjson_mut_obj_add_strcpy(doc, obj, "status",
                                   (const char *)sqlite3_column_text(stmt, 6));
        yyjson_mut_obj_add_real(doc, obj, "importance", sqlite3_column_double(stmt, 7));
        yyjson_mut_obj_add_real(doc, obj, "confidence", sqlite3_column_double(stmt, 8));
        yyjson_mut_obj_add_real(doc, obj, "reusability", sqlite3_column_double(stmt, 9));
        yyjson_mut_obj_add_real(doc, obj, "specificity", sqlite3_column_double(stmt, 10));
        yyjson_mut_obj_add_int(doc, obj, "hit_count", sqlite3_column_int(stmt, 11));
        yyjson_mut_obj_add_real(doc, obj, "decay", sqlite3_column_double(stmt, 12));
        yyjson_mut_obj_add_int(doc, obj, "version", sqlite3_column_int(stmt, 13));
        {
            const char *sup = (const char *)sqlite3_column_text(stmt, 14);
            if (sup && sup[0]) {
                yyjson_mut_obj_add_strcpy(doc, obj, "supersedes", sup);
            }
        }
        yyjson_mut_obj_add_int(doc, obj, "created_at", (int64_t)sqlite3_column_int64(stmt, 15));
        yyjson_mut_obj_add_int(doc, obj, "updated_at", (int64_t)sqlite3_column_int64(stmt, 16));
        yyjson_mut_arr_append(items, obj);
        total++;
    }
    sqlite3_finalize(stmt);

    yyjson_mut_obj_add_int(doc, root, "total", total);
    {
        size_t len = 0;
        char *s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
        *out_json = heap_strdup(s ? s : "{}");
        free(s);
    }
    yyjson_mut_doc_free(doc);
    return CBM_STORE_OK;
}

/* ── adr_chain: walk the supersedes chain for an ADR ────────────────
 *
 * Operates in two modes:
 *   - item_id given: look up that item, walk backward to find the root
 *     (version=1), then walk forward from root to newest.
 *   - entity_key given: find root directly (version=1 + oldest created_at),
 *     then walk forward.
 *
 * Forward walk query: SELECT * FROM memory_item WHERE supersedes=?1
 *   AND scope_project=?2 AND deleted_at IS NULL.
 *
 * Cycle detection: visited-id linear array (max_depth ≤ 50).
 * Warning flags: multiple_roots (entity_key mode), cycle_detected,
 *   truncated (max_depth reached), orphan_link (supersedes pointer dangling). */

#define ADR_CHAIN_COLS \
    "id,kind,layer,COALESCE(title,summary) AS title,summary,entity_key," \
    "status,version,supersedes,created_at,updated_at"
#define ADR_CHAIN_COL_COUNT 11

int cbm_store_memory_adr_chain(cbm_store_t *s, const char *project,
                               const char *entity_key, const char *item_id,
                               int max_depth, char **out_json) {
    if (!s || !s->db || !project || !out_json) return CBM_STORE_ERR;
    if (!entity_key && !item_id) return CBM_STORE_ERR;
    if (max_depth <= 0 || max_depth > 200) max_depth = 50;

    /* Track warnings during traversal. */
    char warnings[3][256];
    int nwarns = 0;

    /* Resolved entity key (may come from item lookup). */
    const char *ekey = entity_key;
    char *resolved_ekey = NULL;

    /* ── Phase 1: find the root ──────────────────────────────────── */
    char root_id[128];
    root_id[0] = '\0';

    if (item_id && item_id[0]) {
        /* Walk backward from item_id following supersedes pointers. */
        const char *cursor = item_id;
        const char *visited[64];
        int nvis = 0;
        for (int hop = 0; hop < max_depth; hop++) {
            /* Cycle guard. */
            for (int v = 0; v < nvis; v++) {
                if (visited[v] && strcmp(visited[v], cursor) == 0) {
                    snprintf(warnings[nwarns++], sizeof(warnings[0]),
                             "cycle detected: %s already visited", cursor);
                    goto chain_forward;
                }
            }
            if (nvis < 64) visited[nvis++] = cursor;

            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(s->db,
                    "SELECT " ADR_CHAIN_COLS ",supersedes FROM memory_item "
                    "WHERE id=?1 AND scope_project=?2 AND deleted_at IS NULL;",
                    -1, &st, NULL) != SQLITE_OK) {
                break;
            }
            bind_text(st, 1, cursor);
            bind_text(st, 2, project);
            if (sqlite3_step(st) != SQLITE_ROW) {
                sqlite3_finalize(st);
                break; /* Dangling supersedes — stop here as root. */
            }
            /* Capture entity_key from the first backward stop if not provided. */
            if (!ekey && !resolved_ekey) {
                const char *ek = (const char *)sqlite3_column_text(st, 6);
                if (ek && ek[0]) resolved_ekey = heap_strdup(ek);
                ekey = resolved_ekey;
            }
            /* Record current as potential root. */
            const char *rid = (const char *)sqlite3_column_text(st, 0);
            if (rid) snprintf(root_id, sizeof(root_id), "%s", rid);
            /* The supersedes pointer tells us where to go next (backward). */
            const char *sup = (const char *)sqlite3_column_text(st, 8);
            sqlite3_finalize(st);

            if (!sup || !sup[0]) {
                break; /* Reached root (no supersedes on this item). */
            }
            cursor = sup;
        }
    } else if (ekey && ekey[0]) {
        /* Find root by entity_key: version=1, oldest created_at. */
        sqlite3_stmt *rs = NULL;
        if (sqlite3_prepare_v2(s->db,
                "SELECT id FROM memory_item "
                "WHERE entity_key=?1 AND scope_project=?2 AND version=1 "
                "AND deleted_at IS NULL ORDER BY created_at ASC LIMIT 1;",
                -1, &rs, NULL) == SQLITE_OK) {
            bind_text(rs, 1, ekey);
            bind_text(rs, 2, project);
            if (sqlite3_step(rs) == SQLITE_ROW) {
                const char *rid = (const char *)sqlite3_column_text(rs, 0);
                if (rid) snprintf(root_id, sizeof(root_id), "%s", rid);
            }
            sqlite3_finalize(rs);
        }
        /* Check for multiple roots. */
        sqlite3_stmt *mc = NULL;
        if (sqlite3_prepare_v2(s->db,
                "SELECT COUNT(*) FROM memory_item "
                "WHERE entity_key=?1 AND scope_project=?2 AND version=1 "
                "AND deleted_at IS NULL;",
                -1, &mc, NULL) == SQLITE_OK) {
            bind_text(mc, 1, ekey);
            bind_text(mc, 2, project);
            if (sqlite3_step(mc) == SQLITE_ROW) {
                int cnt = sqlite3_column_int(mc, 0);
                if (cnt > 1) {
                    snprintf(warnings[nwarns++], sizeof(warnings[0]),
                             "multiple_roots: %d version=1 items for entity_key "
                             "%s, using oldest by created_at", cnt, ekey);
                }
            }
            sqlite3_finalize(mc);
        }
    }

chain_forward:
    /* ── Phase 2: walk forward from root ──────────────────────────── */
    if (!root_id[0]) {
        free(resolved_ekey);
        /* No root found — return empty chain. */
        *out_json = heap_strdup("{\"project\":\"\",\"entity_key\":\"\",\"total_items\":0,\"items\":[]}");
        return *out_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    if (!ekey) ekey = "";

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "entity_key", ekey);
    if (item_id && item_id[0]) {
        yyjson_mut_obj_add_str(doc, root, "start_item_id", item_id);
    }
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "items", items);

    int generation = 0;
    const char *cursor = root_id;
    const char *vseen[64];
    int nvseen = 0;
    int hop = 0;

    for (hop = 0; hop < max_depth; hop++) {
        /* Cycle guard. */
        int cycle = 0;
        for (int v = 0; v < nvseen; v++) {
            if (vseen[v] && strcmp(vseen[v], cursor) == 0) {
                snprintf(warnings[nwarns++], sizeof(warnings[0]),
                         "cycle detected at generation %d: %s revisited", generation, cursor);
                cycle = 1;
                break;
            }
        }
        if (cycle) break;
        if (nvseen < 64) vseen[nvseen++] = cursor;

        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(s->db,
                "SELECT " ADR_CHAIN_COLS " FROM memory_item "
                "WHERE id=?1 AND scope_project=?2 AND deleted_at IS NULL;",
                -1, &st, NULL) != SQLITE_OK) {
            break;
        }
        bind_text(st, 1, cursor);
        bind_text(st, 2, project);
        if (sqlite3_step(st) != SQLITE_ROW) {
            sqlite3_finalize(st);
            break;
        }

        generation++;
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, obj, "id",
            (const char *)sqlite3_column_text(st, 0));
        yyjson_mut_obj_add_strcpy(doc, obj, "kind",
            (const char *)sqlite3_column_text(st, 1));
        yyjson_mut_obj_add_strcpy(doc, obj, "layer",
            (const char *)sqlite3_column_text(st, 2));
        yyjson_mut_obj_add_strcpy(doc, obj, "title",
            (const char *)sqlite3_column_text(st, 3));
        yyjson_mut_obj_add_strcpy(doc, obj, "summary",
            (const char *)sqlite3_column_text(st, 4));
        yyjson_mut_obj_add_strcpy(doc, obj, "entity_key",
            (const char *)sqlite3_column_text(st, 5));
        yyjson_mut_obj_add_strcpy(doc, obj, "status",
            (const char *)sqlite3_column_text(st, 6));
        yyjson_mut_obj_add_int(doc, obj, "version",
            sqlite3_column_int(st, 7));
        {
            const char *sup = (const char *)sqlite3_column_text(st, 8);
            if (sup && sup[0])
                yyjson_mut_obj_add_strcpy(doc, obj, "supersedes", sup);
        }
        yyjson_mut_obj_add_int(doc, obj, "created_at",
            (int64_t)sqlite3_column_int64(st, 9));
        yyjson_mut_obj_add_int(doc, obj, "updated_at",
            (int64_t)sqlite3_column_int64(st, 10));
        yyjson_mut_obj_add_int(doc, obj, "generation", generation);
        yyjson_mut_arr_add_val(items, obj);

        /* Find the next item in chain (supersedes → cursor). */
        const char *next_id = NULL;
        sqlite3_finalize(st);

        sqlite3_stmt *ns = NULL;
        if (sqlite3_prepare_v2(s->db,
                "SELECT id FROM memory_item "
                "WHERE supersedes=?1 AND scope_project=?2 AND deleted_at IS NULL LIMIT 1;",
                -1, &ns, NULL) == SQLITE_OK) {
            bind_text(ns, 1, cursor);
            bind_text(ns, 2, project);
            if (sqlite3_step(ns) == SQLITE_ROW) {
                next_id = (const char *)sqlite3_column_text(ns, 0);
            }
            sqlite3_finalize(ns);
        }
        if (!next_id || !next_id[0]) {
            break; /* End of chain. */
        }
        cursor = next_id;
    }

    if (hop >= max_depth) {
        snprintf(warnings[nwarns++], sizeof(warnings[0]),
                 "truncated: chain exceeded max_depth=%d", max_depth);
    }

    yyjson_mut_obj_add_int(doc, root, "total_items", generation);

    if (nwarns > 0) {
        yyjson_mut_val *warr = yyjson_mut_arr(doc);
        for (int w = 0; w < nwarns; w++)
            yyjson_mut_arr_add_strcpy(doc, warr, warnings[w]);
        yyjson_mut_obj_add_val(doc, root, "warnings", warr);
    }

    {
        size_t len = 0;
        char *s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
        *out_json = heap_strdup(s ? s : "{}");
        free(s);
    }
    yyjson_mut_doc_free(doc);
    free(resolved_ekey);
    return CBM_STORE_OK;
}
#undef ADR_CHAIN_COLS
#undef ADR_CHAIN_COL_COUNT

int cbm_store_memory_health(cbm_store_t *s, const char *project, cbm_memory_health_t *out) {
    memset(out, 0, sizeof(*out));
    if (!s || !s->db) {
        return CBM_STORE_ERR;
    }
    const char *sql =
        "SELECT "
        "(SELECT COUNT(*) FROM memory_event WHERE ?1 IS NULL OR project=?1),"
        "(SELECT COUNT(*) FROM memory_item WHERE ?1 IS NULL OR scope_project=?1),"
        "(SELECT COUNT(*) FROM memory_edge),"
        "(SELECT COUNT(*) FROM memory_item WHERE status='candidate' AND deleted_at IS NULL AND (?1 "
        "IS NULL OR scope_project=?1)),"
        "(SELECT COUNT(*) FROM memory_item WHERE status='active' AND deleted_at IS NULL AND (?1 IS "
        "NULL OR scope_project=?1)),"
        "(SELECT COUNT(*) FROM memory_item WHERE status='deprecated' AND deleted_at IS NULL AND "
        "(?1 IS NULL OR scope_project=?1)),"
        "(SELECT COUNT(*) FROM memory_item WHERE status='archived' AND deleted_at IS NULL AND (?1 "
        "IS NULL OR scope_project=?1)),"
        "(SELECT COUNT(*) FROM memory_item WHERE status='retracted' AND deleted_at IS NULL AND (?1 "
        "IS NULL OR scope_project=?1)),"
        "(SELECT COALESCE(SUM(hit_count),0) FROM memory_item WHERE ?1 IS NULL OR scope_project=?1),"
        "(SELECT COUNT(*) FROM memory_edge WHERE type='contradicts'),"
        "(SELECT COUNT(DISTINCT COALESCE(scope_project, scope_user, scope_task, 'global')) FROM "
        "memory_item WHERE ?1 IS NULL OR scope_project=?1),"
        "(SELECT COUNT(*) FROM memory_item WHERE deleted_at IS NOT NULL AND (?1 IS NULL OR "
        "scope_project=?1));";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        store_set_error_sqlite(s, "memory_health_prepare");
        return CBM_STORE_ERR;
    }
    memory_bind_nullable(stmt, 1, project);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->event_count = sqlite3_column_int(stmt, 0);
        out->item_count = sqlite3_column_int(stmt, 1);
        out->edge_count = sqlite3_column_int(stmt, 2);
        out->candidate_count = sqlite3_column_int(stmt, 3);
        out->active_count = sqlite3_column_int(stmt, 4);
        out->deprecated_count = sqlite3_column_int(stmt, 5);
        out->archived_count = sqlite3_column_int(stmt, 6);
        out->retracted_count = sqlite3_column_int(stmt, 7);
        out->total_hits = sqlite3_column_int64(stmt, 8);
        out->conflict_count = sqlite3_column_int(stmt, 9);
        out->scope_count = sqlite3_column_int(stmt, 10);
        out->deleted_count = sqlite3_column_int(stmt, 11);
        out->hit_rate =
            out->item_count > 0 ? (double)out->total_hits / (double)out->item_count : 0.0;
    }
    sqlite3_finalize(stmt);
    return CBM_STORE_OK;
}
