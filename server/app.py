"""
RaenyxBot server
================
Small FastAPI server that sits between the ESP32 device and the Gemini API.

What it does
------------
1. Voice chat brain (Gemini):
     POST /api/transcribe   wav upload  -> {"text": "..."}
     POST /api/chat         {"text", "history"} -> {"reply": "..."}
     POST /api/tts          {"text"}    -> audio/wav (Gemini TTS, 24 kHz PCM)

2. Song library (auto-discovered):
     Drop files into server/songs/ with the SAME base name:
         blue.mp3   blue.lrc   blue.jpg   (blue.json optional)
     The server scans the folder and builds the catalogue automatically.
         GET /songs.json        -> the catalogue the ESP32 downloads
         GET /songs/blue.mp3    -> streamed with HTTP Range support
         GET /songs/blue.lrc
         GET /songs/blue.jpg
     Optional blue.json: {"title": "...", "artist": "...",
                          "aliases": ["..."], "lrc_offset_ms": 0}
     If there is no .json, the title/artist are read from the LRC
     [ti:...] / [ar:...] tags, or guessed from the file name.

3. GET /health  -> {"status": "ok", "songs": n}   (UptimeRobot pings this)

Secrets come from the environment (Render dashboard), never from this repo
and never sent to the ESP32:

  GEMINI_API_KEYS     comma-separated list, e.g. "key1,key2,key3".
                      Calls rotate through the list: if one key fails or
                      hits its limit, the next key is tried automatically.
                      (The old single GEMINI_API_KEY also still works.)
  TELEGRAM_BOT_TOKEN  bot token from @BotFather.
  TELEGRAM_CHAT_IDS   comma-separated Telegram user/chat IDs. These IDs
                      receive the server logs AND are the only users the
                      bot answers (try /status, /songs, /keys in Telegram).
  SERVER_PUBLIC_URL   optional, e.g. https://raenyxbot.onrender.com -
                      if set, the Telegram webhook is registered on startup.
"""

import base64
import json
import os
import re
import struct
import threading
import time
import unicodedata
import urllib.request
from pathlib import Path

from fastapi import FastAPI, File, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel

# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------

BASE_DIR = Path(__file__).resolve().parent
SONGS_DIR = BASE_DIR / "songs"
SONGS_DIR.mkdir(exist_ok=True)

def _parse_csv(name: str) -> list[str]:
    return [p.strip() for p in os.environ.get(name, "").split(",") if p.strip()]


# Many keys, comma-separated. If a key fails or its limit is exhausted,
# the next one is used automatically (see run_gemini below).
GEMINI_API_KEYS = _parse_csv("GEMINI_API_KEYS") or _parse_csv("GEMINI_API_KEY")

TELEGRAM_BOT_TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
# These IDs get the logs and are the only users the bot answers.
TELEGRAM_CHAT_IDS = _parse_csv("TELEGRAM_CHAT_IDS") or _parse_csv(
    "TELEGRAM_AUTHORIZED_IDS"
)
SERVER_PUBLIC_URL = os.environ.get("SERVER_PUBLIC_URL", "").rstrip("/")

CHAT_MODEL = os.environ.get("GEMINI_CHAT_MODEL", "gemini-3.8-flash")
# If the primary chat model is overloaded (503 high demand) on every key,
# chat/transcribe fall back to these models, in order. A demand spike is
# per-model, so a quieter model usually answers immediately.
FALLBACK_MODELS = _parse_csv("GEMINI_FALLBACK_MODELS") or ["gemini-3.5-flash-lite"]
TTS_MODEL = os.environ.get("GEMINI_TTS_MODEL", "gemini-3.8-flash-tts")
TTS_VOICE = os.environ.get("GEMINI_TTS_VOICE", "Kore")

SYSTEM_PROMPT = (
    "You are Raenyx, a friendly voice assistant living inside a small device "
    "with a cute animated face. Always reply in the same language the user "
    "spoke: Bangla script for Bangla, English for English. Keep replies "
    "short, one to three sentences, because they will be spoken aloud. "
    "Plain text only: no markdown, no lists, no emojis."
)

app = FastAPI(title="RaenyxBot server")

START_TIME = time.time()

# ------------------------------------------------------------ Telegram log --
def _tg_send(chat_id: str, text: str):
    if not TELEGRAM_BOT_TOKEN:
        return
    try:
        data = json.dumps(
            {"chat_id": chat_id, "text": text[:4000], "disable_web_page_preview": True}
        ).encode()
        req = urllib.request.Request(
            f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage",
            data=data,
            headers={"Content-Type": "application/json"},
        )
        urllib.request.urlopen(req, timeout=10).read()
    except Exception as e:  # logging must never break the server
        print(f"[telegram] send failed: {e}")


