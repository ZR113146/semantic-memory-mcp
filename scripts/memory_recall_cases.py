#!/usr/bin/env python3
"""Fixed memory + query cases for memory_recall_eval.py.

SEED_MEMORIES: written once into a throwaway project, then consolidated.
Each entry is the argument set for the `events` tool (project is injected by
the harness). Mix of Chinese and English to exercise the CJK bigram
segmentation and the OR-joined FTS retrieval path.

CASES: each is {query, expect}.
    expect = substring that must appear in a returned memory's content
             (None means a NEGATIVE case: nothing relevant should match —
             used to catch false positives from over-eager OR matching).

Keep cases deterministic and content substrings unique enough to score
unambiguously. When you change retrieval logic, re-run with --baseline to
re-anchor, but only after manually confirming the new numbers are correct.
"""

# ── Seed corpus ────────────────────────────────────────────────────
SEED_MEMORIES = [
    # Chinese — preferences / decisions / constraints
    {"kind": "preference", "content": "用户偏好用 pnpm 而非 npm，所有新项目默认使用 pnpm 作为包管理器",
     "importance": 0.9, "reusability": 0.9, "payload": {"tool": "pnpm"}},
    {"kind": "decision", "content": "项目数据统一存储在 SQLite，知识图谱和长期记忆都走同一个库",
     "importance": 0.8, "payload": {"storage": "sqlite"}},
    {"kind": "constraint", "content": "构建产物必须是单一静态二进制，不允许引入运行时外部依赖",
     "importance": 0.8, "payload": {"rule": "static-binary"}},
    {"kind": "decision", "content": "认证服务采用 JWT 令牌，使用 RS256 算法对所有接口签名",
     "importance": 0.8, "payload": {"auth": "jwt-rs256"}},
    {"kind": "lesson", "content": "中文检索失效的根因是分词器把整段中文当成一个词，需要切成字符二元组",
     "importance": 0.7, "payload": {"topic": "tokenizer"}},
    # English — preferences / decisions
    {"kind": "preference", "content": "Prefer ripgrep over grep for code search across the repository",
     "importance": 0.7, "payload": {"tool": "ripgrep"}},
    {"kind": "decision", "content": "The HTTP server binds to localhost only and requires no authentication in dev mode",
     "importance": 0.6, "payload": {"net": "localhost"}},
    {"kind": "constraint", "content": "All database writes must go through the WAL journal mode for durability",
     "importance": 0.7, "payload": {"db": "wal"}},
    # A second pnpm-adjacent memory to test ranking (less specific)
    {"kind": "event", "content": "团队讨论了包管理器选型，最终倾向 pnpm 的硬链接节省磁盘",
     "importance": 0.5, "payload": {"topic": "pkgmgr"}},
]

# ── Query cases ────────────────────────────────────────────────────
CASES = [
    # --- Chinese recall (the core fix) ---
    {"query": "包管理器", "expect": "pnpm 作为包管理器"},
    {"query": "pnpm", "expect": "pnpm 作为包管理器"},
    {"query": "数据存储", "expect": "统一存储在 SQLite"},
    {"query": "静态二进制", "expect": "单一静态二进制"},
    {"query": "认证令牌", "expect": "JWT 令牌"},
    {"query": "签名算法", "expect": "RS256"},
    {"query": "分词器", "expect": "分词器把整段中文"},
    {"query": "字符二元组", "expect": "切成字符二元组"},
    # --- English recall ---
    {"query": "ripgrep", "expect": "Prefer ripgrep over grep"},
    {"query": "code search", "expect": "ripgrep over grep for code search"},
    {"query": "WAL journal", "expect": "WAL journal mode"},
    {"query": "authentication localhost", "expect": "binds to localhost"},
    # --- Mixed / cross-lingual token ---
    {"query": "JWT RS256", "expect": "RS256"},
    {"query": "SQLite 知识图谱", "expect": "知识图谱和长期记忆"},
    # --- Negative cases (must NOT produce an FTS hit) ---
    {"query": "区块链智能合约", "expect": None},
    {"query": "kubernetes deployment yaml", "expect": None},
    {"query": "用户的生日和家庭住址", "expect": None},
]
