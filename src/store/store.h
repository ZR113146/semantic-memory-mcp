/*
 * store.h — Opaque SQLite graph store for code knowledge graphs.
 *
 * All functions are prefixed cbm_store_*. The store handle is opaque —
 * callers never touch SQLite internals directly.
 *
 * Thread safety: a single store handle must not be used concurrently.
 * Use one store per thread or external synchronization.
 */
#ifndef CBM_STORE_H
#define CBM_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Opaque handle ──────────────────────────────────────────────── */

typedef struct cbm_store cbm_store_t;

/* ── Result codes ───────────────────────────────────────────────── */

#define CBM_STORE_OK 0
#define CBM_STORE_ERR (-1)
#define CBM_STORE_NOT_FOUND (-2)

/* ── Data structures ────────────────────────────────────────────── */

typedef struct {
    int64_t id;
    const char *project;
    const char *label;          /* Function, Class, Method, Module, File, ... */
    const char *name;           /* short name */
    const char *qualified_name; /* full dotted path */
    const char *file_path;      /* relative file path */
    int start_line;
    int end_line;
    const char *properties_json; /* JSON string, NULL → "{}" */
} cbm_node_t;

typedef struct {
    int64_t id;
    const char *project;
    int64_t source_id;
    int64_t target_id;
    const char *type;            /* CALLS, HTTP_CALLS, IMPORTS, ... */
    const char *properties_json; /* JSON string, NULL → "{}" */
} cbm_edge_t;

typedef struct {
    const char *name;
    const char *indexed_at; /* ISO 8601 */
    const char *root_path;
} cbm_project_t;

typedef struct {
    const char *project;
    const char *rel_path;
    const char *sha256;
    int64_t mtime_ns;
    int64_t size;
} cbm_file_hash_t;

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

/* Find nodes overlapping a line range in a file (excludes Module/Package). */
int cbm_store_find_nodes_by_file_overlap(cbm_store_t *s, const char *project, const char *file_path,
                                         int start_line, int end_line, cbm_node_t **out,
                                         int *count);

/* Find nodes whose qualified_name ends with the given suffix (dot-boundary). */
int cbm_store_find_nodes_by_qn_suffix(cbm_store_t *s, const char *project, const char *suffix,
                                      cbm_node_t **out, int *count);

/* Get CALLS degree of a node (inbound and outbound). */
void cbm_store_node_degree(cbm_store_t *s, int64_t node_id, int *in_deg, int *out_deg);

/* Get distinct file paths for a project. Caller must free each out[i] and out itself.
 * Returns CBM_STORE_OK or CBM_STORE_ERR. */
int cbm_store_list_files(cbm_store_t *s, const char *project, char ***out, int *count);

/* Get caller/callee names for a node (CALLS/HTTP_CALLS/ASYNC_CALLS edges).
 * Returns 0 on success. Caller must free each out_callers[i]/out_callees[i]
 * and the arrays themselves. */
int cbm_store_node_neighbor_names(cbm_store_t *s, int64_t node_id, int limit, char ***out_callers,
                                  int *caller_count, char ***out_callees, int *callee_count);

/* Batch count in/out degree for multiple nodes.
 * edge_type: filter by edge type (e.g. "CALLS"), or NULL/"" for all types.
 * out_in[i] and out_out[i] receive the in/out degree for node_ids[i].
 * Returns CBM_STORE_OK or CBM_STORE_ERR. */
int cbm_store_batch_count_degrees(cbm_store_t *s, const int64_t *node_ids, int id_count,
                                  const char *edge_type, int *out_in, int *out_out);

/* Upsert file hashes in batch. */
int cbm_store_upsert_file_hash_batch(cbm_store_t *s, const cbm_file_hash_t *hashes, int count);

/* Find edges whose properties contain a url_path matching the keyword. */
int cbm_store_find_edges_by_url_path(cbm_store_t *s, const char *project, const char *keyword,
                                     cbm_edge_t **out, int *count);

/* Restore database from another store (backup API). */
int cbm_store_restore_from(cbm_store_t *dst, cbm_store_t *src);

/* ── Search ─────────────────────────────────────────────────────── */

