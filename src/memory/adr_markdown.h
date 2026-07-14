/*
 * adr_markdown.h - Deterministic, one-way ADR Markdown projection.
 *
 * SQLite remains authoritative. This API never mutates the memory store and
 * intentionally has no import path.
 */
#ifndef CBM_ADR_MARKDOWN_H
#define CBM_ADR_MARKDOWN_H

#include <stdbool.h>

#include "store/store.h"

typedef enum {
    CBM_ADR_EXPORT_PLAN = 0,
    CBM_ADR_EXPORT_WRITE = 1,
    CBM_ADR_EXPORT_CHECK = 2,
} cbm_adr_export_mode_t;

/* Export project-scoped decision/constraint memories to
 * <repo_path>/.semantic-memory/adr.
 *
 * On success, *out_report_json receives a heap JSON report and *out_clean says
 * whether the on-disk mirror matched the generated projection before any write.
 * The caller frees *out_report_json. `write` applies managed changes; `plan` and
 * `check` are read-only. Returns CBM_STORE_OK or CBM_STORE_ERR. */
int cbm_store_memory_adr_export(cbm_store_t *store, const char *project, const char *repo_path,
                                cbm_adr_export_mode_t mode, char **out_report_json,
                                bool *out_clean);

#endif /* CBM_ADR_MARKDOWN_H */
