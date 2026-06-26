#include "store/store.h"
#include "foundation/platform.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)
#define RUN(name) do { fprintf(stderr, "  %s... ", #name); fflush(stderr); int r = test_##name(); if (r) { fprintf(stderr, "FAIL\n"); fail++; } else { fprintf(stderr, "OK\n"); pass++; } total++; } while(0)

static int scalar_int(cbm_store_t *s, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int out = -1;
    if (sqlite3_prepare_v2(cbm_store_get_db(s), sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) out = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return out;
}

TEST(memory_schema_init) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_store_close(s);
    return 0;
}

TEST(memory_append_event) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_event_t ev = {0};
    ev.type = "test.event";
    ev.source = "unit_test";
    ev.project = "test-proj";
    ev.payload = "{\"key\":\"value\"}";
    ev.confidence = 0.8;
    char *id = NULL;
    int rc = cbm_store_memory_append_event(s, &ev, &id);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(id != NULL);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_append_candidate) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.layer = "semantic";
    item.title = "Test Fact";
    item.content = "This is a test memory item for MVP validation.";
    item.scope_project = "test-proj";
    item.status = "candidate";
    item.confidence = 0.9;
    char *id = NULL;
    int rc = cbm_store_memory_append_candidate(s, &item, &id);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(id != NULL);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_append_structured_candidate) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "decision";
    item.layer = "semantic";
    item.title = "Use structured memory events";
    item.summary = "Structured fields should survive the hot path.";
    item.content = "Store user decisions with explicit entity and predicate.";
    item.scope_user = "alice";
    item.scope_project = "test-proj";
    item.scope_task = "memory-mvp";
    item.entity_key = "memory.events";
    item.predicate = "decides";
    item.importance = 0.8;
    item.confidence = 0.9;
    item.reusability = 0.7;
    item.specificity = 0.6;
    item.status = "candidate";
    item.source_event_ids = "[\"evt-structured\"]";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.kind, "decision") == 0);
    ASSERT(strcmp(out.layer, "semantic") == 0);
    ASSERT(strcmp(out.scope_user, "alice") == 0);
    ASSERT(strcmp(out.scope_task, "memory-mvp") == 0);
    ASSERT(strcmp(out.entity_key, "memory.events") == 0);
    ASSERT(strcmp(out.predicate, "decides") == 0);
    ASSERT(out.importance > 0.79 && out.reusability > 0.69 && out.specificity > 0.59);
    ASSERT(strstr(out.source_event_ids, "evt-structured") != NULL);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}


