/*
 * mcp_memory_handlers.c — Local-fork ADR MCP handler functions.
 *
 * Extracted from src/mcp/mcp.c (HEAD, commit 38f8d25) so mcp.c can be
 * replaced with the upstream version independently. Every handler here
 * opens its own memory-store handle via the public cbm_store_* API so it
 * never touches cbm_mcp_server_t internals (the struct definition is
 * static inside mcp.c).
 */

#include "mcp/mcp.h"
#include "store/store.h"
#include "memory/memory_store.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/compat.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ═════════════════════════════════════════════════════════════════════
 *  Internal helpers (all static — no symbols exported)
 * ═════════════════════════════════════════════════════════════════════ */

/* ── yyjson argument extraction ──────────────────────────────────── */

static yyjson_val *memory_arg(yyjson_doc *doc, const char *key) {
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    return root && yyjson_is_obj(root) ? yyjson_obj_get(root, key) : NULL;
}

static char *memory_arg_string_dup(yyjson_doc *doc, const char *key) {
    yyjson_val *v = memory_arg(doc, key);
    return (v && yyjson_is_str(v)) ? cbm_strdup(yyjson_get_str(v)) : NULL;
}

static double memory_arg_double(yyjson_doc *doc, const char *key, double def) {
    yyjson_val *v = memory_arg(doc, key);
    return (v && yyjson_is_num(v)) ? yyjson_get_real(v) : def;
}

static double memory_arg_positive_double(yyjson_doc *doc, const char *key, double def) {
    yyjson_val *v = memory_arg(doc, key);
    if (!v || !yyjson_is_num(v)) {
        return def;
    }
    double value = yyjson_get_real(v);
    return value > 0.0 ? value : def;
}

static char *memory_arg_raw_dup(yyjson_doc *doc, const char *key) {
    yyjson_val *v = memory_arg(doc, key);
    if (!v)
        return NULL;
    if (yyjson_is_str(v))
        return cbm_strdup(yyjson_get_str(v));
    return yyjson_val_write(v, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
}

/* Confidence/reusability scoring (formerly memory_l1_blend + the P4 reusability
 * floor, both inline here) now lives in one place: cbm_memory_score_item in
 * memory_store.c, which folds L1 graph signal ⊕ L2 kind prior ⊕ L3 declared. */

/* Free the heap-copied about_code anchor list (see handle_events). */
static void free_anchor_qns(char **qns, size_t n) {
    if (!qns) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(qns[i]);
    }
    free(qns);
}

/* P3-d structure dimension (HELPER, advice only — never rejects, never requires
 * exact format). For a decision-class memory, do a deliberately LOOSE check for
 * ADR structure elements (CN or EN keywords, order/format ignored) and return a
 * gentle nudge string, or NULL when no advice is warranted. Two cases earn a
 * nudge: (1) substantial content with NO recognizable structure element at all;
 * (2) has a decision element but is missing the "rejected alternatives" part —
 * the most valuable and most-often-omitted ADR section. Short content is left
 * alone. Returns a static string (not owned by caller). */
static const char *memory_structure_advice(const char *kind, const char *content) {
    if (!kind || !content) {
        return NULL;
    }
    if (strcmp(kind, "decision") != 0 && strcmp(kind, "constraint") != 0) {
        return NULL;
    }
    if (strlen(content) < 80) {
        return NULL; /* too short to expect full structure */
    }
    bool has_decision = strstr(content, "决策") || strstr(content, "Decision") ||
                        strstr(content, "decision") || strstr(content, "[Decision]");
    bool has_context =
        strstr(content, "背景") || strstr(content, "Context") || strstr(content, "context");
    bool has_rejected = strstr(content, "否决") || strstr(content, "替代") ||
                        strstr(content, "Rejected") || strstr(content, "alternative") ||
                        strstr(content, "Alternative");
    bool any_structure = has_decision || has_context || has_rejected;
    if (!any_structure) {
        return "no recognizable ADR structure: consider stating the Decision, its "
               "Context, and the Rejected alternatives so the rationale survives.";
    }
    if (!has_rejected) {
        return "missing 'Rejected alternatives': recording what you DIDN'T choose and "
               "why is the most valuable part of an ADR for a future reader.";
    }
    return NULL;
}


/* ── Phantom project name guard ─────────────────────────────────────
 * An earlier buggy list_projects handed callers the sidecar filename
 * "<project>-memory"; passed back as a project it makes
 * cbm_memory_db_path re-append "-memory.db" → a spurious
 * "<project>-memory-memory.db" orphan, and the SQL filter
 * scope_project="<project>-memory" matches nothing (the rows store the
 * un-suffixed name). If the incoming name ends in "-memory" AND the
 * de-suffixed base already has a real "<base>-memory.db" on disk, the
 * name is a phantom: return the base. Otherwise return a copy of the
 * input unchanged. A genuine project whose own name ends in "-memory"
 * (e.g. "D-semantic-memory-mcp" — its base "D-semantic-memory-" has no
 * "-memory.db") is left intact. Caller frees. */

static char *normalize_phantom_project(const char *project) {
    if (!project) {
        return NULL;
    }
    const char *suf = "-memory";
    size_t plen = strlen(project);
    size_t slen = strlen(suf);
    if (plen > slen && strcmp(project + plen - slen, suf) == 0) {
        char base[CBM_SZ_1K];
        snprintf(base, sizeof(base), "%.*s", (int)(plen - slen), project);
        char base_mem_path[CBM_SZ_1K];
        if (cbm_memory_db_path(base, base_mem_path, sizeof(base_mem_path)) == CBM_STORE_OK &&
            cbm_file_exists(base_mem_path)) {
            return cbm_strdup(base);
        }
    }
    return cbm_strdup(project);
}

/* ── Open memory store (read-only, independent of srv internals) ─── */

static cbm_store_t *open_memory_store_for_project(const char *project) {
    if (!project) {
        return NULL;
    }
    char mem_path[CBM_SZ_1K];
    if (cbm_memory_db_path(project, mem_path, sizeof(mem_path)) != CBM_STORE_OK) {
        return NULL;
    }
    if (!cbm_file_exists(mem_path)) {
        return NULL;
    }
    return cbm_store_open_path_query(mem_path);
}

/* Mutation handlers must not write through the query-only handle returned by
 * resolve_*_memory_store(..., false). Resolve once in read mode to preserve the
 * "existing DB only" guard, then ask the resolver to reopen that cached handle
 * read-write. This avoids both SQLITE_READONLY on a cold server and accidental
 * creation of a memory DB for a misspelled project. */
static cbm_store_t *open_existing_memory_store_for_write(cbm_mcp_server_t *srv,
                                                         const char *project) {
    if (!resolve_memory_store(srv, project, false)) {
        return NULL;
    }
    return resolve_memory_store(srv, project, true);
}

static cbm_store_t *open_existing_global_memory_store_for_write(cbm_mcp_server_t *srv) {
    if (!resolve_global_memory_store(srv, false)) {
        return NULL;
    }
    return resolve_global_memory_store(srv, true);
}

/* ═════════════════════════════════════════════════════════════════════
 *  Public handlers (declared in mcp.h, called from mcp.c dispatch)
 * ═════════════════════════════════════════════════════════════════════ */

/* ═════════════════════════════════════════════════════════════════════
 *  Memory tool handlers (moved verbatim from mcp.c, 2026-07-09).
 *  Store access goes through resolve_memory_store/resolve_global_memory_store
 *  exported by mcp.c — the only functions touching cbm_mcp_server internals.
 * ═════════════════════════════════════════════════════════════════════ */

static const char *memory_item_str(const char *s) {
    return s ? s : "";
}

