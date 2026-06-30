/*
 * hook_recall.c — `semantic-memory-mcp memory-recall`
 *
 * A non-blocking Claude Code UserPromptSubmit augmenter. Reads the hook JSON
 * from stdin and, using the user's prompt as a semantic query, injects
 * task-relevant long-term memories as `additionalContext` so the agent recalls
 * prior decisions/preferences without having to call memories_retrieve itself.
 *
 * Cardinal rule (shared with hook_augment.c): this NEVER blocks a prompt.
 * Every error, timeout, missing project, or empty result is `exit 0` with NO
 * stdout output (a clean pass-through). The hook can only ever ADD context.
 *
 * The underlying query is the same `memories_retrieve` MCP tool the agent
 * would call by hand, so retrieval ranking is judgment-source-shared with
 * production. Auto-maintenance (consolidate/decay) is elapsed-time gated inside
 * that handler and is best-effort; if it overruns, the deadline below yields a
 * clean no-op (any open SQLite txn rolls back).
 */

#include "cli/cli.h"
#include "foundation/mem.h"
#include "mcp/mcp.h"
#include "pipeline/pipeline.h"
#include "yyjson/yyjson.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define HR_STDIN_CAP (256 * 1024) /* hook payloads are tiny; cap defensively  */
#define HR_MIN_PROMPT 8           /* skip trivially short prompts before work  */
#define HR_MAX_QUERY 2048         /* cap the query handed to memories_retrieve */
#define HR_RESULT_LIMIT 5
#define HR_MAX_WALKUP 8    /* cwd may be a subdir of the indexed root          */
#define HR_DEADLINE_MS 600 /* hard in-process budget; memories_retrieve is     */
                           /* heavier than search_graph (vector + maybe        */
                           /* maintenance), so a touch above hook_augment's.   */
#define HR_CTX_CAP 4096

/* ── Hard deadline ────────────────────────────────────────────────
 * A slow SQLite open, query, or lazy-maintenance pass must never stall the
 * agent. When the timer fires we _exit(0). Output is written exactly once at
 * the very end, so firing mid-work yields a clean no-op (no partial JSON). */
#ifndef _WIN32
static void hr_deadline_exit(int sig) {
    (void)sig;
    _exit(0);
}

static void hr_arm_deadline(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = hr_deadline_exit;
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = HR_DEADLINE_MS / 1000;
    it.it_value.tv_usec = (HR_DEADLINE_MS % 1000) * 1000;
    setitimer(ITIMER_REAL, &it, NULL);
}
#else
static void hr_arm_deadline(void) { /* Windows: rely on settings.json timeout */ }
#endif

/* ── stdin ────────────────────────────────────────────────────────── */

static char *hr_read_stdin(void) {
    char *buf = malloc(HR_STDIN_CAP + 1);
    if (!buf) {
        return NULL;
    }
    size_t total = 0;
    size_t n;
    while (total < HR_STDIN_CAP && (n = fread(buf + total, 1, HR_STDIN_CAP - total, stdin)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    return buf;
}

/* ── JSON helpers ─────────────────────────────────────────────────── */

static const char *hr_obj_str(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return (v && yyjson_is_str(v)) ? yyjson_get_str(v) : NULL;
}

/* Does the prompt carry at least one non-space character beyond HR_MIN_PROMPT
 * bytes of signal? Pure-whitespace or near-empty prompts skip all work. */
static bool hr_prompt_has_signal(const char *s) {
    if (!s) {
        return false;
    }
    size_t signal = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p > ' ') {
            signal++;
        }
    }
    return signal >= HR_MIN_PROMPT;
}

/* Build the memories_retrieve args JSON: {"project":..,"query":..,"limit":N}.
 * The query is passed through yyjson which escapes it safely — unlike
 * hook_augment's name_pattern, no identifier sanitization is needed since
 * query is a free-text FTS/vector input, not a regex. */
static char *hr_build_args(const char *project, const char *prompt) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    char query[HR_MAX_QUERY + 1];
    snprintf(query, sizeof(query), "%s", prompt);

    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "query", query);
    yyjson_mut_obj_add_int(doc, root, "limit", HR_RESULT_LIMIT);

    char *out = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return out; /* caller frees */
}

/* Pick the best human-readable label for a memory: summary first (it is the
 * one-sentence, query-like conclusion — the most useful recall label), then
 * title, then a content snippet. Summary leads title because the events writer
 * often leaves title unset, where it defaults to the useless "memory.event"
 * literal; the summary always carries real signal. Returns NULL only if all
 * three are empty. */