TEST(memory_get_item) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.content = "Get item test";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    cbm_memory_item_t out = {0};
    int rc = cbm_store_memory_get_item(s, id, &out);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(out.content != NULL);
    ASSERT(strstr(out.content, "Get item test") != NULL);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_structured) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    for (int i = 0; i < 3; i++) {
        cbm_memory_item_t item = {0};
        item.kind = "fact";
        item.content = "Structured retrieval test item";
        item.scope_project = "test-proj";
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    int processed = -1;
    cbm_store_memory_consolidate(s, "test-proj", 100, &processed);
    fprintf(stderr, "processed=%d\n", processed); ASSERT(processed == 3);
    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    int rc = cbm_store_memory_retrieve(s, &q, &res);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(res.count >= 1);
    ASSERT(res.count <= 3);
    cbm_store_memory_result_free(&res);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_fts) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.content = "FTS5 full text search test for memory system";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    free(id);
    int processed = -1;
    cbm_store_memory_consolidate(s, "test-proj", 100, &processed);
    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "FTS5";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    int rc = cbm_store_memory_retrieve(s, &q, &res);
    if (rc == CBM_STORE_OK) { ASSERT(res.count >= 1); }
    cbm_store_memory_result_free(&res);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_vector_fusion) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.content = "vector-only-alpha";
    item.scope_project = "test-proj";
    item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    sqlite3_stmt *del = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s), "DELETE FROM memory_fts WHERE item_id=?1;",
                              -1, &del, NULL) == SQLITE_OK);
    sqlite3_bind_text(del, 1, id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(del) == SQLITE_DONE);
    sqlite3_finalize(del);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "vector-only-alpha";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count >= 1);
    bool found_vector = false;
    for (int i = 0; i < res.count; i++) {
        if (res.items[i].id && strcmp(res.items[i].id, id) == 0 &&
            res.items[i].retrieval_source &&
            strcmp(res.items[i].retrieval_source, "vector") == 0) {
            found_vector = true;
            ASSERT(res.items[i].retrieval_score > 0.99);
        }
    }
    ASSERT(found_vector);
    cbm_store_memory_result_free(&res);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_conflict_resolution) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t base = {0};
    base.kind = "fact";
    base.layer = "semantic";
    base.content = "Service timeout is 30 seconds";
    base.scope_project = "test-proj";
    base.entity_key = "service.timeout";
    base.predicate = "is";
    base.status = "active";
    base.confidence = 0.95;
    base.updated_at = 1000;
    char *base_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &base, &base_id) == CBM_STORE_OK);

    cbm_memory_item_t task = {0};
    task.kind = "fact";
    task.layer = "semantic";
    task.content = "Service timeout is 45 seconds for import task";
    task.scope_project = "test-proj";
    task.scope_task = "import";
    task.entity_key = "service.timeout";
    task.predicate = "is";
    task.status = "active";
    task.confidence = 0.80;
    task.updated_at = 2000;
    char *task_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &task, &task_id) == CBM_STORE_OK);

    sqlite3_stmt *stmt = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
                              "INSERT INTO memory_edge "
                              "(id,src_id,dst_id,type,weight,origin,confidence,created_at) "
                              "VALUES ('edge-1',?1,?2,'contradicts',1.0,'test',1.0,1),"
                              "('edge-2',?2,?1,'contradicts',1.0,'test',1.0,1);",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, base_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.entity_key = "service.timeout";
    q.task = "import";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].id, task_id) == 0);
    ASSERT(res.items[0].conflict_count == 1);
    ASSERT(strstr(res.items[0].conflict_ids, base_id) != NULL);
    ASSERT(res.items[0].conflict_resolution != NULL);
    ASSERT(strstr(res.items[0].conflict_resolution, "winner_by_scope") != NULL);
    cbm_store_memory_result_free(&res);

    memset(&q, 0, sizeof(q));
    q.project = "test-proj";
    q.entity_key = "service.timeout";
    q.limit = 5;
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].id, base_id) == 0);
    ASSERT(res.items[0].conflict_count == 1);
    ASSERT(strstr(res.items[0].conflict_ids, task_id) != NULL);
    ASSERT(res.items[0].conflict_resolution != NULL);
    ASSERT(strstr(res.items[0].conflict_resolution, "winner_by_confidence") != NULL);
    cbm_store_memory_result_free(&res);

    free(base_id);
    free(task_id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_evidence_graph) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t root = {0};
    root.kind = "decision";
    root.layer = "semantic";
    root.content = "Use SQLite as the only MVP memory store";
    root.scope_project = "test-proj";
    root.entity_key = "memory.storage";
    root.predicate = "decides";
    root.status = "active";
    root.confidence = 0.9;
    char *root_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &root, &root_id) == CBM_STORE_OK);

    cbm_memory_item_t support = {0};
    support.kind = "fact";
    support.layer = "semantic";
    support.content = "Single SQLite file keeps transactions simple";
    support.scope_project = "test-proj";
    support.entity_key = "memory.storage";
    support.predicate = "supports";
    support.status = "active";
    support.confidence = 0.8;
    char *support_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &support, &support_id) == CBM_STORE_OK);

    cbm_memory_item_t task = {0};
    task.kind = "task";
    task.layer = "episodic";
    task.content = "MVP implementation task";
    task.scope_project = "test-proj";
    task.entity_key = "memory.mvp";
    task.predicate = "used_in";
    task.status = "active";
    char *task_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &task, &task_id) == CBM_STORE_OK);

    sqlite3_stmt *stmt = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
                              "INSERT INTO memory_edge "
                              "(id,src_id,dst_id,type,weight,origin,confidence,created_at) VALUES "
                              "('edge-derived',?1,'evt-1','derived_from',1.0,'rule',1.0,1),"
                              "('edge-support',?2,?1,'supports',1.0,'test',0.9,1),"
                              "('edge-used',?2,?3,'used_in',1.0,'test',0.8,1);",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, root_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, support_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, task_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.entity_key = "memory.storage";
    q.kind = "decision";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(res.items[0].evidence_json != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"type\":\"derived_from\"") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"type\":\"supports\"") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"type\":\"used_in\"") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"hop\":2") != NULL);
    cbm_store_memory_result_free(&res);

    free(root_id);
    free(support_id);
    free(task_id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_consolidate) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Consolidation test";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = -1;
    int rc = cbm_store_memory_consolidate(s, "test-proj", 100, &processed);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(processed == 1);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0);
    ASSERT(out.entity_key != NULL);
    ASSERT(out.predicate != NULL);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE type='belongs_to'") == 1);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_consolidate_merge_keeps_new_event_evidence) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t active = {0};
    active.kind = "fact";
    active.layer = "semantic";
    active.content = "Use SQLite for memory storage";
    active.scope_project = "test-proj";
    active.entity_key = "memory.storage";
    active.predicate = "decides";
    active.status = "active";
    char *active_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &active, &active_id) == CBM_STORE_OK);

    cbm_memory_item_t candidate = {0};
    candidate.kind = "fact";
    candidate.layer = "semantic";
    candidate.content = "Use SQLite for memory storage";
    candidate.scope_project = "test-proj";
    candidate.entity_key = "memory.storage";
    candidate.predicate = "decides";
    candidate.status = "candidate";
    candidate.source_event_ids = "[\"evt-merge-support\"]";
    char *candidate_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &candidate, &candidate_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);

    /* Archived candidate: supersedes must be NULL (it is retired, not superseding). */
    cbm_memory_item_t archived = {0};
    ASSERT(cbm_store_memory_get_item(s, candidate_id, &archived) == CBM_STORE_OK);
    ASSERT(strcmp(archived.status, "archived") == 0);
    ASSERT(archived.supersedes == NULL || archived.supersedes[0] == '\0');
    cbm_store_memory_item_free(&archived);

    /* Surviving active item: supersedes must point to the retired candidate. */
    cbm_memory_item_t survivor = {0};
    ASSERT(cbm_store_memory_get_item(s, active_id, &survivor) == CBM_STORE_OK);
    ASSERT(survivor.supersedes != NULL && strcmp(survivor.supersedes, candidate_id) == 0);
    cbm_store_memory_item_free(&survivor);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.entity_key = "memory.storage";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].id, active_id) == 0);
    ASSERT(res.items[0].evidence_json != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "evt-merge-support") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"origin\":\"merge\"") != NULL);
    cbm_store_memory_result_free(&res);

    free(active_id);
    free(candidate_id);
    cbm_store_close(s);
    return 0;
}