static yyjson_mut_val *memory_item_to_json(yyjson_mut_doc *doc, const cbm_memory_item_t *it) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, obj, "id", memory_item_str(it->id));
    yyjson_mut_obj_add_str(doc, obj, "kind", memory_item_str(it->kind));
    yyjson_mut_obj_add_str(doc, obj, "layer", memory_item_str(it->layer));
    yyjson_mut_obj_add_str(doc, obj, "title", memory_item_str(it->title));
    yyjson_mut_obj_add_str(doc, obj, "summary", memory_item_str(it->summary));
    yyjson_mut_obj_add_str(doc, obj, "content", memory_item_str(it->content));
    yyjson_mut_obj_add_str(doc, obj, "scope_user", memory_item_str(it->scope_user));
    yyjson_mut_obj_add_str(doc, obj, "scope_project", memory_item_str(it->scope_project));
    yyjson_mut_obj_add_str(doc, obj, "scope_task", memory_item_str(it->scope_task));
    yyjson_mut_obj_add_str(doc, obj, "entity_key", memory_item_str(it->entity_key));
    yyjson_mut_obj_add_str(doc, obj, "predicate", memory_item_str(it->predicate));
    yyjson_mut_obj_add_real(doc, obj, "importance", it->importance);
    yyjson_mut_obj_add_real(doc, obj, "confidence", it->confidence);
    yyjson_mut_obj_add_real(doc, obj, "reusability", it->reusability);
    yyjson_mut_obj_add_real(doc, obj, "specificity", it->specificity);
    yyjson_mut_obj_add_int(doc, obj, "hit_count", it->hit_count);
    yyjson_mut_obj_add_int(doc, obj, "last_hit_at", it->last_hit_at);
    yyjson_mut_obj_add_real(doc, obj, "decay", it->decay);
    yyjson_mut_obj_add_str(doc, obj, "status", memory_item_str(it->status));
    yyjson_mut_obj_add_int(doc, obj, "version", it->version);
    yyjson_mut_obj_add_str(doc, obj, "supersedes", memory_item_str(it->supersedes));
    yyjson_mut_obj_add_int(doc, obj, "created_at", it->created_at);
    yyjson_mut_obj_add_int(doc, obj, "updated_at", it->updated_at);
    yyjson_mut_obj_add_str(doc, obj, "source_event_ids", memory_item_str(it->source_event_ids));
    yyjson_mut_obj_add_int(doc, obj, "conflict_count", it->conflict_count);
    yyjson_mut_obj_add_str(doc, obj, "conflict_ids", memory_item_str(it->conflict_ids));
    yyjson_mut_obj_add_str(doc, obj, "conflict_resolution",
                           memory_item_str(it->conflict_resolution));
    yyjson_mut_obj_add_str(doc, obj, "evidence_json", memory_item_str(it->evidence_json));
    yyjson_mut_obj_add_str(doc, obj, "retrieval_source", memory_item_str(it->retrieval_source));
    yyjson_mut_obj_add_real(doc, obj, "retrieval_score", it->retrieval_score);
    return obj;
}

static bool memory_policy_has_signal(const char *s) {
    if (!s)
        return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p > ' ')
            return true;
    }
    return false;
}

