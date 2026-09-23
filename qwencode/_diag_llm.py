#!/usr/bin/env python3
"""Diagnostic: call the LLM once with the card prompt and dump the RAW response
so we can see why the first attempt fails to parse."""
import json, sys, urllib.request

API = "http://127.0.0.1:8000/v1/chat/completions"
MODEL = "Qwen/Qwen3.8-27B"

SYSTEM = (
    "You are the Card Forge, an oracle that forges collectible trading cards "
    "for calendar days. You always respond with a single valid JSON object and "
    "nothing else: no markdown, no code fences, no commentary."
)
USER = open("/dev/stdin").read() if not sys.stdin.isatty() else (
    "Today is Wednesday, September 23, 2026.\n\nForge a collectible trading "
    "card for this calendar day. Respond with a SINGLE JSON object (no markdown, "
    "no code fences) with EXACTLY these keys: name, subtitle, rarity, stats "
    "(power/wisdom/wonder/chaos/luck 0-100), fun_facts (3 strings), flavor, "
    "artwork {title, svg}."
)

payload = {
    "model": MODEL,
    "messages": [
        {"role": "system", "content": SYSTEM},
        {"role": "user", "content": USER},
    ],
    "max_tokens": 8192,
    "temperature": 0.8,
    "top_p": 0.95,
}
req = urllib.request.Request(
    API, data=json.dumps(payload).encode(),
    headers={"Content-Type": "application/json"}, method="POST",
)
with urllib.request.urlopen(req, timeout=300) as r:
    data = json.loads(r.read().decode())

msg = data["choices"][0]["message"]
print("=== KEYS in message ===")
print(list(msg.keys()))
print("=== finish_reason ===", data["choices"][0].get("finish_reason"))
print("=== usage ===", data.get("usage"))
content = msg.get("content") or ""
print("=== content length ===", len(content))
print("=== FIRST 1500 CHARS OF CONTENT ===")
print(content[:1500])
print("=== LAST 500 CHARS OF CONTENT ===")
print(content[-500:])

# Now try to parse it the same way the app does
import re
text = content.strip()
m = re.search(r"```(?:json)?\s*(\{.*\})\s*```", text, re.S)
if m:
    text = m.group(1)
else:
    s, e = text.find("{"), text.rfind("}")
    if s != -1 and e > s:
        text = text[s:e+1]
try:
    obj = json.loads(text)
    print("\n=== PARSE: SUCCESS ===")
    print("keys:", list(obj.keys()))
except Exception as e:
    print("\n=== PARSE FAILED ===", repr(e))
    print("text head:", repr(text[:200]))
    print("text tail:", repr(text[-200:]))