def tg_log(text: str):
    """Send a log line to every authorized Telegram chat, in the background
    so a slow Telegram API never delays the ESP32's answer."""
    if not TELEGRAM_BOT_TOKEN or not TELEGRAM_CHAT_IDS:
        return

    def _run():
        for cid in TELEGRAM_CHAT_IDS:
            _tg_send(cid, text)

    threading.Thread(target=_run, daemon=True).start()


# ------------------------------------------------------- Gemini key pool --
_clients: dict[int, object] = {}
_key_idx = 0  # the key that worked most recently; calls start from here


def _client_for(idx: int):
    if idx not in _clients:
        from google import genai
        from google.genai import types

        # v1beta: the current TTS models (gemini-3.8-flash-tts) and the
        # Interactions API are only served there; the default v1 endpoint
        # answers "model not found" for them.
        _clients[idx] = genai.Client(
            api_key=GEMINI_API_KEYS[idx],
            http_options=types.HttpOptions(api_version="v1beta"),
        )
    return _clients[idx]


def run_gemini(fn, what: str):
    """Run fn(client) with the current Gemini key. If it fails (quota,
    limit, auth, server error), log it and try the next key, wrapping
    around the whole comma-separated list. Raises 502 if all keys fail."""
    global _key_idx
    if not GEMINI_API_KEYS:
        raise HTTPException(503, "GEMINI_API_KEYS is not set on the server")
    n = len(GEMINI_API_KEYS)
    start = _key_idx
    last_exc = None
    for step in range(n):
        idx = (start + step) % n
        try:
            out = fn(_client_for(idx))
            if idx != start:
                tg_log(f"✅ Gemini: switched to key #{idx + 1} of {n} ({what})")
            _key_idx = idx
            return out
        except HTTPException:
            raise
        except Exception as e:
            last_exc = e
            code = getattr(e, "code", None) or getattr(e, "status_code", None)
            print(f"[gemini] key #{idx + 1} failed ({what}, code={code}): {e}")
            tg_log(
                f"⚠️ Gemini key #{idx + 1} of {n} failed ({what}, code={code}). "
                f"Trying the next key...\n{e}"
            )
    tg_log(f"❌ All {n} Gemini keys failed ({what}):\n{last_exc}")
    raise HTTPException(502, f"all Gemini keys failed: {last_exc}")


def run_chat_model(fn, what: str):
    """fn(client, model) for chat/transcribe. Try the primary model across
    all keys first; if it fails everywhere (e.g. a 503 demand spike, which
    is per-model), retry with each fallback model so one busy model never
    silences the bot."""
    models = [CHAT_MODEL] + [m for m in FALLBACK_MODELS if m != CHAT_MODEL]
    last_exc = None
    for i, m in enumerate(models):
        try:
            return run_gemini(lambda c, m=m: fn(c, m), what)
        except HTTPException as e:
            last_exc = e
            if i + 1 < len(models):
                tg_log(f"↪️ {what}: {m} unavailable — falling back to {models[i + 1]}")
    raise last_exc


# --------------------------------------------------------------------------
# Song library: scan songs/ and build the catalogue automatically
# --------------------------------------------------------------------------