static bool memory_policy_contains_i(const char *hay, const char *needle) {
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

/* Skip leading ASCII whitespace. */
static const char *memory_skip_ws(const char *s) {
    if (!s)
        return s;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    return s;
}

/* Structured-format gate for high-value kinds (decision/lesson/constraint).
 * Returns a non-NULL reason string when the write is malformed, NULL when OK.
 * Enforces the ADR contract advertised in the events tool schema so the format
 * holds across every window regardless of how the user phrased the request:
 *   - summary and content must both be present and non-empty
 *   - content must be plain text, not a {"content":...} JSON wrapper
 *   - summary must not be a copy of content (it is the independent recall key)
 * Other kinds (fact/todo/reference/raw events) are not constrained here. */
static const char *memory_validate_format(const char *kind, const char *summary,
                                          const char *content) {
    if (!kind)
        return NULL;
    if (strcmp(kind, "decision") != 0 && strcmp(kind, "lesson") != 0 &&
        strcmp(kind, "constraint") != 0)
        return NULL;
    if (!memory_policy_has_signal(summary))
        return "missing_summary";
    if (!memory_policy_has_signal(content))
        return "missing_content";
    /* Reject JSON-wrapped content like {"content":"..."} — a write-side bug that
     * double-encodes the field. Detect a leading '{' followed by a quoted key. */
    const char *c = memory_skip_ws(content);
    if (*c == '{') {
        const char *q = memory_skip_ws(c + 1);
        if (*q == '"')
            return "content_json_wrapped";
    }
    if (strcmp(summary, content) == 0)
        return "summary_equals_content";
    return NULL;
}

static const char *memory_write_policy_decide(const char *text, const char *kind, const char *type,
                                              const char **reason) {
    if (!memory_policy_has_signal(text)) {
        if (reason)
            *reason = "empty_payload";
        return "rejected";
    }
    if ((kind && (strcmp(kind, "debug") == 0 || strcmp(kind, "scratch") == 0)) ||
        (type && (strcmp(type, "debug") == 0 || strcmp(type, "scratch") == 0)) ||
        memory_policy_contains_i(text, "temporary note") ||
        memory_policy_contains_i(text, "scratch note") ||
        memory_policy_contains_i(text, "临时记录") || memory_policy_contains_i(text, "临时笔记") ||
        memory_policy_contains_i(text, "草稿")) {
        if (reason)
            *reason = "low_value_transient";
        return "rejected";
    }
    if ((kind && (strcmp(kind, "preference") == 0 || strcmp(kind, "decision") == 0 ||
                  strcmp(kind, "constraint") == 0 || strcmp(kind, "lesson") == 0)) ||
        memory_policy_contains_i(text, "remember") ||
        memory_policy_contains_i(text, "do not forget") || memory_policy_contains_i(text, "记住") ||
        memory_policy_contains_i(text, "牢记") || memory_policy_contains_i(text, "别忘") ||
        memory_policy_contains_i(text, "不要忘") || memory_policy_contains_i(text, "务必")) {
        if (reason)
            *reason = "explicit_or_high_value";
        return "must_write";
    }
    if (reason)
        *reason = "default_candidate";
    return "candidate";
}
char *handle_events(cbm_mcp_server_t *srv, const char *args) {
    yyjson_doc *adoc = yyjson_read(args ? args : "{}", args ? strlen(args) : 2, 0);
    if (!adoc)
        return cbm_mcp_text_result("invalid JSON arguments", true);
    char *project = memory_arg_string_dup(adoc, "project");
    char *scope = memory_arg_string_dup(adoc, "scope");
    char *type = memory_arg_string_dup(adoc, "type");
    char *source = memory_arg_string_dup(adoc, "source");
    char *user = memory_arg_string_dup(adoc, "user");
    char *task = memory_arg_string_dup(adoc, "task");
    char *kind = memory_arg_string_dup(adoc, "kind");
    char *layer = memory_arg_string_dup(adoc, "layer");
    char *title = memory_arg_string_dup(adoc, "title");
    char *summary = memory_arg_string_dup(adoc, "summary");
    char *entity_key = memory_arg_string_dup(adoc, "entity_key");
    char *predicate = memory_arg_string_dup(adoc, "predicate");
    char *payload = memory_arg_raw_dup(adoc, "payload");
    char *content = memory_arg_string_dup(adoc, "content");
    char *supersedes = memory_arg_string_dup(adoc, "supersedes");
    char *context_json = memory_arg_raw_dup(adoc, "context");
    double confidence = memory_arg_double(adoc, "confidence", 0.5);
    double importance = memory_arg_positive_double(adoc, "importance", 0.5);
    double reusability = memory_arg_positive_double(adoc, "reusability", 0.5);
    double specificity = memory_arg_positive_double(adoc, "specificity", 0.5);
    /* Extract about_code anchors BEFORE freeing adoc. (Previously this array was
     * read from adoc AFTER yyjson_doc_free — a use-after-free that silently
     * dropped every anchor, which is also why graph scoring had nothing to read.
     * Copy the qualified-name strings onto the heap so they outlive the doc.) */
    char **about_code_qns = NULL;
    size_t about_code_n = 0;
    {
        yyjson_val *ac = yyjson_obj_get(yyjson_doc_get_root(adoc), "about_code");
        if (ac && yyjson_is_arr(ac)) {
            size_t cap = yyjson_arr_size(ac);
            if (cap > 0) {
                about_code_qns = calloc(cap, sizeof(char *));
                if (about_code_qns) {
                    size_t ai, amax;
                    yyjson_val *av;
                    yyjson_arr_foreach(ac, ai, amax, av) {
                        if (yyjson_is_str(av)) {
                            about_code_qns[about_code_n++] = cbm_strdup(yyjson_get_str(av));
                        }
                    }
                }
            }
        }
    }
    yyjson_doc_free(adoc);
    if (!project || !payload) {
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("project and payload are required", true);
    }
    /* Collapse a phantom "<project>-memory" name so writes land in the real
     * store with the canonical scope_project (not a "-memory-memory.db" orphan). */
    {
        char *canon = normalize_phantom_project(project);
        if (canon) {
            free(project);
            project = canon;
        }
    }
    /* scope routes the write: "global" lands in the cross-project store with
     * scope_project=NULL; anything else (default) is project-scoped. `project`
     * stays required and is still used as anchor/audit context even for global
     * writes — it just isn't the storage key. */
    bool is_global = scope && strcmp(scope, "global") == 0;

    const char *policy_reason = NULL;
    const char *policy_decision =
        memory_write_policy_decide(content ? content : payload, kind, type, &policy_reason);

    /* Structured-format gate: for high-value kinds, malformed writes (missing or
     * duplicated summary, JSON-wrapped content) are downgraded to rejected so the
     * ADR format holds across every window. Only override an otherwise-accepting
     * decision — never flip an already-rejected one. */
    if (strcmp(policy_decision, "rejected") != 0) {
        const char *fmt_reason = memory_validate_format(kind, summary, content);
        if (fmt_reason) {
            policy_decision = "rejected";
            policy_reason = fmt_reason;
        }
    }

    /* Resolve the store early so we can write the audit event even for rejected writes.
     * Write path: create-if-absent so a pure-memory project's first write builds its DB.
     * Global writes go to the cross-project store instead of the per-project one. */
    cbm_store_t *store = is_global ? resolve_global_memory_store(srv, true)
                                   : resolve_memory_store(srv, project, true);

    if (strcmp(policy_decision, "rejected") == 0) {
        /* Write a lightweight audit.rejected event so policy decisions are traceable
         * and can be used to tune write policy thresholds (framework §3, §0 principle 8). */
        if (store) {
            cbm_memory_event_t audit_ev = {0};
            audit_ev.type = "audit.rejected";
            audit_ev.source = source ? source : "mcp.events";
            audit_ev.project = project;
            audit_ev.user = user;
            audit_ev.payload = payload;
            audit_ev.confidence = 0.0;
            /* Encode the rejection reason in context so it's queryable. */
            char audit_ctx[256];
            snprintf(audit_ctx, sizeof(audit_ctx),
                     "{\"policy_reason\":\"%s\",\"kind\":\"%s\",\"type\":\"%s\"}",
                     policy_reason ? policy_reason : "", kind ? kind : "", type ? type : "");
            audit_ev.context_json = audit_ctx;
            (void)cbm_store_memory_append_event(store, &audit_ev, NULL);
        }
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "status", "rejected");
        yyjson_mut_obj_add_str(doc, root, "policy_decision", policy_decision);
        yyjson_mut_obj_add_str(doc, root, "policy_reason", policy_reason ? policy_reason : "");
        yyjson_mut_obj_add_str(doc, root, "hot_path",
                               "write policy rejected; audit event written to memory_event");
        char *json = yy_doc_to_str(doc);
        yyjson_mut_doc_free(doc);
        char *result = cbm_mcp_text_result(json, false);
        free(json);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return result;
    }
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return _res;
    }
    cbm_memory_event_t event = {0};
    event.type = type ? type : "memory.event";
    event.source = source ? source : "mcp.events";
    event.project = project;
    event.user = user;
    event.payload = payload;
    event.confidence = confidence;
    event.context_json = context_json ? context_json : "{}";
    char *event_id = NULL;
    /* Event + structured candidate must persist atomically. Wrap both in one
     * transaction so a crash between them can't leave an orphan event row with
     * no corresponding memory_item. The transaction lives at this business-op
     * layer (not inside the store append fns) because those fns are also called
     * standalone elsewhere — nesting a BEGIN inside them would fail. */
    if (cbm_store_begin(store) != CBM_STORE_OK) {
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to begin memory transaction", true);
    }
    if (cbm_store_memory_append_event(store, &event, &event_id) != CBM_STORE_OK) {
        cbm_store_rollback(store);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to append memory event", true);
    }
    char source_ids[CBM_SZ_256];
    snprintf(source_ids, sizeof(source_ids), "[\"%s\"]", event_id ? event_id : "");
    cbm_memory_item_t item = {0};
    item.kind = kind ? kind : "event";
    /* P0-a: ADR identity — decision and constraint memories default to layer "adr"
     * instead of "episodic" so they are fetchable, rankable, and decay-tunable as a
     * distinct class. An explicit layer argument always wins. */
    item.layer = layer ? layer
                       : ((kind && (strcmp(kind, "decision") == 0 ||
                                    strcmp(kind, "constraint") == 0))
                              ? "adr"
                              : "episodic");
    /* P0-b: for decision-class items without a title, derive one from the summary
     * (first sentence, up to CBM_SZ_128 chars). Summary always carries the query-like
     * conclusion so it makes a far better display label than a NULL fallback. */
    char *derived_title = NULL;
    item.title = title;
    if (!item.title && summary && summary[0] && kind &&
        (strcmp(kind, "decision") == 0 || strcmp(kind, "constraint") == 0)) {
        char title_buf[CBM_SZ_128];
        int tl = 0;
        while (summary[tl] && summary[tl] != '\n' && tl < (int)sizeof(title_buf) - 1) {
            title_buf[tl] = summary[tl];
            tl++;
        }
        /* Never cut inside a UTF-8 sequence: locate the lead byte of the final
         * character and drop it if its sequence is incomplete. A mid-character
         * cut leaves invalid UTF-8 in the stored title, which later makes the
         * JSON-RPC envelope's strict re-parse drop the whole result (client
         * hangs waiting for a response that never validates). */
        {
            int lead = tl;
            while (lead > 0 && ((unsigned char)title_buf[lead - 1] & 0xC0) == 0x80) {
                lead--;
            }
            if (lead > 0 && (unsigned char)title_buf[lead - 1] >= 0xC0) {
                unsigned char lb = (unsigned char)title_buf[lead - 1];
                int need = (lb >= 0xF0) ? 4 : (lb >= 0xE0) ? 3 : 2;
                if (tl - (lead - 1) < need) {
                    tl = lead - 1;
                }
            }
        }
        /* Trim trailing punctuation so the label reads cleanly. */
        while (tl > 0 && (title_buf[tl - 1] == '.' ||
                          title_buf[tl - 1] == '!')) {
            tl--;
        }
        title_buf[tl] = '\0';
        derived_title = cbm_strdup(title_buf);
        item.title = derived_title;
    }
    item.summary = summary ? summary : (content ? content : payload);
    item.content = content ? content : payload;
    item.scope_user = user;
    /* Global memories carry no project scope (scope_project=NULL) so they read
     * back from every project; memory_infer_entity then classifies them as
     * global/user. Project-scoped writes keep the resolved project name. */
    item.scope_project = is_global ? NULL : project;
    item.scope_task = task;
    item.entity_key = entity_key;
    item.predicate = predicate;
    item.importance = importance;
    item.confidence = confidence;
    item.reusability = reusability;
    item.specificity = specificity;
    item.status = "candidate";
    /* Version increment: when supersedes is set, query the superseded item's version
     * and set this item's version = old_version + 1, giving the ADR timeline a
     * naturally ascending sequence. If the old item is not found (deleted, different
     * scope, etc.), start fresh at version 1 and surface a warning. */
    item.version = 1;
    bool supersedes_found = false;
    const char *supersedes_warning = NULL;
    if (supersedes && supersedes[0]) {
        sqlite3_stmt *ver_stmt = NULL;
        if (sqlite3_prepare_v2(cbm_store_get_db(store),
                               "SELECT version FROM memory_item WHERE id=?1 AND deleted_at IS NULL;",
                               -1, &ver_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ver_stmt, 1, supersedes, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(ver_stmt) == SQLITE_ROW) {
                item.version = sqlite3_column_int(ver_stmt, 0) + 1;
                supersedes_found = true;
            }
            sqlite3_finalize(ver_stmt);
        }
        if (!supersedes_found) {
            supersedes_warning =
                "supersedes target not found: the referenced item_id does not exist "
                "or has been soft-deleted. Version set to 1; supersedes link recorded "
                "but may be dangling.";
        }
    }
    item.supersedes = supersedes; /* P3-d: NULL unless this ADR replaces an earlier one */
    item.source_event_ids = source_ids;
    char *item_id = NULL;
    int item_rc = cbm_store_memory_append_candidate(store, &item, &item_id);

    /* Atomic: if the candidate write fails, roll back the event too so we never
     * persist an orphan event. Otherwise commit both together. */
    if (item_rc != CBM_STORE_OK) {
        cbm_store_rollback(store);
        free(event_id);
        free(item_id);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to append memory candidate", true);
    }

    /* Explicit code anchoring: an optional "about_code" array of qualified_names
     * links this memory to code symbols (about_code edges). Written inside the
     * same transaction as the candidate so anchoring is atomic with the write.
     * Unknown qns are allowed — the symbol may be indexed later, and recall does
     * a lazy existence check anyway. */
    if (item_id) {
        for (size_t ai = 0; ai < about_code_n; ai++) {
            if (about_code_qns[ai]) {
                (void)cbm_store_memory_link_code(store, item_id, about_code_qns[ai], "user");
            }
        }
    }

    /* Confidence/reusability via the consolidated 3-tier composition
     * (cbm_memory_score_item, memory_store.c): L1 graph signal from about_code
     * anchors ⊕ L2 kind prior ⊕ L3 declared offset. Runs for EVERY write so an
     * unanchored item still gets its kind baseline; the composition is monotonic
     * (a tier only raises), so an anchored low-degree ADR keeps at least its kind
     * prior instead of scoring below an unanchored one and decaying out first.
     * The graph is borrowed only when anchors exist (NULL for pure-memory
     * projects → no L1, never an error). The result stays a LIVE value a later
     * falsification (memory_feedback/supersede/decay) can pull down. */
    if (item_id) {
        int resolved = 0;
        double l1_conf = 0.0;
        double l1_reuse = 0.0;
        if (about_code_n > 0) {
            cbm_store_t *graph = resolve_store(srv, project);
            sqlite3 *graph_db = graph ? cbm_store_get_db(graph) : NULL;
            if (graph_db) {
                resolved = cbm_store_memory_score_from_anchors(store, graph_db, item_id, project,
                                                               &l1_conf, &l1_reuse);
            }
        }
        cbm_memory_score_t sc = cbm_memory_score_item(kind ? kind : "event", resolved, l1_conf,
                                                      l1_reuse, confidence, reusability);
        sqlite3_stmt *up = NULL;
        if (sqlite3_prepare_v2(cbm_store_get_db(store),
                               "UPDATE memory_item SET confidence=?1,reusability=?2 WHERE id=?3;",
                               -1, &up, NULL) == SQLITE_OK) {
            sqlite3_bind_double(up, 1, sc.confidence);
            sqlite3_bind_double(up, 2, sc.reusability);
            sqlite3_bind_text(up, 3, item_id, -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(up);
            sqlite3_finalize(up);
        }
    }

    /* P4 recall=latest-link: an explicit `supersedes` retires the old ADR from
     * the recall mainline. The merge path already archives the item it retires;
     * the explicit-supersede path must do the same, or the superseded ADR stays
     * active and both versions surface. Archive (not delete) the target: it
     * leaves recall but stays on disk as history, still shielded by the purge
     * red line. Scope-guarded; only an active/candidate target is touched.
     * Direct UPDATE (not cbm_store_memory_update_status) because we are already
     * inside this handler's transaction and that helper opens its own. */
    if (supersedes && supersedes[0]) {
        sqlite3_stmt *sup = NULL;
        int arch_changed = 0;
        if (sqlite3_prepare_v2(
                cbm_store_get_db(store),
                "UPDATE memory_item SET status='archived', updated_at=?1 "
                "WHERE id=?2 AND scope_project=?3 AND status IN ('active','candidate');",
                -1, &sup, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(sup, 1, (int64_t)time(NULL) * 1000);
            sqlite3_bind_text(sup, 2, supersedes, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sup, 3, project, -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(sup);
            arch_changed = sqlite3_changes(cbm_store_get_db(store));
            sqlite3_finalize(sup);
        }
        if (arch_changed == 0 && supersedes_found) {
            /* Target exists but could not be archived — already archived, wrong
             * status, or scope mismatch. Only report when the target was found
             * above; if it was never found, the primary warning already covers it. */
            supersedes_warning =
                "supersedes target found but could not be archived "
                "(status may not be active/candidate — already archived or retracted)";
        }
    }

    if (cbm_store_commit(store) != CBM_STORE_OK) {
        cbm_store_rollback(store);
        free(event_id);
        free(item_id);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to commit memory transaction", true);
    }

    /* Lazy auto-maintenance: a single-user agent has no operator to call
     * admin_consolidate/admin_decay, so the write hot path opportunistically
     * triggers them when due. Runs AFTER commit (new candidate is visible and
     * no transaction is open). Best-effort — never fails the write. */
    cbm_memory_maintain_report_t maint = {0};
    (void)cbm_store_memory_maintain_if_due(store, project, &maint);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "accepted");
    yyjson_mut_obj_add_str(doc, root, "policy_decision", policy_decision);
    yyjson_mut_obj_add_str(doc, root, "policy_reason", policy_reason ? policy_reason : "");
    yyjson_mut_obj_add_str(doc, root, "event_id", event_id ? event_id : "");
    yyjson_mut_obj_add_str(doc, root, "item_id", item_id ? item_id : "");
    yyjson_mut_obj_add_str(doc, root, "item_status", "candidate");
    /* P3-b anchoring dimension (HELPER, not gatekeeper): report whether this
     * memory got a code anchor, and for high-value decision-class kinds written
     * WITHOUT one, advise (never block) — an unanchored decision misses L1 graph
     * scoring and anchor-boost at recall, and can't ride the ADR↔code lifecycle.
     * Pure-memory projects legitimately have no graph, so this stays advice. */
    yyjson_mut_obj_add_bool(doc, root, "anchored", about_code_n > 0);
    if (about_code_n == 0 && kind &&
        (strcmp(kind, "decision") == 0 || strcmp(kind, "constraint") == 0 ||
         strcmp(kind, "lesson") == 0)) {
        yyjson_mut_obj_add_str(
            doc, root, "anchoring_advice",
            "no about_code anchor: this decision-class memory won't get graph-derived "
            "confidence/reusability or recall anchor-boost. If it concerns specific code, "
            "pass about_code=[\"<qualified_name>\", ...] so it anchors to the graph.");
    }
    /* P3-d structure dimension: loose ADR-structure nudge (advice only). */
    const char *struct_advice = memory_structure_advice(kind, content);
    if (struct_advice) {
        yyjson_mut_obj_add_str(doc, root, "structure_advice", struct_advice);
    }
    /* Supersedes chain integrity: warn when a supersedes target is missing or
     * could not be archived (see the version-query and archive-UPDATE blocks above). */
    if (supersedes_warning) {
        yyjson_mut_obj_add_str(doc, root, "supersedes_warning", supersedes_warning);
    }
    yyjson_mut_obj_add_bool(doc, root, "maintained", maint.consolidated || maint.decayed);
    if (maint.consolidated) {
        yyjson_mut_obj_add_int(doc, root, "consolidated", maint.consolidate_count);
    }
    if (maint.decayed) {
        yyjson_mut_obj_add_int(doc, root, "decayed", maint.decay_count);
    }
    yyjson_mut_obj_add_str(
        doc, root, "hot_path",
        "event+structured candidate only; consolidation builds dedup, vectors, and evidence edges");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    free(event_id);
    free(item_id);
    free(project);
    free(type);
    free(scope);
    free(source);
    free(user);
    free(task);
    free(kind);
    free(layer);
    free(title);
    free(derived_title);
    free(summary);
    free(entity_key);
    free(predicate);
    free(payload);
    free(content);
    free(supersedes);
    free(context_json);
    free_anchor_qns(about_code_qns, about_code_n);
    return result;
}
/* Scope-aware downweight for global (cross-project) memories in a project-scoped
 * recall: their retrieval_score is multiplied by this before merging with the
 * project store. <1.0 so project-specific hits win the limited top-K; >0 so a
 * strongly-relevant global memory can still surface. Tunable. */
#define MEMORY_GLOBAL_SCOPE_WEIGHT 0.5

/* qsort comparator: order memory item pointers by retrieval_score descending,
 * used to merge project-store and global-store result sets into one ranked list. */
static int memory_item_ptr_score_desc(const void *a, const void *b) {
    const cbm_memory_item_t *ia = *(const cbm_memory_item_t *const *)a;
    const cbm_memory_item_t *ib = *(const cbm_memory_item_t *const *)b;
    if (ia->retrieval_score < ib->retrieval_score)
        return 1;
    if (ia->retrieval_score > ib->retrieval_score)
        return -1;
    return 0;
}

char *handle_memories_retrieve(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    /* Collapse a phantom "<project>-memory" name onto the real project so both
     * the store handle and the scope_project SQL filter use the canonical name. */
    {
        char *canon = normalize_phantom_project(project);
        if (canon) {
            free(project);
            project = canon;
        }
    }
    cbm_store_t *store = resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    /* Lazy auto-maintenance before reading, so a single-user agent sees freshly
     * consolidated/decayed state without ever calling admin endpoints. At the
     * entry point no transaction is open. Best-effort — never fails the read. */
    (void)cbm_store_memory_maintain_if_due(store, project, NULL);
    cbm_memory_query_t query = {0};
    query.project = project;
    query.user = cbm_mcp_get_string_arg(args, "user");
    query.task = cbm_mcp_get_string_arg(args, "task");
    query.entity_key = cbm_mcp_get_string_arg(args, "entity_key");
    query.kind = cbm_mcp_get_string_arg(args, "kind");
    query.query = cbm_mcp_get_string_arg(args, "query");
    query.code_context = cbm_mcp_get_string_arg(args, "code_context");
    query.include_inactive = cbm_mcp_get_bool_arg(args, "include_inactive");
    query.limit = cbm_mcp_get_int_arg(args, "limit", 10);
    /* Anchor-boost (about_code) needs the code graph, which now lives in a
     * separate DB. Borrow the project's graph handle only when a code_context is
     * given; absence/unindexed graph degrades to "no boost", never an error. */
    if (query.code_context && query.code_context[0]) {
        cbm_store_t *graph = resolve_store(srv, project);
        if (graph) {
            query.graph_db = cbm_store_get_db(graph);
        }
    }
    cbm_memory_result_t out = {0};
    int rc = cbm_store_memory_retrieve(store, &query, &out);
    if (rc == CBM_STORE_OK && out.count > 0) {
        const char **ids = calloc((size_t)out.count, sizeof(char *));
        if (ids) {
            for (int i = 0; i < out.count; i++)
                ids[i] = out.items[i].id;
            (void)cbm_store_memory_mark_hits(store, ids, out.count, 0);
            free(ids);
        }
    }

    /* Union the global (cross-project) store: scope_project=NULL memories live
     * in __global__-memory.db and must surface from every project. Query it with
     * project=NULL (the global rows have no project scope) and merge by score.
     * Purely additive — only runs if the global store exists; never gates or
     * errors the project read, and the project-required guard above still
     * protects against mistyped project names. */
    cbm_memory_result_t gout = {0};
    bool have_global = false;
    /* Test/isolation switch: CBM_MEMORY_NO_GLOBAL_UNION=1 skips the global union
     * so a project-scoped recall measures ONLY the project store. The recall eval
     * sets this to stay deterministic (its baseline predates the global store);
     * production leaves it unset so global memories surface everywhere. */
    char no_union[8];
    cbm_safe_getenv("CBM_MEMORY_NO_GLOBAL_UNION", no_union, sizeof(no_union), NULL);
    cbm_store_t *gstore = (no_union[0] == '1') ? NULL : resolve_global_memory_store(srv, false);
    if (gstore) {
        (void)cbm_store_memory_maintain_if_due(gstore, CBM_GLOBAL_MEMORY_PROJECT, NULL);
        cbm_memory_query_t gquery = query;
        gquery.project = NULL; /* global rows carry scope_project=NULL */
        if (cbm_store_memory_retrieve(gstore, &gquery, &gout) == CBM_STORE_OK) {
            have_global = true;
            /* Scope-aware downweight (B): global memories are cross-project,
             * project-agnostic public info, AND are already injected every turn
             * by the recall hook. So in a PROJECT-scoped query they must not
             * compete head-to-head for the limited top-K — multiply their score
             * by MEMORY_GLOBAL_SCOPE_WEIGHT so a project-specific hit wins, while
             * a genuinely dominant global hit can still surface. */
            for (int i = 0; i < gout.count; i++)
                gout.items[i].retrieval_score *= MEMORY_GLOBAL_SCOPE_WEIGHT;
            if (gout.count > 0) {
                const char **gids = calloc((size_t)gout.count, sizeof(char *));
                if (gids) {
                    for (int i = 0; i < gout.count; i++)
                        gids[i] = gout.items[i].id;
                    (void)cbm_store_memory_mark_hits(gstore, gids, gout.count, 0);
                    free(gids);
                }
            }
        }
    }

    /* Build one ranked pointer list across both stores, sorted by retrieval
     * score, truncated to the requested limit. Pointers borrow item storage
     * from `out`/`gout`; both are freed after the JSON is serialized. */
    int merged_total = out.total + (have_global ? gout.total : 0);
    int combined = out.count + (have_global ? gout.count : 0);
    const cbm_memory_item_t **ranked =
        combined > 0 ? calloc((size_t)combined, sizeof(*ranked)) : NULL;
    int nranked = 0;
    if (ranked) {
        for (int i = 0; i < out.count; i++)
            ranked[nranked++] = &out.items[i];
        if (have_global)
            for (int i = 0; i < gout.count; i++)
                ranked[nranked++] = &gout.items[i];
        qsort(ranked, (size_t)nranked, sizeof(*ranked), memory_item_ptr_score_desc);
        if (query.limit > 0 && nranked > query.limit)
            nranked = query.limit;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "total", merged_total);
    yyjson_mut_obj_add_int(doc, root, "count", nranked);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < nranked; i++)
        yyjson_mut_arr_add_val(arr, memory_item_to_json(doc, ranked[i]));
    yyjson_mut_obj_add_val(doc, root, "memories", arr);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(ranked);
    cbm_store_memory_result_free(&out);
    cbm_store_memory_result_free(&gout);
    free(project);
    free((char *)query.user);
    free((char *)query.task);
    free((char *)query.entity_key);
    free((char *)query.kind);
    free((char *)query.query);
    free((char *)query.code_context);
    return result;
}