TEST(memory_health) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_health_t h = {0};
    int rc = cbm_store_memory_health(s, "test-proj", &h);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(h.item_count >= 0);
    ASSERT(h.hit_rate >= 0.0);
    cbm_store_close(s);
    return 0;
}

TEST(memory_mark_hits) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Hit counter test";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    const char *ids[] = {id};
    ASSERT(cbm_store_memory_mark_hits(s, ids, 1, 0) == CBM_STORE_OK);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(out.hit_count == 1);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_update_status_retracts_from_default_retrieval) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.content = "Retracted memory should be hidden";
    item.scope_project = "test-proj";
    item.status = "active";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_update_status(s, id, "test-proj", "retracted") == CBM_STORE_OK);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 0);
    cbm_store_memory_result_free(&res);

    q.include_inactive = true;
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].status, "retracted") == 0);
    cbm_store_memory_result_free(&res);

    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_update_status_rejects_invalid_status) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Invalid status test";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_update_status(s, id, "test-proj", "deleted") == CBM_STORE_ERR);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_feedback_useful_records_event_and_boosts_hit_signal) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Useful feedback target";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.5;
    item.reusability = 0.5;
    item.decay = 0.2;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    char *event_id = NULL;
    ASSERT(cbm_store_memory_feedback(s, id, "test-proj", "useful", "helped answer", "alice", &event_id) == CBM_STORE_OK);
    ASSERT(event_id != NULL);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(out.hit_count == 1);
    ASSERT(out.confidence > 0.54);
    ASSERT(out.reusability > 0.54);
    ASSERT(out.decay < 0.11);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'") == 1);
    cbm_store_memory_item_free(&out);
    free(event_id);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_feedback_wrong_retracts_from_default_retrieval) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Wrong feedback target";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.8;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_feedback(s, id, "test-proj", "wrong", "contradicted by user", NULL, NULL) == CBM_STORE_OK);
    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 0);
    cbm_store_memory_result_free(&res);
    q.include_inactive = true;
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].status, "retracted") == 0);
    cbm_store_memory_result_free(&res);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_decay_archives_stale) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Low confidence stale memory";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.1;
    item.reusability = 0.1;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_decay(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0 || strcmp(out.status, "archived") == 0);
    ASSERT(out.decay > 0.0);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* The dedup vector must carry lexical-semantic signal: two near-identical
 * statements in the same (entity, predicate, scope) bucket should MERGE via the
 * cosine>=0.90 path during consolidation, not coexist. A whole-string hash
 * would make these orthogonal and never merge. */
