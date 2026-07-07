/*
 * memory_store.h — Local-fork memory/ADR extensions for the semantic-memory-mcp
 * project. These types and functions were originally embedded in store/store.h
 * alongside the code-graph layer. They are extracted here so memory-layer code
 * can evolve independently while the upstream store.h stays aligned with the
 * graph-only reference implementation.
 *
 * All memory data lives in a separate SQLite file (<cache>/<project>-memory.db)
 * so rebuilding the code graph never destroys memory.
 */

#ifndef CBM_MEMORY_STORE_H
#define CBM_MEMORY_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Forward declarations ──────────────────────────────────────────── */

typedef struct cbm_store cbm_store_t;
struct sqlite3;

/* ── Memory event ──────────────────────────────────────────────────── */

typedef struct {
    const char *id;
    const char *type;
    const char *source;
    int64_t timestamp_ms;
    const char *project;
    const char *user;
    const char *payload;
    double confidence;
    const char *context_json;
} cbm_memory_event_t;

/* ── Memory item ───────────────────────────────────────────────────── */

typedef struct {
    const char *id;
    const char *kind;
    const char *layer;
    const char *title;
    const char *summary;
    const char *content;
    const char *scope_user;
    const char *scope_project;
    const char *scope_task;
    const char *entity_key;
    const char *predicate;
    double importance;
    double confidence;
    double reusability;
    double specificity;
    int hit_count;
    int64_t last_hit_at;
    double decay;
    const char *status;
    int version;
    const char *supersedes;
    int64_t created_at;
    int64_t updated_at;
    const char *source_event_ids;
    int conflict_count;
    const char *conflict_ids;
    const char *conflict_resolution;
    const char *evidence_json;
    const char *retrieval_source;
    double retrieval_score;
} cbm_memory_item_t;

/* ── Memory query / result ─────────────────────────────────────────── */

typedef struct {
    const char *project;
    const char *user;
    const char *task;
    const char *entity_key;
    const char *kind;
    const char *query;
    bool include_inactive;
    int limit;
    /* Optional: qualified_name of the code symbol the agent is currently looking
     * at. When set, memories anchored to it (or to a symbol in the same file) via
     * an about_code edge get a retrieval boost. Pure ranking signal — never
     * changes the candidate set. NULL = no code context. */
    const char *code_context;
    /* Optional borrowed handle to the project's code-graph DB (NOT owned; valid
     * only for the duration of the call). Since memory and graph now live in
     * separate files, the anchor-boost pass reads `nodes` through this handle.
     * NULL disables anchor boosting (memories still returned, just unboosted). */
    struct sqlite3 *graph_db;
} cbm_memory_query_t;

typedef struct {
    cbm_memory_item_t *items;
    int count;
    int total;
} cbm_memory_result_t;

/* ── Memory health ─────────────────────────────────────────────────── */

typedef struct {
    int event_count;
    int item_count;
    int edge_count;
    int candidate_count;
    int active_count;
    int deprecated_count;
    int archived_count;
    int retracted_count;
    int deleted_count; /* soft-deleted, awaiting retention-sweep physical purge */
    int conflict_count;
    int scope_count;
    double hit_rate;
    int64_t total_hits;
} cbm_memory_health_t;

/* Report from cbm_store_memory_maintain_if_due: what (if anything) the lazy
 * auto-maintenance pass actually did this call. */
typedef struct {
    bool consolidated;     /* consolidate pass ran */
    int consolidate_count; /* candidates processed by it */
    bool decayed;          /* decay pass ran */
    int decay_count;       /* items decayed/archived by it */
    bool swept;            /* retention sweep ran */
    int sweep_count;       /* expired soft-deletes physically purged by it */
} cbm_memory_maintain_report_t;

/* ── Global memory project ─────────────────────────────────────────── */