char *handle_memories_inspect(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    bool is_global = scope && strcmp(scope, "global") == 0;
    free(scope);
    /* scope='global' inspects the cross-project store (scope_project=NULL rows);
     * otherwise the per-project store. The SQL below binds the matching scope. */
    cbm_store_t *store = is_global ? resolve_global_memory_store(srv, false)
                                   : resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error(is_global ? "no global memories yet"
                                                        : "project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    char *status = cbm_mcp_get_string_arg(args, "status");
    int limit = cbm_mcp_get_int_arg(args, "limit", 50);

    const char *cols = "m.id,m.entity_key,m.predicate,m.status,m.kind,m.layer,"
                       "m.title,m.hit_count,m.last_hit_at,m.confidence,m.version,m.updated_at";
    char sql[CBM_SZ_2K];
    snprintf(sql, sizeof(sql),
             "SELECT %s FROM memory_item m WHERE (?1 IS NULL OR m.scope_project=?1) "
             "AND (?2 IS NULL OR m.status=?2) "
             "ORDER BY m.updated_at DESC LIMIT ?3;",
             cols);
    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        free(project);
        free(status);
        return cbm_mcp_text_result("inspect query failed", true);
    }
    /* Global inspect binds NULL for the project filter (matches scope_project=NULL
     * rows); project inspect binds the project name. */
    if (is_global) {
        sqlite3_bind_null(stmt, 1);
    } else {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    }
    if (status && status[0]) {
        sqlite3_bind_text(stmt, 2, status, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 2);
    }
    sqlite3_bind_int(stmt, 3, limit > 0 ? limit : 50);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < limit) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, obj, "id", (const char *)sqlite3_column_text(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc, obj, "entity_key",
                                  (const char *)sqlite3_column_text(stmt, 1));
        yyjson_mut_obj_add_strcpy(doc, obj, "predicate",
                                  (const char *)sqlite3_column_text(stmt, 2));
        yyjson_mut_obj_add_strcpy(doc, obj, "status", (const char *)sqlite3_column_text(stmt, 3));
        yyjson_mut_obj_add_strcpy(doc, obj, "kind", (const char *)sqlite3_column_text(stmt, 4));
        yyjson_mut_obj_add_strcpy(doc, obj, "layer", (const char *)sqlite3_column_text(stmt, 5));
        yyjson_mut_obj_add_strcpy(doc, obj, "title", (const char *)sqlite3_column_text(stmt, 6));
        yyjson_mut_obj_add_int(doc, obj, "hit_count", sqlite3_column_int(stmt, 7));
        yyjson_mut_obj_add_int(doc, obj, "last_hit_at", sqlite3_column_int64(stmt, 8));
        yyjson_mut_obj_add_real(doc, obj, "confidence", sqlite3_column_double(stmt, 9));
        yyjson_mut_obj_add_int(doc, obj, "version", sqlite3_column_int(stmt, 10));
        yyjson_mut_obj_add_int(doc, obj, "updated_at", sqlite3_column_int64(stmt, 11));
        yyjson_mut_arr_add_val(arr, obj);
        n++;
    }
    yyjson_mut_obj_add_val(doc, root, "items", arr);
    yyjson_mut_obj_add_int(doc, root, "count", n);
    sqlite3_finalize(stmt);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    free(project);
    free(status);
    return result;
}