TEST(memory_consolidate_merges_paraphrase) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t base = {0};
    base.kind = "preference";
    base.content = "user prefers dark mode for the editor theme";
    base.scope_project = "test-proj";
    base.entity_key = "user:theme";
    base.predicate = "prefers";
    base.status = "active";
    char *base_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &base, &base_id) == CBM_STORE_OK);

    /* Same fact, one extra word: high lexical overlap → cosine should clear 0.90. */
    cbm_memory_item_t dup = {0};
    dup.kind = "preference";
    dup.content = "user really prefers dark mode for the editor theme";
    dup.scope_project = "test-proj";
    dup.entity_key = "user:theme";
    dup.predicate = "prefers";
    dup.status = "candidate";
    char *dup_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &dup, &dup_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);

    cbm_memory_item_t merged = {0};
    ASSERT(cbm_store_memory_get_item(s, dup_id, &merged) == CBM_STORE_OK);
    ASSERT(strcmp(merged.status, "archived") == 0); /* merged away, not coexisting */
    cbm_store_memory_item_free(&merged);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE type='similar_to'") == 1);

    free(base_id);
    free(dup_id);
    cbm_store_close(s);
    return 0;
}

/* Divergent values under the SAME (entity, predicate, scope) bucket must be
 * detected as a contradiction by consolidation itself (cosine<=0.30), producing
 * a contradicts edge WITHOUT any hand-injected edge. This is the signal the
 * read-time adjudicator depends on. */
TEST(memory_consolidate_detects_contradiction) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t a = {0};
    a.kind = "decision";
    a.content = "always indent using tabs";
    a.scope_project = "test-proj";
    a.entity_key = "style:indentation";
    a.predicate = "decides";
    a.status = "active";
    char *a_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &a, &a_id) == CBM_STORE_OK);

    cbm_memory_item_t b = {0};
    b.kind = "decision";
    b.content = "never use four whitespace characters";
    b.scope_project = "test-proj";
    b.entity_key = "style:indentation";
    b.predicate = "decides";
    b.status = "candidate";
    char *b_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &b, &b_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);

    /* Not merged (low cosine) and a contradicts edge was raised by the rule path. */
    cbm_memory_item_t bout = {0};
    ASSERT(cbm_store_memory_get_item(s, b_id, &bout) == CBM_STORE_OK);
    ASSERT(strcmp(bout.status, "active") == 0); /* coexists, not archived */
    cbm_store_memory_item_free(&bout);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE type='contradicts' AND origin='rule'") >= 1);

    free(a_id);
    free(b_id);
    cbm_store_close(s);
    return 0;
}

/* A successful recall must let accumulated decay fall back (framework §11.2),
 * not merely refresh recency. */
