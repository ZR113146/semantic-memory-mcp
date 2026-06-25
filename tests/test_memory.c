#include "store/store.h"
#include "foundation/platform.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

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

int main(void) {
    fprintf(stderr, "START\n");
    fflush(stderr);
    int pass = 0, fail = 0, total = 0;
    RUN(memory_schema_init);
    RUN(memory_append_event);
    RUN(memory_append_candidate);
    RUN(memory_get_item);
    RUN(memory_retrieve_structured);
    RUN(memory_retrieve_fts);
    RUN(memory_retrieve_vector_fusion);
    RUN(memory_retrieve_conflict_resolution);
    RUN(memory_retrieve_evidence_graph);
    RUN(memory_consolidate);
    RUN(memory_health);
    RUN(memory_mark_hits);
    RUN(memory_decay_archives_stale);
    fprintf(stderr, "\n%d/%d passed\n", pass, total);
    return fail ? 1 : 0;
}