char *handle_memory_update_status(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *id = cbm_mcp_get_string_arg(args, "id");
    char *status = cbm_mcp_get_string_arg(args, "status");
    if (!project || !id || !status) {
        free(project);
        free(id);
        free(status);
        return cbm_mcp_text_result("project, id, and status are required", true);
    }
    cbm_store_t *store = open_existing_memory_store_for_write(srv, project);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(id);
        free(status);
        return _res;
    }
    int rc = cbm_store_memory_update_status(store, id, project, status);
    /* By-id ops are scope-guarded on project, so a global memory (scope_project
     * =NULL) is NOT_FOUND in the project store. Fall back to the global store
     * with project=NULL, which its (?4 IS NULL OR scope_project=?4) clause accepts. */
    if (rc == CBM_STORE_NOT_FOUND) {
        cbm_store_t *gstore = open_existing_global_memory_store_for_write(srv);
        if (gstore)
            rc = cbm_store_memory_update_status(gstore, id, NULL, status);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "status",
                           rc == CBM_STORE_NOT_FOUND ? "not_found"
                                                     : (rc == CBM_STORE_OK ? "updated" : "error"));
    yyjson_mut_obj_add_str(doc, root, "item_status", status);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(project);
    free(id);
    free(status);
    return result;
}

char *handle_memory_feedback(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *id = cbm_mcp_get_string_arg(args, "id");
    char *feedback = cbm_mcp_get_string_arg(args, "feedback");
    char *note = cbm_mcp_get_string_arg(args, "note");
    char *user = cbm_mcp_get_string_arg(args, "user");
    if (!project || !id || !feedback) {
        free(project);
        free(id);
        free(feedback);
        free(note);
        free(user);
        return cbm_mcp_text_result("project, id, and feedback are required", true);
    }
    cbm_store_t *store = open_existing_memory_store_for_write(srv, project);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(id);
        free(feedback);
        free(note);
        free(user);
        return _res;
    }
    char *event_id = NULL;
    int rc = cbm_store_memory_feedback(store, id, project, feedback, note, user, &event_id);
    /* Global-memory fallback: by-id feedback is scope-guarded, retry the global
     * store with project=NULL when the project store reports NOT_FOUND. */
    if (rc == CBM_STORE_NOT_FOUND) {
        cbm_store_t *gstore = open_existing_global_memory_store_for_write(srv);
        if (gstore)
            rc = cbm_store_memory_feedback(gstore, id, NULL, feedback, note, user, &event_id);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "feedback", feedback);
    yyjson_mut_obj_add_str(doc, root, "status",
                           rc == CBM_STORE_NOT_FOUND ? "not_found"
                                                     : (rc == CBM_STORE_OK ? "recorded" : "error"));
    yyjson_mut_obj_add_str(doc, root, "event_id", event_id ? event_id : "");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(event_id);
    free(project);
    free(id);
    free(feedback);
    free(note);
    free(user);
    return result;
}

