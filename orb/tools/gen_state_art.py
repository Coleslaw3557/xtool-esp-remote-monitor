#!/usr/bin/env python3
"""Generate the orb's state illustrations via OpenRouter image models.
Usage: gen_art.py <state>... | all | anchor
Anchor (idle) is generated first; every other state passes it as a style ref."""
import base64, json, os, sys, time, urllib.request

KEY = os.environ["OPENROUTER_API_KEY"]
MODEL = os.environ.get("OR_IMAGE_MODEL", "google/gemini-3-pro-image")
OUT = os.environ.get("ART_OUT", "/tmp/orb_state_art")
os.makedirs(OUT, exist_ok=True)

STYLE = (
    "Cute kawaii mascot illustration for a small round display. "
    "Character: a friendly little laser-cutter robot buddy - soft rounded teal and cream "
    "body shaped like a tiny enclosed laser cutter with a gentle face on its front panel, "
    "big glossy dark eyes, tiny orange safety goggles perched on top, small stubby arms. "
    "Style: flat vector kawaii with soft two-tone shading, thick clean outlines, limited "
    "palette of at most 20 colors, no photorealism, no fine texture. "
    "Background: plain very dark navy blue, gently darkening toward the edges, a few tiny "
    "pale stars allowed. Composition: square image, character centered in the UPPER 60 "
    "percent, bottom third mostly empty dark background. "
    "Absolutely no letters, numbers, words, logos, or watermarks anywhere in the image."
)

STATES = {
    "idle": "The buddy is relaxed and content, holding a tiny steaming mug of tea with both arms, eyes half-closed happy.",
    "searching": "The buddy holds a big glowing flashlight, peering into the dark with curious wide eyes; three soft pale question-mark-shaped sparkles float above (stylized sparkles, not typographic characters).",
    "sleeping": "The buddy is fast asleep, eyes closed as gentle arcs, with three soft pale rounded bubbles floating up above it, cozy vibe.",
    "ready": "The buddy stands alert and excited, one arm raised high holding a small solid-green triangular pennant flag, sparkles of anticipation around it.",
    "framing": "The buddy points a thin pale-green laser pointer beam downward, tracing a glowing dashed rectangle outline floating in front of it.",
    "cutting": "The buddy has the orange safety goggles pulled down over its eyes, focused, projecting a bright thin orange laser beam downward onto a small wooden board, with tiny orange sparks flying where the beam hits.",
    "paused": "The buddy leans back taking a break, holding a big rounded white pause symbol made of two vertical bars, calm expression.",
    "done": "The buddy celebrates joyfully, both arms up, holding a small wooden heart shape it just cut, with colorful confetti pieces falling around it.",
    "error": "The buddy looks worried with big concerned eyes and tiny sweat drop, next to a floating soft-red rounded warning triangle with an exclamation shape inside (iconic, not typographic).",
}

def call(prompt, ref_png=None, retries=3):
    content = [{"type": "text", "text": prompt}]
    if ref_png:
        b64 = base64.b64encode(open(ref_png, "rb").read()).decode()
        content.append({"type": "image_url", "image_url": {"url": "data:image/png;base64," + b64}})
    body = {
        "model": MODEL,
        "messages": [{"role": "user", "content": content}],
        "modalities": ["image", "text"],
    }
    req = urllib.request.Request(
        "https://openrouter.ai/api/v1/chat/completions",
        data=json.dumps(body).encode(),
        headers={"Authorization": "Bearer " + KEY, "Content-Type": "application/json"},
    )
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=180) as r:
                d = json.load(r)
            msg = d["choices"][0]["message"]
            imgs = msg.get("images") or []
            if not imgs:
                raise RuntimeError("no image in response: " + json.dumps(msg)[:300])
            url = imgs[0]["image_url"]["url"]
            b64 = url.split(",", 1)[1]
            return base64.b64decode(b64)
        except Exception as e:
            print(f"  attempt {attempt+1} failed: {e}", flush=True)
            time.sleep(4)
    raise RuntimeError("all attempts failed")

def gen(state):
    anchor = f"{OUT}/idle.png"
    if state == "idle":
        prompt = STYLE + " Scene: " + STATES["idle"]
        ref = None
    else:
        prompt = (
            "Use the exact same character, art style, palette, background and framing as "
            "the reference image. Same buddy, new scene: " + STATES[state] + " " +
            "Keep the bottom third of the image mostly empty dark background. "
            "No letters, numbers or words anywhere."
        )
        ref = anchor
    print(f"[{state}] generating via {MODEL}...", flush=True)
    png = call(prompt, ref)
    open(f"{OUT}/{state}.png", "wb").write(png)
    print(f"[{state}] saved {len(png)} bytes", flush=True)

if __name__ == "__main__":
    args = sys.argv[1:] or ["all"]
    if args == ["anchor"]:
        todo = ["idle"]
    elif args == ["all"]:
        todo = list(STATES)
    else:
        todo = args
    for s in todo:
        gen(s)