typedef struct {
    const char *project;
    const char *label;        /* NULL = any label */
    const char *name_pattern; /* regex on name, NULL = any */
    const char *qn_pattern;   /* regex on qualified_name, NULL = any */
    const char *file_pattern; /* glob on file_path, NULL = any */
    const char *relationship; /* edge type filter, NULL = any */
    const char *direction;    /* "inbound" / "outbound" / "any", NULL = any */
    int min_degree;           /* -1 = no filter (default), 0+ = minimum */
    int max_degree;           /* -1 = no filter (default), 0+ = maximum */
    int limit;                /* 0 = default (10) */
    int offset;
    bool exclude_entry_points;
    bool include_connected;
    const char *sort_by; /* "relevance" / "name" / "degree", NULL = relevance */
    bool case_sensitive;
    const char **exclude_labels; /* NULL-terminated array, or NULL */
} cbm_search_params_t;

typedef struct {
    cbm_node_t node;
    int in_degree;
    int out_degree;
    /* connected_names: allocated array of strings, count in connected_count */
    const char **connected_names;
    int connected_count;
} cbm_search_result_t;

typedef struct {
    cbm_search_result_t *results;
    int count;
    int total; /* total before pagination */
} cbm_search_output_t;

/* ── Traversal ──────────────────────────────────────────────────── */

typedef struct {
    cbm_node_t node;
    int hop; /* BFS depth from root */
} cbm_node_hop_t;

typedef struct {
    const char *from_name;
    const char *to_name;
    const char *type;
    double confidence;
} cbm_edge_info_t;

typedef struct {
    cbm_node_t root;
    cbm_node_hop_t *visited;
    int visited_count;
    cbm_edge_info_t *edges;
    int edge_count;
} cbm_traverse_result_t;

/* ── Schema introspection ───────────────────────────────────────── */

typedef struct {
    const char *label;
    int count;
    char **properties; /* distinct property keys for this label (base + JSON) */
    int property_count;
} cbm_label_count_t;

typedef struct {
    const char *type;
    int count;
    char **properties; /* distinct property keys for this edge type (base + JSON) */
    int property_count;
} cbm_type_count_t;

typedef struct {
    cbm_label_count_t *node_labels;
    int node_label_count;
    cbm_type_count_t *edge_types;
    int edge_type_count;
    /* relationship patterns like "(Function)-[CALLS]->(Function) [123x]" */
    const char **rel_patterns;
    int rel_pattern_count;
    const char **sample_func_names;
    int sample_func_count;
    const char **sample_class_names;
    int sample_class_count;
    const char **sample_qns;
    int sample_qn_count;
} cbm_schema_info_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/* Open an in-memory database (for testing). */
cbm_store_t *cbm_store_open_memory(void);

/* Open a file-backed database at the given path. Creates if needed. */
cbm_store_t *cbm_store_open_path(const char *db_path);

/* Open an existing file-backed database for querying only (no SQLITE_OPEN_CREATE).
 * Returns NULL if the file does not exist — never creates a new .db file. */
cbm_store_t *cbm_store_open_path_query(const char *db_path);

/* Check database integrity. Returns true if the DB passes basic sanity checks
 * (projects table has correct types, no corruption indicators).
 * Returns false if corruption is detected — caller should delete and re-index. */
bool cbm_store_check_integrity(cbm_store_t *s);

/* Open database for a named project in the default cache dir. */
cbm_store_t *cbm_store_open(const char *project);

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

/* Close the store and free all resources. NULL-safe. */
void cbm_store_close(cbm_store_t *s);

/* Get the underlying sqlite3 handle (for testing only). */
struct sqlite3 *cbm_store_get_db(cbm_store_t *s);

/* Get the last error message (static string, valid until next call). */
const char *cbm_store_error(cbm_store_t *s);

/* ── Transaction ────────────────────────────────────────────────── */

/* Begin a transaction. Returns CBM_STORE_OK on success. */
int cbm_store_begin(cbm_store_t *s);

/* Commit the current transaction. */
int cbm_store_commit(cbm_store_t *s);

/* Rollback the current transaction. */
int cbm_store_rollback(cbm_store_t *s);

/* ── Bulk write optimization ────────────────────────────────────── */

/* Tune pragmas for bulk write throughput (synchronous=OFF, large cache).
 * WAL journal mode is preserved throughout for crash safety. */
int cbm_store_begin_bulk(cbm_store_t *s);