char *handle_memory_delete(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *id = cbm_mcp_get_string_arg(args, "id");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *user = cbm_mcp_get_string_arg(args, "user");
    if (!project || !id) {
        free(project);
        free(id);
        free(mode);
        free(user);
        return cbm_mcp_text_result("project and id are required", true);
    }
    const char *m = (mode && mode[0]) ? mode : "soft";
    cbm_store_t *store = open_existing_memory_store_for_write(srv, project);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(id);
        free(mode);
        free(user);
        return _res;
    }
    int rc;
    const char *ok_status;
    if (strcmp(m, "restore") == 0) {
        rc = cbm_store_memory_restore(store, id, project, user);
        ok_status = "restored";
    } else if (strcmp(m, "soft") == 0) {
        rc = cbm_store_memory_delete(store, id, project, m, user);
        ok_status = "soft_deleted";
    } else {
        /* hard / purge */
        rc = cbm_store_memory_delete(store, id, project, m, user);
        ok_status = "deleted";
    }
    /* Global-memory fallback: by-id delete/restore is scope-guarded on project,
     * so retry the global store with project=NULL on NOT_FOUND. */
    if (rc == CBM_STORE_NOT_FOUND) {
        cbm_store_t *gstore = open_existing_global_memory_store_for_write(srv);
        if (gstore) {
            if (strcmp(m, "restore") == 0)
                rc = cbm_store_memory_restore(gstore, id, NULL, user);
            else
                rc = cbm_store_memory_delete(gstore, id, NULL, m, user);
        }
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "mode", m);
    yyjson_mut_obj_add_str(doc, root, "status",
                           rc == CBM_STORE_NOT_FOUND ? "not_found"
                                                     : (rc == CBM_STORE_OK ? ok_status : "error"));
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK && rc != CBM_STORE_NOT_FOUND);
    free(json);
    free(project);
    free(id);
    free(mode);
    free(user);
    return result;
}

