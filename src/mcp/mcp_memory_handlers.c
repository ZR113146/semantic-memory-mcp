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

/* ═════════════════════════════════════════════════════════════════════
 *  Public handlers (declared in mcp.h, called from mcp.c dispatch)
 * ═════════════════════════════════════════════════════════════════════ */

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
    if (v && yyjson_is_int(v)) yyjson_mut_obj_add_int((mdoc), (obj), (key), (int64_t)yyjson_get_int(v)); \
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