/* Restore normal pragmas (synchronous=NORMAL, default cache) after bulk writes. */
int cbm_store_end_bulk(cbm_store_t *s);

/* Drop user indexes for faster bulk inserts. */
int cbm_store_drop_indexes(cbm_store_t *s);

/* Recreate user indexes after bulk inserts. */
int cbm_store_create_indexes(cbm_store_t *s);

/* ── WAL / Checkpoint ───────────────────────────────────────────── */

/* Force WAL checkpoint + PRAGMA optimize. */
int cbm_store_checkpoint(cbm_store_t *s);

/* Resolve the mmap_size pragma value applied to on-disk stores from the
 * CBM_SQLITE_MMAP_SIZE environment variable. Defaults to 67108864 (64 MB)
 * when the variable is unset, malformed, or partially numeric. Negative
 * values clamp to 0 (which disables mmap and reverts to read()/pread()
 * I/O — recoverable SQLITE_IOERR instead of SIGBUS when concurrent
 * processes truncate the DB file under live mappings). Exposed for
 * testability. */
int64_t cbm_store_resolve_mmap_size(void);

/* ── Dump / Restore ─────────────────────────────────────────────── */

/* Dump in-memory database to a file. */
int cbm_store_dump_to_file(cbm_store_t *s, const char *dest_path);

/* ── Project CRUD ───────────────────────────────────────────────── */

int cbm_store_upsert_project(cbm_store_t *s, const char *name, const char *root_path);
int cbm_store_get_project(cbm_store_t *s, const char *name, cbm_project_t *out);
int cbm_store_list_projects(cbm_store_t *s, cbm_project_t **out, int *count);
int cbm_store_delete_project(cbm_store_t *s, const char *name);

/* ── Node CRUD ──────────────────────────────────────────────────── */

/* Upsert a single node. Returns node ID (>0) or CBM_STORE_ERR. */
int64_t cbm_store_upsert_node(cbm_store_t *s, const cbm_node_t *n);

/* Upsert nodes in batch. out_ids must have room for count entries. */
int cbm_store_upsert_node_batch(cbm_store_t *s, const cbm_node_t *nodes, int count,
                                int64_t *out_ids);

/* Find node by primary key. Returns CBM_STORE_OK or CBM_STORE_NOT_FOUND. */
int cbm_store_find_node_by_id(cbm_store_t *s, int64_t id, cbm_node_t *out);

/* Find node by project + qualified_name. */
int cbm_store_find_node_by_qn(cbm_store_t *s, const char *project, const char *qn, cbm_node_t *out);

/* Find node by qualified_name only (no project filter — QNs are globally unique). */
int cbm_store_find_node_by_qn_any(cbm_store_t *s, const char *qn, cbm_node_t *out);

/* Find nodes by name (exact match). Returns allocated array, caller frees. */
int cbm_store_find_nodes_by_name(cbm_store_t *s, const char *project, const char *name,
                                 cbm_node_t **out, int *count);

/* Find nodes by name across all projects. Returns allocated array, caller frees. */
int cbm_store_find_nodes_by_name_any(cbm_store_t *s, const char *name, cbm_node_t **out,
                                     int *count);

/* Find nodes by label. */
int cbm_store_find_nodes_by_label(cbm_store_t *s, const char *project, const char *label,
                                  cbm_node_t **out, int *count);

/* Find nodes by file path. */
int cbm_store_find_nodes_by_file(cbm_store_t *s, const char *project, const char *file_path,
                                 cbm_node_t **out, int *count);

/* Batch lookup: map qualified names → node IDs.
 * qns[i] is resolved; out_ids[i] receives the ID or 0 if not found.
 * Returns number of QNs actually found, or CBM_STORE_ERR. */
int cbm_store_find_node_ids_by_qns(cbm_store_t *s, const char *project, const char **qns,
                                   int qn_count, int64_t *out_ids);

/* Count nodes in project. Returns count or CBM_STORE_ERR. */
int cbm_store_count_nodes(cbm_store_t *s, const char *project);

/* Delete all nodes for a project (cascade deletes edges). */
int cbm_store_delete_nodes_by_project(cbm_store_t *s, const char *project);

/* Delete nodes by file path. */
int cbm_store_delete_nodes_by_file(cbm_store_t *s, const char *project, const char *file_path);

