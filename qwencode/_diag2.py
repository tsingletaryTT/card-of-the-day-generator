#!/usr/bin/env python3
"""Test the EXACT prompt the app uses, to see if the first response parses."""
import json, re, sys, urllib.request
from datetime import date

# Replicate the app's exact prompt builders (copied from card_of_the_day.py)
SYSTEM_PROMPT = (
    "You are the Card Forge, an oracle that forges collectible trading cards "
    "for calendar days. You always respond with a single valid JSON object "
    "and nothing else: no markdown, no code fences, no commentary."
)

def build_user_prompt(today):
    return f"""Today is {today.strftime('%A, %B %d, %Y')}.

Forge a collectible trading card for this calendar day. Respond with a SINGLE
JSON object (no markdown, no code fences) with EXACTLY these keys:

- "name": short evocative card name, max 4 words
- "subtitle": one-line subtitle, max 8 words
- "rarity": one of "common", "uncommon", "rare", "epic", "legendary"
  (choose by how historically/culturally significant this day is)
- "stats": an object with integer values 0-100 for exactly these keys:
  "power", "wisdom", "wonder", "chaos", "luck"
- "fun_facts": an array of exactly 3 short factual sentences (max 90 chars
  each) about this calendar day in history, culture or science. They must be
  true.
- "flavor": one evocative sentence, max 120 characters
- "artwork": an object with:
  - "title": max 40 characters
  - "svg": a complete standalone SVG document as a string, with these
    requirements:
    * starts with <svg ... viewBox="0 0 400 250" ...>
    * flat-illustration style, bold simple shapes, at most 6 colors
    * no external references, no scripts, no <text> elements
    * depicts the theme of this day

Return only the JSON object."""

today = date.today()
user_prompt = build_user_prompt(today)
print("=== USER PROMPT SENT ===")
print(user_prompt)
print("=== END PROMPT ===\n")

payload = {
    "model": "Qwen/Qwen3.8-27B",
    "messages": [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": user_prompt},
    ],
    "max_tokens": 8192,
    "temperature": 0.8,
    "top_p": 0.95,
}
req = urllib.request.Request(
    "http://127.0.0.1:8000/v1/chat/completions",
    data=json.dumps(payload).encode(),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(req, timeout=300) as r:
    data = json.loads(r.read().decode())
msg = data["choices"][0]["message"]
content = msg.get("content") or ""
print("=== finish_reason:", data["choices"][0].get("finish_reason"))
print("=== content length:", len(content))
print("=== REASONING (first 300) ===")
print((msg.get("reasoning") or "")[:300])
print("=== CONTENT (first 800) ===")
print(content[:800])

# Now parse exactly like the app does
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
    print("head:", repr(text[:200]))
    print("tail:", repr(text[-200:]))
