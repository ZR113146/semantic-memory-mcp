#!/usr/bin/env python3
"""embed_sidecar.py — local sentence-embedding sidecar for the memory subsystem.

The C process spawns this as a long-lived child and speaks newline-delimited
JSON over stdin/stdout:

    -> {"id": N, "text": "..."}
    <- {"id": N, "vec": [f0, f1, ...]}      # len == model dim (bge-m3: 1024)

On a malformed request or encode error it replies {"id": N, "error": "..."}.
A readiness line {"ready": true, "dim": D} is written to STDERR after the model
loads (stdout stays clean for protocol traffic).

Runs fully offline against the locally-cached model. No network. Model is
overridable via CBM_EMBED_MODEL (default BAAI/bge-m3).
"""
import json
import os
import sys


def log(msg):
    sys.stderr.write(str(msg) + "\n")
    sys.stderr.flush()


def main():
    model_name = os.environ.get("CBM_EMBED_MODEL", "BAAI/bge-m3")
    # Default to offline so a missing cache fails fast rather than hitting the
    # network; callers who want a download can unset this explicitly.
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")
    try:
        from sentence_transformers import SentenceTransformer
        model = SentenceTransformer(model_name)
    except Exception as e:  # noqa: BLE001
        log(json.dumps({"ready": False, "error": f"{type(e).__name__}: {e}"}))
        return 1

    dim = int(model.get_sentence_embedding_dimension())
    log(json.dumps({"ready": True, "dim": dim, "model": model_name}))

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            rid = req.get("id")
            text = req.get("text", "")
            vec = model.encode([text], normalize_embeddings=True)[0]
            out = {"id": rid, "vec": [float(x) for x in vec.tolist()]}
        except Exception as e:  # noqa: BLE001
            out = {"id": req.get("id") if isinstance(req, dict) else None,
                   "error": f"{type(e).__name__}: {e}"}
        sys.stdout.write(json.dumps(out) + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