static const char *hr_memory_label(yyjson_val *m) {
    const char *summary = hr_obj_str(m, "summary");
    if (summary && summary[0]) {
        return summary;
    }
    const char *title = hr_obj_str(m, "title");
    if (title && title[0]) {
        return title;
    }
    const char *content = hr_obj_str(m, "content");
    if (content && content[0]) {
        return content;
    }
    return NULL;
}

/* Standing write-side nudge, injected every turn (see hr_format_context).
 * Per Claude Code hook semantics, UserPromptSubmit additionalContext is
 * re-injected adjacent to each new user message, so this stays salient through
 * a long session and survives context compaction — unlike a one-shot
 * SessionStart reminder, which gets buried as the transcript grows. Recording
 * stays the agent's judgment call (never automatic): this only reminds. */
static const char HR_WRITE_GUIDANCE[] =
    "\n[codebase-memory] Persisting durable knowledge is part of your job here, "
    "not an optional extra. If this turn produced a reusable decision, "
    "constraint, preference, or lesson worth recalling later, call the `events` "
    "tool now to record it — don't wait to be asked. Route user profile / "
    "preferences / cross-project lessons with scope='global'; project-specific "
    "rationale stays project-scoped. You judge what clears the bar: skip "
    "transient, speculative, or trivially-derivable details.";

/* Parse the MCP envelope returned by cbm_mcp_handle_tool and, if it is a
 * successful memories_retrieve result, format a compact additionalContext
 * string. Returns malloc'd text or NULL.
 *
 * A valid project always yields text: with >=1 memory it lists them and then
 * appends the standing write-side nudge; with zero memories it returns the
 * nudge alone (so the write reminder stays present every turn). NULL is
 * returned only on a genuine error/parse failure.
 *
 * *is_error is set when the envelope is an MCP error (e.g. project not
 * indexed) so the caller can try a parent directory. */
static char *hr_format_context(const char *envelope, bool *is_error) {
    *is_error = false;
    yyjson_doc *edoc = yyjson_read(envelope, strlen(envelope), 0);
    if (!edoc) {
        return NULL;
    }
    yyjson_val *eroot = yyjson_doc_get_root(edoc);
    yyjson_val *err = yyjson_obj_get(eroot, "isError");
    if (err && yyjson_is_true(err)) {
        *is_error = true;
        yyjson_doc_free(edoc);
        return NULL;
    }
    yyjson_val *content = yyjson_obj_get(eroot, "content");
    yyjson_val *item0 = (content && yyjson_is_arr(content)) ? yyjson_arr_get(content, 0) : NULL;
    const char *inner = hr_obj_str(item0, "text");
    if (!inner) {
        yyjson_doc_free(edoc);
        return NULL;
    }

    yyjson_doc *idoc = yyjson_read(inner, strlen(inner), 0);
    if (!idoc) {
        yyjson_doc_free(edoc);
        return NULL;
    }
    yyjson_val *iroot = yyjson_doc_get_root(idoc);
    yyjson_val *mems = yyjson_obj_get(iroot, "memories");
    size_t nres = (mems && yyjson_is_arr(mems)) ? yyjson_arr_size(mems) : 0;
    if (nres == 0) {
        /* Valid project, nothing relevant recalled — still inject the standing
         * write-side nudge so the reminder is present every turn. */
        yyjson_doc_free(idoc);
        yyjson_doc_free(edoc);
        return strdup(HR_WRITE_GUIDANCE);
    }

    char *text = malloc(HR_CTX_CAP);
    if (!text) {
        yyjson_doc_free(idoc);
        yyjson_doc_free(edoc);
        return NULL;
    }
    int off = snprintf(text, HR_CTX_CAP,
                       "[codebase-memory] %zu long-term memory(ies) relevant to "
                       "this prompt (recalled context — verify before relying on "
                       "any that name a file/symbol, they reflect a past state):",
                       nres);
    size_t idx;
    size_t maxn;
    yyjson_val *m;
    yyjson_arr_foreach(mems, idx, maxn, m) {
        /* Reserve tail room for HR_WRITE_GUIDANCE (~235 bytes) appended below. */
        if (off < 0 || off >= HR_CTX_CAP - 320) {
            break;
        }
        const char *label = hr_memory_label(m);
        if (!label) {
            continue;
        }
        const char *kind = hr_obj_str(m, "kind");
        off += snprintf(text + off, (size_t)(HR_CTX_CAP - off), "\n- %s%s%s%s", label,
                        (kind && kind[0]) ? "  [" : "", (kind && kind[0]) ? kind : "",
                        (kind && kind[0]) ? "]" : "");
    }

    /* Append the standing write-side nudge (re-injected every turn). */
    if (off > 0 && off < HR_CTX_CAP - (int)sizeof(HR_WRITE_GUIDANCE)) {
        snprintf(text + off, (size_t)(HR_CTX_CAP - off), "%s", HR_WRITE_GUIDANCE);
    }

    yyjson_doc_free(idoc);
    yyjson_doc_free(edoc);
    return text;
}