/* Delete nodes by label. */
int cbm_store_delete_nodes_by_label(cbm_store_t *s, const char *project, const char *label);

/* ── Edge CRUD ──────────────────────────────────────────────────── */

/* Insert or update edge. Returns edge ID (>0) or CBM_STORE_ERR. */
int64_t cbm_store_insert_edge(cbm_store_t *s, const cbm_edge_t *e);

/* Insert edges in batch. */
int cbm_store_insert_edge_batch(cbm_store_t *s, const cbm_edge_t *edges, int count);

/* Find edges by source node. */
int cbm_store_find_edges_by_source(cbm_store_t *s, int64_t source_id, cbm_edge_t **out, int *count);

/* Find edges by target node. */
int cbm_store_find_edges_by_target(cbm_store_t *s, int64_t target_id, cbm_edge_t **out, int *count);

/* Find edges by source + type. */
int cbm_store_find_edges_by_source_type(cbm_store_t *s, int64_t source_id, const char *type,
                                        cbm_edge_t **out, int *count);

/* Find edges by target + type. */
int cbm_store_find_edges_by_target_type(cbm_store_t *s, int64_t target_id, const char *type,
                                        cbm_edge_t **out, int *count);

/* Find all edges of a type in project. */
int cbm_store_find_edges_by_type(cbm_store_t *s, const char *project, const char *type,
                                 cbm_edge_t **out, int *count);

/* Count all edges in project. */
int cbm_store_count_edges(cbm_store_t *s, const char *project);

/* Count edges of given type. */
int cbm_store_count_edges_by_type(cbm_store_t *s, const char *project, const char *type);

/* Delete all edges for a project. */
int cbm_store_delete_edges_by_project(cbm_store_t *s, const char *project);

/* Delete edges by type. */
int cbm_store_delete_edges_by_type(cbm_store_t *s, const char *project, const char *type);

/* ── File hash CRUD ─────────────────────────────────────────────── */

int cbm_store_upsert_file_hash(cbm_store_t *s, const char *project, const char *rel_path,
                               const char *sha256, int64_t mtime_ns, int64_t size);

int cbm_store_get_file_hashes(cbm_store_t *s, const char *project, cbm_file_hash_t **out,
                              int *count);

int cbm_store_delete_file_hash(cbm_store_t *s, const char *project, const char *rel_path);

int cbm_store_delete_file_hashes(cbm_store_t *s, const char *project);

/* -- Long-term memory MVP --------------------------------------- */

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

/* ── Search ─────────────────────────────────────────────────────── */

int cbm_store_search(cbm_store_t *s, const cbm_search_params_t *params, cbm_search_output_t *out);

/* Free a search output's allocated memory. */
void cbm_store_search_free(cbm_search_output_t *out);

/* ── Traversal ──────────────────────────────────────────────────── */

int cbm_store_bfs(cbm_store_t *s, int64_t start_id, const char *direction, const char **edge_types,
                  int edge_type_count, int max_depth, int max_results, cbm_traverse_result_t *out);

/* Free a traverse result's allocated memory. */
void cbm_store_traverse_free(cbm_traverse_result_t *out);

/* ── Impact analysis ────────────────────────────────────────────── */

typedef enum {
    CBM_RISK_CRITICAL = 0,
    CBM_RISK_HIGH = 1,
    CBM_RISK_MEDIUM = 2,
    CBM_RISK_LOW = 3,
} cbm_risk_level_t;

/* Map BFS hop depth to risk level. */
cbm_risk_level_t cbm_hop_to_risk(int hop);

/* String representation of risk level. */
const char *cbm_risk_label(cbm_risk_level_t level);

typedef struct {
    int critical;
    int high;
    int medium;
    int low;
    int total;
    bool has_cross_service;
} cbm_impact_summary_t;

/* Build impact summary from visited hops and edges. */
cbm_impact_summary_t cbm_build_impact_summary(const cbm_node_hop_t *hops, int hop_count,
                                              const cbm_edge_info_t *edges, int edge_count);

/* Deduplicate BFS hops, keeping minimum hop per node ID.
 * Returns allocated array and count via out params. Caller frees result. */
int cbm_deduplicate_hops(const cbm_node_hop_t *hops, int hop_count, cbm_node_hop_t **out,
                         int *out_count);