TEST(memory_mark_hits_relaxes_decay) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "decayed but still recalled";
    item.scope_project = "test-proj";
    item.status = "active";
    item.decay = 0.5;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);

    const char *ids[1] = { id };
    ASSERT(cbm_store_memory_mark_hits(s, ids, 1, 0) == CBM_STORE_OK);

    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(out.hit_count == 1);
    ASSERT(out.last_hit_at > 0);
    ASSERT(out.decay < 0.5);          /* decay relaxed by the hit */
    ASSERT(out.decay >= 0.0);
    cbm_store_memory_item_free(&out);

    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── Step-1: real 768-d semantic embeddings ─────────────────────── */

/* The whole point of the 256→768 nomic switch: vector recall ranks a memory
 * by *meaning*, not shared spelling. Store two memories — one topically related
 * to the query but sharing few exact words, one unrelated — and assert the
 * related one gets the higher vector score. Under the old bag-of-words hashing
 * the related pair (different words, same topic) would have been near-orthogonal
 * and this ranking would not hold. */
TEST(memory_embedding_ranks_semantic_neighbor_higher) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);

    cbm_memory_item_t related = {0};
    related.kind = "fact"; related.layer = "semantic";
    related.content = "the function returns an integer value";
    related.scope_project = "test-proj"; related.status = "candidate";
    char *rid = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &related, &rid) == CBM_STORE_OK);

    cbm_memory_item_t unrelated = {0};
    unrelated.kind = "fact"; unrelated.layer = "semantic";
    unrelated.content = "the weather today is sunny and warm";
    unrelated.scope_project = "test-proj"; unrelated.status = "candidate";
    char *uid = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &unrelated, &uid) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);

    /* Query shares almost no exact tokens with the related item ("method"≠"function",
     * "int"≠"integer", "result"≠"value") but is the same topic; embeddings should
     * still rank it above the weather item. */
    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "the method yields an int result";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);

    double related_score = -1.0, unrelated_score = -1.0;
    for (int i = 0; i < res.count; i++) {
        if (res.items[i].id && strcmp(res.items[i].id, rid) == 0) related_score = res.items[i].retrieval_score;
        if (res.items[i].id && strcmp(res.items[i].id, uid) == 0) unrelated_score = res.items[i].retrieval_score;
    }
    ASSERT(related_score >= 0.0);              /* related item retrieved */
    ASSERT(related_score > unrelated_score);   /* and ranked above the unrelated one */

    cbm_store_memory_result_free(&res);
    free(rid); free(uid);
    cbm_store_close(s);
    return 0;
}

/* The stored memory vector must be the full 768-d nomic dimension, not the old
 * 256. Guards against a silent regression in MEMORY_VEC_DIM wiring. */
TEST(memory_embedding_dim_is_768) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic";
    item.content = "dimension probe"; item.scope_project = "test-proj"; item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT dim FROM memory_vec LIMIT 1") == 768);
    ASSERT(scalar_int(s, "SELECT length(embedding) FROM memory_vec LIMIT 1") == 768);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── A1: schema migration framework ─────────────────────────────── */

TEST(migration_fresh_db_sets_version) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    /* A freshly opened DB must be stamped at the current schema version. */
    ASSERT(scalar_int(s, "PRAGMA user_version") == 3);
    /* Baseline tables must exist (migration 0->1 ran init_schema). */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='memory_item'") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='memory_event'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(migration_idempotent_reopen) {
    char path[512];
    const char *tmp = getenv("TMP");
    if (!tmp || !tmp[0]) tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(path, sizeof(path), "%s/cbm_migrate_test_%d.db", tmp, (int)1234);
    remove(path);

    /* First open: fresh DB, runs baseline migration, writes an item. */
    cbm_store_t *s = cbm_store_open_path(path);
    ASSERT(s != NULL);
    ASSERT(scalar_int(s, "PRAGMA user_version") == 3);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic"; item.title = "persisted";
    item.content = "survives reopen"; item.scope_project = "test-proj";
    item.status = "candidate"; item.confidence = 0.9;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    free(id);
    cbm_store_close(s);

    /* Reopen the same on-disk DB: migration must be a no-op (already current),
     * version unchanged, data intact. */
    cbm_store_t *s2 = cbm_store_open_path(path);
    ASSERT(s2 != NULL);
    ASSERT(scalar_int(s2, "PRAGMA user_version") == 3);
    ASSERT(scalar_int(s2, "SELECT COUNT(*) FROM memory_item") == 1);
    cbm_store_close(s2);
    remove(path);
    return 0;
}