/* Emit the UserPromptSubmit additionalContext payload to stdout (exactly
 * once). */
static void hr_emit(const char *text) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *hso = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, hso, "hookEventName", "UserPromptSubmit");
    yyjson_mut_obj_add_str(doc, hso, "additionalContext", text);
    yyjson_mut_obj_add_val(doc, root, "hookSpecificOutput", hso);

    char *json = yyjson_mut_write(doc, 0, NULL);
    if (json) {
        fputs(json, stdout);
        free(json);
    }
    yyjson_mut_doc_free(doc);
}

/* Trim the last path component of `dir` in place, handling both '/' and '\'
 * separators (Claude Code passes native cwd, so on Windows this is a
 * backslash path). Returns false when there is no parent left to climb to. */
static bool hr_trim_to_parent(char *dir) {
    char *fwd = strrchr(dir, '/');
    char *bwd = strrchr(dir, '\\');
    char *sep = (fwd > bwd) ? fwd : bwd;
    if (!sep) {
        return false;
    }
    /* Don't climb past a POSIX root ("/x") or a Windows drive root ("C:\x"):
     * trimming there would leave "" or "C:", neither a real parent. */
    if (sep == dir) {
        return false; /* "/foo" → would leave "" */
    }
    if (sep == dir + 2 && dir[1] == ':') {
        return false; /* "C:\foo" → would leave "C:" */
    }
    *sep = '\0';
    return true;
}

/* Walk up from `start`, deriving a project name at each level (separator-
 * agnostic, unlike hook_augment which is POSIX-only) and querying
 * memories_retrieve until an indexed project is found or the walk is
 * exhausted. Stops at the first non-error result: a valid project with zero
 * memories is a legitimate "nothing to recall" and must NOT trigger a
 * parent-directory probe. */
static char *hr_resolve_and_query(cbm_mcp_server_t *srv, const char *start, const char *prompt) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", start);

    for (int level = 0; level < HR_MAX_WALKUP; level++) {
        char *project = cbm_project_name_from_path(dir);
        if (project) {
            char *args = hr_build_args(project, prompt);
            free(project);
            if (args) {
                char *res = cbm_mcp_handle_tool(srv, "memories_retrieve", args);
                free(args);
                if (res) {
                    bool is_error = false;
                    char *ctx = hr_format_context(res, &is_error);
                    free(res);
                    if (ctx) {
                        return ctx; /* memories → done */
                    }
                    if (!is_error) {
                        return NULL; /* valid project, nothing relevant → stop */
                    }
                }
            }
        }
        /* Not indexed at this level — climb to the parent. */
        if (!hr_trim_to_parent(dir)) {
            break;
        }
    }
    return NULL;
}

int cbm_cmd_hook_recall(void) {
    hr_arm_deadline();

    char *input = hr_read_stdin();
    if (!input) {
        return 0;
    }
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    if (!doc) {
        free(input);
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);

    /* Gate on the event when present, but tolerate its absence: the defining
     * signal is a usable `prompt` field. */
    const char *event = hr_obj_str(root, "hook_event_name");
    if (event && strcmp(event, "UserPromptSubmit") != 0) {
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

    const char *prompt = hr_obj_str(root, "prompt");
    if (!hr_prompt_has_signal(prompt)) {
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

    const char *cwd = hr_obj_str(root, "cwd");
#ifndef _WIN32
    char cwdbuf[4096];
    if (!cwd || !cwd[0]) {
        if (!getcwd(cwdbuf, sizeof(cwdbuf))) {
            yyjson_doc_free(doc);
            free(input);
            return 0;
        }
        cwd = cwdbuf;
    }
#else
    if (!cwd || !cwd[0]) {
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }
#endif

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        yyjson_doc_free(doc);
        free(input);
        return 0;
    }

    char *ctx = hr_resolve_and_query(srv, cwd, prompt);
    if (ctx) {
        hr_emit(ctx);
        free(ctx);
    }

    cbm_mcp_server_free(srv);
    yyjson_doc_free(doc);
    free(input);
    return 0;
}