/* ── Schema ─────────────────────────────────────────────────────── */

int cbm_store_get_schema(cbm_store_t *s, const char *project, cbm_schema_info_t *out);

/* Like cbm_store_get_schema but skips per-label/per-type JSON property-key
 * discovery (json_each scans over every row) — for callers that only need
 * label/type counts, e.g. get_architecture. */
int cbm_store_get_schema_counts(cbm_store_t *s, const char *project, cbm_schema_info_t *out);

/* Free a schema info's allocated memory. */
void cbm_store_schema_free(cbm_schema_info_t *out);

/* ── Architecture ───────────────────────────────────────────────── */

typedef struct {
    const char *language;
    int file_count;
} cbm_language_count_t;

typedef struct {
    const char *name;
    int node_count;
    int fan_in;
    int fan_out;
} cbm_package_summary_t;

typedef struct {
    const char *name;
    const char *qualified_name;
    const char *file;
} cbm_entry_point_t;

typedef struct {
    const char *method;
    const char *path;
    const char *handler;
} cbm_route_info_t;

typedef struct {
    const char *name;
    const char *qualified_name;
    int fan_in;
} cbm_hotspot_t;

typedef struct {
    const char *from;
    const char *to;
    int call_count;
} cbm_cross_pkg_boundary_t;

typedef struct {
    const char *from;
    const char *to;
    const char *type;
    int count;
} cbm_service_link_t;

typedef struct {
    const char *name;
    const char *layer;
    const char *reason;
} cbm_package_layer_t;

typedef struct {
    int id;
    const char *label;
    int members;
    double cohesion;
    const char **top_nodes;
    int top_node_count;
    const char **packages;
    int package_count;
    const char **edge_types;
    int edge_type_count;
} cbm_cluster_info_t;

typedef struct {
    const char *path;
    const char *type; /* "dir" or "file" */
    int children;
} cbm_file_tree_entry_t;

typedef struct {
    /* Pointers first to minimize padding */
    cbm_language_count_t *languages;
    cbm_package_summary_t *packages;
    cbm_entry_point_t *entry_points;
    cbm_route_info_t *routes;
    cbm_hotspot_t *hotspots;
    cbm_cross_pkg_boundary_t *boundaries;
    cbm_service_link_t *services;
    cbm_package_layer_t *layers;
    cbm_cluster_info_t *clusters;
    cbm_file_tree_entry_t *file_tree;
    /* Counts after pointers */
    int language_count;
    int package_count;
    int entry_point_count;
    int route_count;
    int hotspot_count;
    int boundary_count;
    int service_count;
    int layer_count;
    int cluster_count;
    int file_tree_count;
} cbm_architecture_info_t;

int cbm_store_get_architecture(cbm_store_t *s, const char *project, const char **aspects,
                               int aspect_count, cbm_architecture_info_t *out);
void cbm_store_architecture_free(cbm_architecture_info_t *out);

/* ── ADR (Architecture Decision Record) ────────────────────────── */

#define CBM_ADR_MAX_LENGTH 8000

typedef struct {
    const char *project;
    const char *content;
    const char *created_at;
    const char *updated_at;
} cbm_adr_t;

int cbm_store_adr_store(cbm_store_t *s, const char *project, const char *content);
int cbm_store_adr_get(cbm_store_t *s, const char *project, cbm_adr_t *out);
int cbm_store_adr_delete(cbm_store_t *s, const char *project);
int cbm_store_adr_update_sections(cbm_store_t *s, const char *project, const char **keys,
                                  const char **values, int count, cbm_adr_t *out);
void cbm_store_adr_free(cbm_adr_t *adr);

/* ADR section parsing/rendering (pure functions, no store needed) */

enum { PROPS_MAX = 16 };

typedef struct {
    char *keys[PROPS_MAX];
    char *values[PROPS_MAX];
    int count;
} cbm_adr_sections_t;

cbm_adr_sections_t cbm_adr_parse_sections(const char *content);
char *cbm_adr_render(const cbm_adr_sections_t *sections);
int cbm_adr_validate_content(const char *content, char *errbuf, int errbuf_size);
int cbm_adr_validate_section_keys(const char **keys, int count, char *errbuf, int errbuf_size);
void cbm_adr_sections_free(cbm_adr_sections_t *s);