/* Sentinel project name for the global (cross-project) memory store. Memories
 * with scope_project=NULL — user profile, preferences, cross-project lessons —
 * live in <cache>/__global__-memory.db, opened in addition to the per-project
 * store and union-merged on retrieval so they are visible from every project.
 * Chosen so cbm_validate_project_name accepts it (alphanumerics + underscore,
 * no leading dot) and cli_is_rebuildable_index spares it (the "-memory.db"
 * suffix). It is NOT a real indexable project: list_projects filters it out. */
#define CBM_GLOBAL_MEMORY_PROJECT "__global__"

/* ── Path utility ──────────────────────────────────────────────────── */

/* Derive the per-project memory DB path: <cache>/<project>-memory.db.
 * Memory lives in its own file so rebuilding the code graph never destroys it.
 * Returns CBM_STORE_OK and fills buf, or CBM_STORE_ERR on bad input/overflow. */
int cbm_memory_db_path(const char *project, char *buf, size_t bufsz);

/* One-time, idempotent migration: lift memory_* rows from a legacy merged graph
 * DB (where memory and graph shared one file) into this freshly opened memory
 * store. No-op once the memory DB has been migrated, or if graph_db_path is
 * absent / carries no memory rows. Best-effort: failure leaves the memory store
 * usable. Returns CBM_STORE_OK on success/no-op, CBM_STORE_ERR on copy failure. */
int cbm_store_migrate_memory_from_graph(cbm_store_t *mem, const char *graph_db_path);

/* ── Memory CRUD ────────────────────────────────────────────────────── */

int cbm_store_memory_append_event(cbm_store_t *s, const cbm_memory_event_t *event,
                                  char **out_event_id);
int cbm_store_memory_append_candidate(cbm_store_t *s, const cbm_memory_item_t *item,
                                      char **out_item_id);
int cbm_store_memory_get_item(cbm_store_t *s, const char *id, cbm_memory_item_t *out);
/* Anchor a memory to a code symbol: creates an about_code edge from the memory
 * item to a code node, addressed by its stable qualified_name (NOT the volatile
 * integer node id — qn survives re-indexing). dst is stored as "code:<qn>".
 * One-directional (memory -> code); the code graph never references memories, so
 * re-indexing the code graph is unaffected. Idempotent (dedup on src,dst,type).
 * origin is "user" (explicit) or "auto". about_code edges are deliberately kept
 * out of the evidence-subgraph walk — they are a recall signal, not graph algo. */
int cbm_store_memory_link_code(cbm_store_t *s, const char *item_id, const char *qualified_name,
                               const char *origin);
/* P3-a: derive confidence/reusability from a memory's about_code anchors using
 * the borrowed code-graph handle. Returns the count of anchors that resolve to a
 * real graph symbol (0 = no usable signal → caller keeps declared values). */
int cbm_store_memory_score_from_anchors(cbm_store_t *s, struct sqlite3 *graph_db,
                                        const char *item_id, const char *project, double *out_conf,
                                        double *out_reuse);
int cbm_store_memory_retrieve(cbm_store_t *s, const cbm_memory_query_t *query,
                              cbm_memory_result_t *out);
int cbm_store_memory_mark_hits(cbm_store_t *s, const char **ids, int count, int64_t now_ms);
int cbm_store_memory_update_status(cbm_store_t *s, const char *id, const char *project,
                                   const char *status);
int cbm_store_memory_feedback(cbm_store_t *s, const char *id, const char *project,
                              const char *feedback, const char *note, const char *user,
                              char **out_event_id);
/* Delete a memory item (P0-2). mode (default "soft" when NULL/empty):
 *   "soft"  — mark deleted_at; hidden from retrieval, undoable via restore until
 *             the retention sweep physically purges it past the grace window.
 *   "hard"  — delete item + vec + fts + edges in one transaction; source events
 *             are KEPT as an audit trail.
 *   "purge" — hard, plus delete the item's own source events (GDPR erasure).
 * Every mode writes a tombstone audit event (the tombstone survives purge). The
 * delete is scope-guarded: a non-NULL project that doesn't match the item's
 * scope_project returns CBM_STORE_NOT_FOUND. Returns CBM_STORE_OK,
 * CBM_STORE_NOT_FOUND (no such item / already soft-deleted / out of scope), or
 * CBM_STORE_ERR. */