char *handle_admin_consolidate(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    bool is_global = scope && strcmp(scope, "global") == 0;
    free(scope);
    /* scope='global' consolidates the cross-project store; pass project=NULL so
     * the store-level filter covers all of its (scope_project=NULL) rows. */
    const char *scope_arg = is_global ? NULL : project;
    cbm_store_t *store = is_global ? open_existing_global_memory_store_for_write(srv)
                                   : open_existing_memory_store_for_write(srv, project);
    if (!store) {
        char *_err = build_project_list_error(is_global ? "no global memories yet"
                                                        : "project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    int processed = 0;
    int rc = cbm_store_memory_consolidate(store, scope_arg, cbm_mcp_get_int_arg(args, "limit", 100),
                                          &processed);
    /* Rebuild the FTS index with current CJK segmentation so memories indexed
     * before bigram segmentation existed become searchable in Chinese. Pass
     * skip_reindex_fts=true to skip on large stores where the rebuild is costly. */
    int reindexed = 0;
    if (!cbm_mcp_get_bool_arg(args, "skip_reindex_fts")) {
        (void)cbm_store_memory_reindex_fts(store, scope_arg, &reindexed);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "processed", processed);
    yyjson_mut_obj_add_int(doc, root, "fts_reindexed", reindexed);
    yyjson_mut_obj_add_str(doc, root, "mode", "deterministic_mvp_pass");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(project);
    return result;
}

char *handle_admin_decay(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    bool is_global = scope && strcmp(scope, "global") == 0;
    free(scope);
    const char *scope_arg = is_global ? NULL : project;
    cbm_store_t *store = is_global ? open_existing_global_memory_store_for_write(srv)
                                   : open_existing_memory_store_for_write(srv, project);
    if (!store) {
        char *_err = build_project_list_error(is_global ? "no global memories yet"
                                                        : "project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    int processed = 0;
    int rc = cbm_store_memory_decay(store, scope_arg, cbm_mcp_get_int_arg(args, "limit", 100),
                                    &processed);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "processed", processed);
    yyjson_mut_obj_add_str(doc, root, "formula", "age_days/30 * (1-confidence) * (1-reusability)");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(project);
    return result;
}
char *handle_memory_health(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    bool is_global = scope && strcmp(scope, "global") == 0;
    free(scope);
    const char *scope_arg = is_global ? NULL : project;
    cbm_store_t *store = is_global ? resolve_global_memory_store(srv, false)
                                   : resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error(is_global ? "no global memories yet"
                                                        : "project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    cbm_memory_health_t h = {0};
    int rc = cbm_store_memory_health(store, scope_arg, &h);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "events", h.event_count);
    yyjson_mut_obj_add_int(doc, root, "items", h.item_count);
    yyjson_mut_obj_add_int(doc, root, "edges", h.edge_count);
    yyjson_mut_obj_add_int(doc, root, "candidates", h.candidate_count);
    yyjson_mut_obj_add_int(doc, root, "active", h.active_count);
    yyjson_mut_obj_add_int(doc, root, "deprecated", h.deprecated_count);
    yyjson_mut_obj_add_int(doc, root, "archived", h.archived_count);
    yyjson_mut_obj_add_int(doc, root, "retracted", h.retracted_count);
    yyjson_mut_obj_add_int(doc, root, "deleted", h.deleted_count);
    yyjson_mut_obj_add_int(doc, root, "total_hits", h.total_hits);
    yyjson_mut_obj_add_int(doc, root, "conflicts", h.conflict_count);
    yyjson_mut_obj_add_int(doc, root, "scopes", h.scope_count);
    yyjson_mut_obj_add_real(doc, root, "hit_rate", h.hit_rate);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(project);
    return result;
}

/* P1: handle_adr_list — structured ADR index for human browsing and tool use.
 * Mirrors the MEMORY.md concept: a browsable, filterable list of architectural
 * decisions instead of an opaque full-text-retrieval-only store.
 *
 * Takes cbm_mcp_server_t *srv for API compatibility but does NOT access its
 * internals — opens memory stores independently via the public cbm_store_* API. */

char *handle_adr_list(cbm_mcp_server_t *srv, const char *args) {
    (void)srv; /* opaque — never accessed */

    yyjson_doc *adoc = yyjson_read(args ? args : "{}", args ? strlen(args) : CBM_SZ_2, 0);
    if (!adoc)
        return cbm_mcp_text_result("invalid JSON arguments", true);

    char *project = memory_arg_string_dup(adoc, "project");
    char *kind = memory_arg_string_dup(adoc, "kind");
    char *status = memory_arg_string_dup(adoc, "status");
    char *entity_key = memory_arg_string_dup(adoc, "entity_key");
    int limit = 50;
    {
        yyjson_val *lv = yyjson_obj_get(yyjson_doc_get_root(adoc), "limit");
        if (lv && yyjson_is_int(lv)) {
            int v = (int)yyjson_get_int(lv);
            if (v > 0 && v <= 200)
                limit = v;
        }
    }
    yyjson_doc_free(adoc);

    if (!project) {
        free(kind);
        free(status);
        free(entity_key);
        return cbm_mcp_text_result("project is required", true);
    }

    /* Normalize phantom names so adr_list works on "-memory" aliases. */
    {
        char *canon = normalize_phantom_project(project);
        if (canon) {
            free(project);
            project = canon;
        }
    }

    cbm_store_t *store = open_memory_store_for_project(project);
    if (!store) {
        char *res = cbm_mcp_text_result(
            "project not found or not indexed", true);
        free(project);
        free(kind);
        free(status);
        free(entity_key);
        return res;
    }

    char *json = NULL;
    int rc = cbm_store_memory_adr_list(store, project, kind, status, entity_key, limit, &json);

    /* Union with the global (cross-project) store — same semantics as
     * handle_memories_retrieve. Global ADRs carry scope_project=NULL and
     * should surface from every project's adr_list. Both result sets are
     * parsed, merged by composite score, and truncated to limit. */
    char *gjson = NULL;
    {
        char gmem_path[CBM_SZ_1K];
        cbm_store_t *gstore = NULL;
        if (cbm_memory_db_path(CBM_GLOBAL_MEMORY_PROJECT, gmem_path,
                               sizeof(gmem_path)) == CBM_STORE_OK &&
            cbm_file_exists(gmem_path)) {
            gstore = cbm_store_open_path_query(gmem_path);
        }
        if (gstore && rc == CBM_STORE_OK && json) {
            int grc = cbm_store_memory_adr_list_global(gstore, kind, status, entity_key, limit,
                                                        &gjson);
            if (grc == CBM_STORE_OK && gjson && gjson[0]) {
                yyjson_doc *pdoc = yyjson_read(json, strlen(json), 0);
                yyjson_doc *gdoc = yyjson_read(gjson, strlen(gjson), 0);
                if (pdoc && gdoc) {
                    yyjson_val *proot = yyjson_doc_get_root(pdoc);
                    yyjson_val *groot = yyjson_doc_get_root(gdoc);
                    yyjson_val *pitems = yyjson_obj_get(proot, "items");
                    yyjson_val *gitems = yyjson_obj_get(groot, "items");
                    int ptotal = 0, gtotal = 0;
                    yyjson_val *ptv = yyjson_obj_get(proot, "total");
                    yyjson_val *gtv = yyjson_obj_get(groot, "total");
                    if (ptv && yyjson_is_int(ptv)) ptotal = (int)yyjson_get_int(ptv);
                    if (gtv && yyjson_is_int(gtv)) gtotal = (int)yyjson_get_int(gtv);

                    /* Collect item pointers (raw immutable values) with their
                     * composite scores, sort, then field-copy the top-N into
                     * a fresh mutable doc. Avoids yyjson deep-copy ownership
                     * complications while preserving correct score ordering. */
                    struct { const yyjson_val *val; double score; } *items = NULL;
                    int nitems = 0, icap = 0;
                    if (pitems && yyjson_is_arr(pitems)) {
                        size_t pi, pmax; yyjson_val *pv;
                        yyjson_arr_foreach(pitems, pi, pmax, pv) {
                            if (nitems == icap) {
                                icap = icap ? icap * 2 : 64;
                                items = realloc(items, (size_t)icap * sizeof(*items));
                            }
                            if (items) {
                                yyjson_val *imp = yyjson_obj_get(pv, "importance");
                                yyjson_val *con = yyjson_obj_get(pv, "confidence");
                                yyjson_val *reu = yyjson_obj_get(pv, "reusability");
                                yyjson_val *spe = yyjson_obj_get(pv, "specificity");
                                yyjson_val *hc  = yyjson_obj_get(pv, "hit_count");
                                yyjson_val *dec = yyjson_obj_get(pv, "decay");
                                items[nitems].val = pv;
                                items[nitems].score =
                                    (imp && yyjson_is_num(imp) ? yyjson_get_real(imp) : 0.0) +
                                    (con && yyjson_is_num(con) ? yyjson_get_real(con) : 0.0) +
                                    (reu && yyjson_is_num(reu) ? yyjson_get_real(reu) : 0.0) +
                                    (spe && yyjson_is_num(spe) ? yyjson_get_real(spe) : 0.0) +
                                    (hc  && yyjson_is_int(hc)  ? (double)yyjson_get_int(hc) : 0.0) -
                                    (dec && yyjson_is_num(dec) ? yyjson_get_real(dec) : 0.0);
                                nitems++;
                            }
                        }
                    }
                    if (gitems && yyjson_is_arr(gitems)) {
                        size_t gi, gmax; yyjson_val *gv;
                        yyjson_arr_foreach(gitems, gi, gmax, gv) {
                            if (nitems == icap) {
                                icap = icap ? icap * 2 : 64;
                                items = realloc(items, (size_t)icap * sizeof(*items));
                            }
                            if (items) {
                                yyjson_val *imp = yyjson_obj_get(gv, "importance");
                                yyjson_val *con = yyjson_obj_get(gv, "confidence");
                                yyjson_val *reu = yyjson_obj_get(gv, "reusability");
                                yyjson_val *spe = yyjson_obj_get(gv, "specificity");
                                yyjson_val *hc  = yyjson_obj_get(gv, "hit_count");
                                yyjson_val *dec = yyjson_obj_get(gv, "decay");
                                items[nitems].val = gv;
                                items[nitems].score =
                                    (imp && yyjson_is_num(imp) ? yyjson_get_real(imp) : 0.0) +
                                    (con && yyjson_is_num(con) ? yyjson_get_real(con) : 0.0) +
                                    (reu && yyjson_is_num(reu) ? yyjson_get_real(reu) : 0.0) +
                                    (spe && yyjson_is_num(spe) ? yyjson_get_real(spe) : 0.0) +
                                    (hc  && yyjson_is_int(hc)  ? (double)yyjson_get_int(hc) : 0.0) -
                                    (dec && yyjson_is_num(dec) ? yyjson_get_real(dec) : 0.0);
                                nitems++;
                            }
                        }
                    }
                    /* Bubble sort by score descending (items list is small; limit <= 200). */
                    if (items) {
                        for (int a = 0; a < nitems; a++) {
                            for (int b = a + 1; b < nitems; b++) {
                                if (items[b].score > items[a].score) {
                                    double ts = items[a].score;
                                    items[a].score = items[b].score;
                                    items[b].score = ts;
                                    const yyjson_val *tv = items[a].val;
                                    items[a].val = items[b].val;
                                    items[b].val = tv;
                                }
                            }
                        }
                        int out_n = nitems < limit ? nitems : limit;

                        /* Field-by-field copy from the sorted immutable items into
                         * a single mutable doc — avoids yyjson deep-copy ownership
                         * issues while staying type-safe. */
                        yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
                        yyjson_mut_val *mroot = yyjson_mut_obj(mdoc);
                        yyjson_mut_doc_set_root(mdoc, mroot);
                        yyjson_mut_obj_add_str(mdoc, mroot, "project", project);
                        yyjson_mut_obj_add_int(mdoc, mroot, "total", ptotal + gtotal);
                        yyjson_mut_val *marr = yyjson_mut_arr(mdoc);
                        for (int k = 0; k < out_n; k++) {
                            const yyjson_val *src = items[k].val;
                            yyjson_mut_val *obj = yyjson_mut_obj(mdoc);
#define CP_STR(mdoc, obj, key, src) do { \
    yyjson_val *v = yyjson_obj_get((src), (key)); \
    if (v && yyjson_is_str(v)) yyjson_mut_obj_add_strcpy((mdoc), (obj), (key), yyjson_get_str(v)); \
} while(0)
#define CP_REAL(mdoc, obj, key, src) do { \
    yyjson_val *v = yyjson_obj_get((src), (key)); \
    if (v && yyjson_is_num(v)) yyjson_mut_obj_add_real((mdoc), (obj), (key), yyjson_get_real(v)); \
} while(0)
#define CP_INT(mdoc, obj, key, src) do { \
    yyjson_val *v = yyjson_obj_get((src), (key)); \
    if (v && yyjson_is_int(v)) yyjson_mut_obj_add_int((mdoc), (obj), (key), yyjson_get_sint(v)); \
} while(0)
                            CP_STR(mdoc, obj, "id", src);
                            CP_STR(mdoc, obj, "kind", src);
                            CP_STR(mdoc, obj, "layer", src);
                            CP_STR(mdoc, obj, "title", src);
                            CP_STR(mdoc, obj, "summary", src);
                            CP_STR(mdoc, obj, "entity_key", src);
                            CP_STR(mdoc, obj, "status", src);
                            CP_REAL(mdoc, obj, "importance", src);
                            CP_REAL(mdoc, obj, "confidence", src);
                            CP_REAL(mdoc, obj, "reusability", src);
                            CP_REAL(mdoc, obj, "specificity", src);
                            CP_INT(mdoc, obj, "hit_count", src);
                            CP_REAL(mdoc, obj, "decay", src);
                            CP_INT(mdoc, obj, "version", src);
                            CP_STR(mdoc, obj, "supersedes", src);
                            CP_INT(mdoc, obj, "created_at", src);
                            CP_INT(mdoc, obj, "updated_at", src);
#undef CP_STR
#undef CP_REAL
#undef CP_INT
                            yyjson_mut_arr_add_val(marr, obj);
                        }
                        yyjson_mut_obj_add_val(mdoc, mroot, "items", marr);
                        size_t mlen = 0;
                        char *ms = yyjson_mut_write(mdoc,
                            YYJSON_WRITE_ALLOW_INVALID_UNICODE, &mlen);
                        free(json);
                        json = ms ? strdup(ms) : NULL;
                        free(ms);
                        yyjson_mut_doc_free(mdoc);
                        free(items);
                        rc = json ? CBM_STORE_OK : CBM_STORE_ERR;
                    }
                }
                if (pdoc) yyjson_doc_free(pdoc);
                if (gdoc) yyjson_doc_free(gdoc);
            }
            if (gjson) free(gjson);
        }
        if (gstore) cbm_store_close(gstore);
    }

    cbm_store_close(store);

    free(project);
    free(kind);
    free(status);
    free(entity_key);

    if (rc != CBM_STORE_OK || !json) {
        return cbm_mcp_text_result("failed to query ADR list", true);
    }
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* P3: handle_adr_chain — walk the supersedes chain for an ADR, showing the
 * full version timeline. Start from item_id (walk backward to root, then
 * forward to newest) or entity_key (find root at version=1). Does NOT union
 * the global store — supersedes is project-scoped by design.
 *
 * Takes cbm_mcp_server_t *srv for API compatibility but does NOT access its
 * internals — opens the memory store independently via the public API. */

char *handle_adr_chain(cbm_mcp_server_t *srv, const char *args) {
    (void)srv; /* opaque — never accessed */

    yyjson_doc *adoc = yyjson_read(args ? args : "{}", args ? strlen(args) : CBM_SZ_2, 0);
    if (!adoc)
        return cbm_mcp_text_result("invalid JSON arguments", true);

    char *project = memory_arg_string_dup(adoc, "project");
    char *entity_key = memory_arg_string_dup(adoc, "entity_key");
    char *item_id = memory_arg_string_dup(adoc, "item_id");
    int max_depth = 50;
    {
        yyjson_val *dv = yyjson_obj_get(yyjson_doc_get_root(adoc), "max_depth");
        if (dv && yyjson_is_int(dv)) {
            int v = (int)yyjson_get_int(dv);
            if (v > 0 && v <= 200)
                max_depth = v;
        }
    }
    yyjson_doc_free(adoc);

    if (!project) {
        free(entity_key);
        free(item_id);
        return cbm_mcp_text_result("project is required", true);
    }
    if (!entity_key && !item_id) {
        free(project);
        free(entity_key);
        free(item_id);
        return cbm_mcp_text_result("entity_key or item_id is required", true);
    }

    /* Normalize phantom names. */
    {
        char *canon = normalize_phantom_project(project);
        if (canon) {
            free(project);
            project = canon;
        }
    }

    cbm_store_t *store = open_memory_store_for_project(project);
    if (!store) {
        char *res = cbm_mcp_text_result("project not found or not indexed", true);
        free(project);
        free(entity_key);
        free(item_id);
        return res;
    }

    char *json = NULL;
    int rc = cbm_store_memory_adr_chain(store, project, entity_key, item_id,
                                        max_depth, &json);
    cbm_store_close(store);

    free(project);
    free(entity_key);
    free(item_id);

    if (rc != CBM_STORE_OK || !json) {
        return cbm_mcp_text_result("failed to query ADR chain", true);
    }
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}
