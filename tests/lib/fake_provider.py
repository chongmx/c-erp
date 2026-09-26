#!/usr/bin/env python3
"""
fake_provider.py — a model provider that answers exactly what a test needs.

    python3 tests/lib/fake_provider.py <port> <mode>

Point `ir_ai_provider.base_url` at http://127.0.0.1:<port> and the ERP talks
to this instead of xAI or Anthropic. It speaks the OpenAI-compatible wire,
which is what xAI uses.

Why this exists: the failures worth testing here are the ones where the model
answers BADLY — cut off mid-JSON, or in prose — and neither the mock provider
(which always answers well) nor a real one (which cannot be asked to misbehave
on demand) can produce them. This can.

Modes:
  truncated   a reply that stops mid-string, with finish_reason "length"
  prose       a perfectly readable English answer that is not JSON
  repair      truncated to the MAIN model, valid JSON to any other model —
              which is the fallback parser's whole job, seen from outside
  models      only GET /v1/models is interesting; posts answer valid JSON

Every response is logged to stderr so a failing test can be read afterwards.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
MODE = sys.argv[2] if len(sys.argv) > 2 else "truncated"

GOOD = {
    "notes": "fake provider answer",
    "candidates": [{
        "query": "fake", "mpn": "FAKE-8MHZ-TCXO", "manufacturer": "Fake Semi",
        "name": "8 MHz TCXO", "confidence": 0.9, "why": "the only candidate",
        "parameters": [{"name": "Frequency", "value": "8", "unit": "MHz"}],
    }],
}

# Stops mid-string, exactly as a reply does when the model runs out of budget
# while it is still writing.
TRUNCATED = ('{"notes":"Searching for an 8 MHz temperature compensated crystal",'
             '"candidates":[{"mpn":"FAKE-8MHZ-TC')

PROSE = ("I found a few 8 MHz TCXOs. The ECS-TXO-8 is a common one, and Abracon "
         "make a similar part. I could not narrow it down further.")

MODELS = {"data": [{"id": "fake-large"}, {"id": "fake-mini"}, {"id": "fake-large-reasoning"}]}


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/v1/models"):
            self._send(200, MODELS)
        else:
            self._send(404, {"error": "no such path"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            req = json.loads(raw.decode() or "{}")
        except Exception:
            req = {}
        model = req.get("model", "")
        prompt = json.dumps(req.get("messages", req.get("input", "")))
        sys.stderr.write("fake_provider: model=%s len=%d\n" % (model, len(prompt)))
        sys.stderr.flush()

        # A second, different model asking to extract JSON is the repair pass.
        repairing = "BEGIN TEXT" in prompt

        if MODE == "prose" and not repairing:
            content, finish = PROSE, "stop"
        elif MODE == "repair" and repairing:
            content, finish = json.dumps(GOOD), "stop"
        elif MODE in ("truncated", "repair"):
            content, finish = TRUNCATED, "length"
        elif MODE == "models":
            content, finish = json.dumps(GOOD), "stop"
        else:
            content, finish = json.dumps(GOOD), "stop"

        self._send(200, {
            "id": "fake-1", "model": model or "fake-large",
            "choices": [{"index": 0, "finish_reason": finish,
                         "message": {"role": "assistant", "content": content}}],
        })

    def log_message(self, *args):     # quiet: stderr is for our own lines
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