int cbm_store_memory_delete(cbm_store_t *s, const char *id, const char *project, const char *mode,
                            const char *user);
/* Undo a soft delete: clear deleted_at, scope-guarded, writes a restore audit
 * event. Returns CBM_STORE_NOT_FOUND if the item isn't soft-deleted. */
int cbm_store_memory_restore(cbm_store_t *s, const char *id, const char *project, const char *user);
/* Retention sweep: physically purge every item soft-deleted more than grace_ms
 * ago (full purge, source events included — the grace window was the undo
 * chance). Collects ids first then deletes in one batch transaction. *purged
 * receives the count removed. */
int cbm_store_memory_purge_expired(cbm_store_t *s, const char *project, int64_t grace_ms,
                                   int *purged);
int cbm_store_memory_consolidate(cbm_store_t *s, const char *project, int limit, int *processed);
int cbm_store_memory_decay(cbm_store_t *s, const char *project, int limit, int *processed);
/* Lazy auto-maintenance: runs consolidate and/or decay only when "due" (by a
 * cheap candidate-count + elapsed-time gate), so a single-user agent never has
 * to call admin endpoints by hand. Safe to call on the memory hot path: it must
 * NOT be invoked inside an open transaction (consolidate opens its own). Honors
 * env CBM_MEMORY_AUTO_MAINTAIN=0 to disable. out may be NULL. Maintenance
 * failures are swallowed (return CBM_STORE_OK) so they never fail the caller. */
int cbm_store_memory_maintain_if_due(cbm_store_t *s, const char *project,
                                     cbm_memory_maintain_report_t *out);
/* Rebuild the FTS index for a project from memory_item using the current
 * segmentation (heals rows indexed before CJK bigram segmentation existed).
 * Returns number of items reindexed in *processed. */
int cbm_store_memory_reindex_fts(cbm_store_t *s, const char *project, int *processed);
int cbm_store_memory_health(cbm_store_t *s, const char *project, cbm_memory_health_t *out);
void cbm_store_memory_item_free(cbm_memory_item_t *item);
void cbm_store_memory_result_free(cbm_memory_result_t *out);

/* ── ADR ───────────────────────────────────────────────────────────── */

/* P1: ADR list — structured query for decision/constraint-class memories.
 * Returns a JSON array of matching items (projection: id, kind, layer, title,
 * summary, entity_key, status, importance, hit_count, decay, version,
 * supersedes, created_at, updated_at). Filters by scope_project, kind,
 * status, and entity_key. Caller must free the returned string. */
int cbm_store_memory_adr_list(cbm_store_t *s, const char *project, const char *kind_filter,
                              const char *status_filter, const char *entity_key_filter,
                              int limit, char **out_json);

/* Same as cbm_store_memory_adr_list but queries the global (cross-project)
 * store where scope_project IS NULL. project label in output is "__global__". */
int cbm_store_memory_adr_list_global(cbm_store_t *s, const char *kind_filter,
                                     const char *status_filter,
                                     const char *entity_key_filter,
                                     int limit, char **out_json);

/* Walk the supersedes chain for an ADR. Start from item_id (walk backward to
 * root then forward to newest) or entity_key (find root at version=1). Returns
 * JSON with items in version order (oldest first) plus generation ordinal.
 * Cycle detection caps at max_depth. Caller frees *out_json. */
int cbm_store_memory_adr_chain(cbm_store_t *s, const char *project,
                               const char *entity_key, const char *item_id,
                               int max_depth, char **out_json);

#endif /* CBM_MEMORY_STORE_H */