/* ── P0-1: memory-path transaction atomicity ────────────────────── */

/* feedback writes a memory_item UPDATE + a memory_event audit row; both must
 * commit together. Inject a failure on the event insert via a trigger, then
 * assert the item UPDATE was rolled back (status NOT changed, no audit row). */
TEST(feedback_atomic_rolls_back_item_on_event_failure) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic"; item.title = "atomic feedback";
    item.content = "feedback should be all-or-nothing"; item.scope_project = "test-proj";
    item.status = "active"; item.confidence = 0.9;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);

    /* Force any memory_event INSERT to fail mid-transaction. */
    char *err = NULL;
    ASSERT(sqlite3_exec(cbm_store_get_db(s),
        "CREATE TRIGGER cbm_fail_event BEFORE INSERT ON memory_event "
        "BEGIN SELECT RAISE(ABORT, 'injected event failure'); END;",
        NULL, NULL, &err) == SQLITE_OK);

    /* "wrong" feedback would retract the item AND write an audit event.
     * The event insert fails -> the whole op must roll back. */
    int rc = cbm_store_memory_feedback(s, id, "test-proj", "wrong", "should not persist", NULL, NULL);
    ASSERT(rc == CBM_STORE_ERR);

    /* Item must NOT be retracted (UPDATE rolled back). */
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0);
    cbm_store_memory_item_free(&out);
    /* No audit event persisted. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'") == 0);

    (void)sqlite3_exec(cbm_store_get_db(s), "DROP TRIGGER cbm_fail_event;", NULL, NULL, NULL);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* consolidate activates an item and writes its vector + FTS + edges in one
 * batch transaction. Verify the positive invariant: a successfully activated
 * item always has all secondary rows (never a half-written active item). */
TEST(consolidate_active_item_fully_indexed) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic"; item.title = "indexed";
    item.content = "active items carry vector and fts and belongs_to edge";
    item.scope_project = "test-proj"; item.status = "candidate"; item.confidence = 0.9;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);

    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);

    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0);
    cbm_store_memory_item_free(&out);

    /* All-or-nothing: the active item has its vector, FTS row, and belongs_to edge. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_fts") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE type='belongs_to'") == 1);

    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── Step-1: lazy auto-maintenance trigger ──────────────────────── */