def normalize(text: str) -> str:
    """Lowercase, strip accents/punctuation - used for voice matching."""
    text = unicodedata.normalize("NFKD", text.lower())
    text = "".join(c for c in text if not unicodedata.combining(c))
    text = re.sub(r"[^a-z0-9 ]+", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def parse_lrc_tags(lrc_path: Path) -> dict:
    tags = {}
    try:
        head = lrc_path.read_text(encoding="utf-8", errors="ignore")[:2000]
    except OSError:
        return tags
    for key in ("ti", "ar", "al"):
        m = re.search(rf"\[{key}:\s*(.*?)\]", head, re.IGNORECASE)
        if m:
            tags[key] = m.group(1).strip()
    return tags


def mp3_duration(path: Path):
    try:
        from mutagen.mp3 import MP3

        return round(MP3(path).info.length, 1)
    except Exception:
        return None


def scan_songs() -> list[dict]:
    """Every *.mp3 in songs/ becomes a song. Matching .lrc/.jpg/.json by
    base name are picked up automatically - no manual editing anywhere."""
    songs = []
    for mp3 in sorted(SONGS_DIR.glob("*.mp3")):
        base = mp3.stem
        lrc = SONGS_DIR / f"{base}.lrc"
        jpg = SONGS_DIR / f"{base}.jpg"
        meta_path = SONGS_DIR / f"{base}.json"

        meta = {}
        if meta_path.exists():
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                meta = {}

        tags = parse_lrc_tags(lrc) if lrc.exists() else {}

        title = meta.get("title") or tags.get("ti") or ""
        artist = meta.get("artist") or tags.get("ar") or ""
        if not title:
            # Guess from the file name: "Blue - Yung Kai" / "blue_by_yung_kai"
            pretty = base.replace("_", " ").replace("-", " - ")
            parts = [p.strip() for p in pretty.split(" - ") if p.strip()]
            if len(parts) >= 2:
                title, artist = parts[0], parts[1]
            else:
                title = pretty.strip()

        aliases = set()
        for raw in (
            base,
            base.replace("_", " ").replace("-", " "),
            title,
            artist,
            f"{title} by {artist}" if artist else "",
            f"{title} {artist}" if artist else "",
            *meta.get("aliases", []),
        ):
            n = normalize(raw)
            if n:
                aliases.add(n)

        songs.append(
            {
                "id": base,
                "title": title,
                "artist": artist,
                "aliases": sorted(aliases),
                "duration_sec": mp3_duration(mp3),
                "lrc_offset_ms": int(meta.get("lrc_offset_ms", 0)),
                "has_lrc": lrc.exists(),
                "has_image": jpg.exists(),
                "mp3_url": f"/songs/{base}.mp3",
                "lrc_url": f"/songs/{base}.lrc" if lrc.exists() else None,
                "image_url": f"/songs/{base}.jpg" if jpg.exists() else None,
            }
        )
    return songs


@app.get("/")
def root():
    return {"service": "RaenyxBot", "songs": len(scan_songs())}


@app.get("/health")
def health():
    # UptimeRobot pings this every 5 minutes so Render never spins down.
    return {
        "status": "ok",
        "songs": len(scan_songs()),
        "gemini_keys": len(GEMINI_API_KEYS),
        "active_key": _key_idx + 1 if GEMINI_API_KEYS else 0,
        "telegram_log": bool(TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_IDS),
    }


@app.on_event("startup")
def _startup():
    songs = scan_songs()
    names = ", ".join(s["id"] for s in songs) or "(none yet)"
    tg_log(
        "🟢 RaenyxBot server started\n"
        f"🔑 Gemini keys loaded: {len(GEMINI_API_KEYS)}\n"
        f"🎵 Songs ({len(songs)}): {names}"
    )
    # Register the Telegram webhook so authorized users can send commands.
    if TELEGRAM_BOT_TOKEN and SERVER_PUBLIC_URL:
        def _set_hook():
            try:
                data = json.dumps(
                    {"url": f"{SERVER_PUBLIC_URL}/telegram/webhook"}
                ).encode()
                req = urllib.request.Request(
                    f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/setWebhook",
                    data=data,
                    headers={"Content-Type": "application/json"},
                )
                urllib.request.urlopen(req, timeout=10).read()
                print("[telegram] webhook registered")
            except Exception as e:
                print(f"[telegram] setWebhook failed: {e}")

        threading.Thread(target=_set_hook, daemon=True).start()


@app.post("/telegram/webhook")
async def telegram_webhook(request: Request):
    """Commands from authorized Telegram users only: /status /songs /keys."""
    try:
        update = await request.json()
    except Exception:
        return {"ok": True}
    msg = update.get("message") or update.get("edited_message") or {}
    uid = str((msg.get("from") or {}).get("id", ""))
    chat_id = str((msg.get("chat") or {}).get("id", ""))
    text = (msg.get("text") or "").strip()
    if not text:
        return {"ok": True}
    if uid not in TELEGRAM_CHAT_IDS and chat_id not in TELEGRAM_CHAT_IDS:
        tg_log(f"🚫 Unauthorized Telegram user {uid} tried: {text}")
        return {"ok": True}

    cmd = text.split()[0].lower()
    if cmd in ("/start", "/status"):
        up = int(time.time() - START_TIME)
        reply = (
            "🤖 RaenyxBot server is running\n"
            f"⏱ Uptime: {up // 3600}h {(up % 3600) // 60}m\n"
            f"🔑 Gemini keys: {len(GEMINI_API_KEYS)} (active #{_key_idx + 1})\n"
            f"🎵 Songs: {len(scan_songs())}"
        )
    elif cmd == "/songs":
        songs = scan_songs()
        lines = [f"🎵 {s['title']} - {s['artist']}".rstrip(" -") for s in songs]
        reply = "\n".join(lines) if lines else "No songs yet."
    elif cmd == "/keys":
        reply = (
            f"🔑 {len(GEMINI_API_KEYS)} Gemini keys loaded, "
            f"currently using #{_key_idx + 1}. "
            "A failing key is skipped automatically."
        )
    else:
        reply = "Commands: /status  /songs  /keys"
    _tg_send(chat_id, reply)
    return {"ok": True}


@app.get("/songs.json")
def songs_json():
    # Rescanned on every request: add files, they appear. Nothing to edit.
    return {"songs": scan_songs()}


@app.get("/songs/{filename}")
def song_file(filename: str):
    if "/" in filename or "\\" in filename or ".." in filename:
        raise HTTPException(404, "not found")
    if Path(filename).suffix.lower() not in (".mp3", ".lrc", ".jpg"):
        raise HTTPException(404, "not found")
    path = SONGS_DIR / filename
    if not path.exists():
        raise HTTPException(404, "not found")
    media = {
        ".mp3": "audio/mpeg",
        ".lrc": "text/plain; charset=utf-8",
        ".jpg": "image/jpeg",
    }[Path(filename).suffix.lower()]
    # FileResponse honours HTTP Range requests, so the ESP32 can stream.
    return FileResponse(path, media_type=media)


# --------------------------------------------------------------------------
# Gemini: transcription, chat, text-to-speech
# --------------------------------------------------------------------------

class ChatIn(BaseModel):
    text: str
    history: list[dict] = []  # [{"role": "user"|"assistant", "content": "..."}]


class TtsIn(BaseModel):
    text: str
    voice: str | None = None


@app.post("/api/transcribe")
async def transcribe(file: UploadFile = File(...)):
    from google.genai import types

    audio = await file.read()
    if len(audio) < 1000:
        raise HTTPException(400, "audio too short")

    def _do(client, model):
        return client.models.generate_content(
            model=model,
            contents=[
                types.Part.from_bytes(data=audio, mime_type="audio/wav"),
                "Transcribe this audio exactly as spoken, word for word. "
                "Reply with the transcription only, no comments.",
            ],
        )

    resp = run_chat_model(_do, "transcribe")
    return {"text": (resp.text or "").strip()}


@app.post("/api/chat")
def chat(body: ChatIn):
    from google.genai import types

    contents = []
    for h in body.history[-8:]:
        role = "model" if h.get("role") == "assistant" else "user"
        contents.append(
            types.Content(role=role, parts=[types.Part(text=h.get("content", ""))])
        )
    contents.append(types.Content(role="user", parts=[types.Part(text=body.text)]))

    def _do(client, model):
        return client.models.generate_content(
            model=model,
            contents=contents,
            config=types.GenerateContentConfig(
                system_instruction=SYSTEM_PROMPT,
                temperature=0.7,
                max_output_tokens=300,
            ),
        )

    resp = run_chat_model(_do, "chat")
    reply = (resp.text or "").strip()
    reply = re.sub(r"[*#`_>]+", "", reply).strip()
    if not reply:
        raise HTTPException(502, "empty reply from Gemini")
    tg_log(f"💬 {body.text}\n🤖 {reply}")
    return {"reply": reply}


def wrap_wav(pcm: bytes, rate: int = 24000) -> bytes:
    """Gemini TTS returns raw 16-bit mono PCM; the ESP32 wants a WAV file."""
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + len(pcm), b"WAVE", b"fmt ", 16, 1, 1,
        rate, rate * 2, 2, 16, b"data", len(pcm),
    )
    return header + pcm


@app.post("/api/tts")
def tts(body: TtsIn):
    text = body.text.strip()
    if not text:
        raise HTTPException(400, "empty text")

    def _do(client):
        # Gemini 3.8 TTS runs on the Interactions API (the old generateContent
        # speechConfig path is rejected for the new models). output_audio
        # already carries a complete WAV file, base64-encoded.
        interaction = client.interactions.create(
            model=TTS_MODEL,
            input=[{
                "type": "user_input",
                "content": [{"type": "text", "text": text}],
            }],
            response_format={"type": "audio"},
            generation_config={
                "speech_config": [{"voice": body.voice or TTS_VOICE}]
            },
        )
        out = getattr(interaction, "output_audio", None)
        if out is None or not getattr(out, "data", None):
            raise RuntimeError("no audio from Gemini TTS")
        return base64.b64decode(out.data)

    wav = run_gemini(_do, "tts")
    return Response(content=wav, media_type="audio/wav")