/* ── Search helpers (exposed for testing) ───────────────────────── */

/* Convert a glob pattern to SQL LIKE pattern. Caller must free result. */
char *cbm_glob_to_like(const char *pattern);

/* Extract literal substrings (>= 3 chars) from a regex pattern for LIKE pre-filtering.
 * Bails on alternation (|). Returns count of hints written to out[].
 * Each out[i] is malloc'd — caller must free each string. */
int cbm_extract_like_hints(const char *pattern, char **out, int max_out);

/* Prepend (?i) to a regex pattern if not already present.
 * Returns a static buffer — do NOT free. */
const char *cbm_ensure_case_insensitive(const char *pattern);

/* Strip leading (?i) from a regex pattern.
 * Returns a static buffer — do NOT free. */
const char *cbm_strip_case_flag(const char *pattern);

/* ── Architecture helpers (exposed for testing) ────────────────── */

const char *cbm_qn_to_package(const char *qn);
const char *cbm_qn_to_top_package(const char *qn);
bool cbm_is_test_file_path(const char *fp);
int cbm_store_find_architecture_docs(cbm_store_t *s, const char *project, char ***out, int *count);

/* ── Community detection (Leiden) ──────────────────────────────── */

typedef struct {
    int64_t src;
    int64_t dst;
} cbm_louvain_edge_t;

typedef struct {
    int64_t node_id;
    int community;
} cbm_louvain_result_t;

/* Multi-level Leiden community detection (Traag, Waltman & van Eck 2019,
 * arXiv:1810.08473): local moving + refinement + aggregation, repeated until
 * the partition can no longer be coarsened. Refinement guarantees every
 * reported community is internally connected. The resolution parameter
 * controls granularity (higher -> more, smaller communities); 1.0 is standard.
 * Allocates *out (length *out_count == node_count); the caller frees it. */
int cbm_leiden(const int64_t *nodes, int node_count, const cbm_louvain_edge_t *edges,
               int edge_count, double resolution, cbm_louvain_result_t **out, int *out_count);

/* Convenience wrapper: cbm_leiden with resolution 1.0. */
int cbm_louvain(const int64_t *nodes, int node_count, const cbm_louvain_edge_t *edges,
                int edge_count, cbm_louvain_result_t **out, int *out_count);

/* ── Memory management helpers ──────────────────────────────────── */

/* Free heap-allocated strings in a stack-allocated node (does NOT free the node itself). */
void cbm_node_free_fields(cbm_node_t *n);

/* Free heap-allocated strings in a stack-allocated project (does NOT free the project itself). */
void cbm_project_free_fields(cbm_project_t *p);

/* Free an array of nodes returned by find_nodes_by_* functions. */
void cbm_store_free_nodes(cbm_node_t *nodes, int count);

/* Free an array of edges returned by find_edges_by_* functions. */
void cbm_store_free_edges(cbm_edge_t *edges, int count);

/* Free an array of projects. */
void cbm_store_free_projects(cbm_project_t *projects, int count);

/* Free an array of file hashes. */
void cbm_store_free_file_hashes(cbm_file_hash_t *hashes, int count);

/* ── Vector search ───────────────────────────────────────────────── */

/* Result from vector similarity search. */
typedef struct {
    int64_t node_id;
    char *name;
    char *qualified_name;
    char *file_path;
    char *label;
    double score;
} cbm_vector_result_t;

/* Search for nodes similar to the given query keywords using stored RI vectors.
 * Builds a merged query vector from the keywords, then does cosine scan via
 * the cbm_cosine_i8 SQL function joined with the nodes table.
 * Returns results sorted by score DESC. Caller must free with cbm_store_free_vector_results. */
int cbm_store_vector_search(cbm_store_t *s, const char *project, const char **keywords,
                            int keyword_count, int limit, cbm_vector_result_t **out,
                            int *out_count);

/* Free vector search results. */
void cbm_store_free_vector_results(cbm_vector_result_t *results, int count);

/* Count vectors for a project. */
int cbm_store_count_vectors(cbm_store_t *s, const char *project);

/* Execute an arbitrary SQL statement (pragmas, FTS5 maintenance, etc).
 * Returns CBM_STORE_OK on success. */
int cbm_store_exec(cbm_store_t *s, const char *sql);

#endif /* CBM_STORE_H */