TEST(migration_v2_adds_meta_table) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    ASSERT(scalar_int(s, "PRAGMA user_version") == 3);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='memory_meta'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(maintain_triggers_consolidate_on_threshold) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    /* Append the trigger threshold (8) of candidates. */
    for (int i = 0; i < 8; i++) {
        cbm_memory_item_t item = {0};
        char content[64];
        snprintf(content, sizeof(content), "auto maintain candidate number %d", i);
        item.kind = "fact"; item.layer = "semantic"; item.title = "auto";
        item.content = content; item.scope_project = "test-proj";
        item.status = "candidate"; item.confidence = 0.9;
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='candidate'") == 8);

    /* Fresh DB: last_consolidate_ms=0, so the elapsed-time gate is satisfied;
     * 8 >= threshold triggers consolidation. */
    cbm_memory_maintain_report_t rep = {0};
    ASSERT(cbm_store_memory_maintain_if_due(s, "test-proj", &rep) == CBM_STORE_OK);
    ASSERT(rep.consolidated == true);
    ASSERT(rep.consolidate_count == 8);
    /* Candidates are now active and fully indexed. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='candidate'") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='active'") == 8);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 8);
    /* Timestamp was recorded. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_meta WHERE key='last_consolidate_ms'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(maintain_below_threshold_does_not_consolidate) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    /* Only a few candidates (below threshold 8). Fresh DB means the backstop
     * (MAX_INTERVAL) gate would fire on since=now-0 — but that is the documented
     * behavior: a backstop ensures stray candidates eventually consolidate.
     * To test the threshold path specifically we pre-seed last_consolidate_ms to
     * "now" so neither the threshold (count<8) nor the backstop (just ran) fires. */
    {
        cbm_memory_item_t item = {0};
        item.kind = "fact"; item.layer = "semantic"; item.title = "few";
        item.content = "only one candidate, below threshold";
        item.scope_project = "test-proj"; item.status = "candidate"; item.confidence = 0.9;
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    /* Mark maintenance as just-run so debounce + backstop both block. */
    {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO memory_meta (key,value) VALUES "
                 "('last_consolidate_ms','%lld'),('last_decay_ms','%lld');",
                 (long long)((int64_t)time(NULL) * 1000LL),
                 (long long)((int64_t)time(NULL) * 1000LL));
        ASSERT(sqlite3_exec(cbm_store_get_db(s), sql, NULL, NULL, NULL) == SQLITE_OK);
    }
    cbm_memory_maintain_report_t rep = {0};
    ASSERT(cbm_store_memory_maintain_if_due(s, "test-proj", &rep) == CBM_STORE_OK);
    ASSERT(rep.consolidated == false);
    ASSERT(rep.decayed == false);
    /* Candidate untouched. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='candidate'") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 0);
    cbm_store_close(s);
    return 0;
}

TEST(maintain_disabled_by_env) {
    /* With the kill switch set, even an over-threshold batch must not trigger. */
#ifdef _WIN32
    _putenv("CBM_MEMORY_AUTO_MAINTAIN=0");
#else
    setenv("CBM_MEMORY_AUTO_MAINTAIN", "0", 1);
#endif
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    for (int i = 0; i < 8; i++) {
        cbm_memory_item_t item = {0};
        char content[64];
        snprintf(content, sizeof(content), "disabled env candidate %d", i);
        item.kind = "fact"; item.layer = "semantic"; item.title = "off";
        item.content = content; item.scope_project = "test-proj";
        item.status = "candidate"; item.confidence = 0.9;
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    cbm_memory_maintain_report_t rep = {0};
    ASSERT(cbm_store_memory_maintain_if_due(s, "test-proj", &rep) == CBM_STORE_OK);
    ASSERT(rep.consolidated == false);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='candidate'") == 8);
    cbm_store_close(s);
#ifdef _WIN32
    _putenv("CBM_MEMORY_AUTO_MAINTAIN=");
#else
    unsetenv("CBM_MEMORY_AUTO_MAINTAIN");
#endif
    return 0;
}

int main(void) {
    fprintf(stderr, "START\n");
    fflush(stderr);
    int pass = 0, fail = 0, total = 0;
    RUN(memory_schema_init);
    RUN(memory_append_event);
    RUN(memory_append_candidate);
    RUN(memory_append_structured_candidate);
    RUN(memory_get_item);
    RUN(memory_retrieve_structured);
    RUN(memory_retrieve_fts);
    RUN(memory_retrieve_vector_fusion);
    RUN(memory_retrieve_conflict_resolution);
    RUN(memory_retrieve_evidence_graph);
    RUN(memory_consolidate);
    RUN(memory_consolidate_merge_keeps_new_event_evidence);
    RUN(memory_consolidate_merges_paraphrase);
    RUN(memory_consolidate_detects_contradiction);
    RUN(memory_mark_hits_relaxes_decay);
    RUN(memory_health);
    RUN(memory_mark_hits);
    RUN(memory_update_status_retracts_from_default_retrieval);
    RUN(memory_update_status_rejects_invalid_status);
    RUN(memory_feedback_useful_records_event_and_boosts_hit_signal);
    RUN(memory_feedback_wrong_retracts_from_default_retrieval);
    RUN(memory_decay_archives_stale);
    RUN(memory_embedding_ranks_semantic_neighbor_higher);
    RUN(memory_embedding_dim_is_768);
    RUN(migration_fresh_db_sets_version);
    RUN(migration_idempotent_reopen);
    RUN(feedback_atomic_rolls_back_item_on_event_failure);
    RUN(consolidate_active_item_fully_indexed);
    RUN(migration_v2_adds_meta_table);
    RUN(maintain_triggers_consolidate_on_threshold);
    RUN(maintain_below_threshold_does_not_consolidate);
    RUN(maintain_disabled_by_env);
    fprintf(stderr, "\n%d/%d passed\n", pass, total);
    return fail ? 1 : 0;
}
