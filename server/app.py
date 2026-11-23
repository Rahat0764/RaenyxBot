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

Secrets: GEMINI_API_KEY is read from the environment (Render dashboard).
It is never stored in this repo and never sent to the ESP32.
"""

import io
import json
import os
import re
import struct
import unicodedata
from pathlib import Path

from fastapi import FastAPI, File, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse, Response
from pydantic import BaseModel

# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------

BASE_DIR = Path(__file__).resolve().parent
SONGS_DIR = BASE_DIR / "songs"
SONGS_DIR.mkdir(exist_ok=True)

GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY", "")
CHAT_MODEL = os.environ.get("GEMINI_CHAT_MODEL", "gemini-2.5-flash")
TTS_MODEL = os.environ.get("GEMINI_TTS_MODEL", "gemini-2.5-flash-preview-tts")
TTS_VOICE = os.environ.get("GEMINI_TTS_VOICE", "Kore")

SYSTEM_PROMPT = (
    "You are Raenyx, a friendly voice assistant living inside a small device "
    "with a cute animated face. Always reply in the same language the user "
    "spoke: Bangla script for Bangla, English for English. Keep replies "
    "short, one to three sentences, because they will be spoken aloud. "
    "Plain text only: no markdown, no lists, no emojis."
)

app = FastAPI(title="RaenyxBot server")

_gemini_client = None


def gemini():
    """Lazy-init the Gemini client so /health and /songs work without a key."""
    global _gemini_client
    if _gemini_client is None:
        if not GEMINI_API_KEY:
            raise HTTPException(503, "GEMINI_API_KEY is not set on the server")
        from google import genai

        _gemini_client = genai.Client(api_key=GEMINI_API_KEY)
    return _gemini_client


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
    return {"status": "ok", "songs": len(scan_songs())}


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
    resp = gemini().models.generate_content(
        model=CHAT_MODEL,
        contents=[
            types.Part.from_bytes(data=audio, mime_type="audio/wav"),
            "Transcribe this audio exactly as spoken, word for word. "
            "Reply with the transcription only, no comments.",
        ],
    )
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

    resp = gemini().models.generate_content(
        model=CHAT_MODEL,
        contents=contents,
        config=types.GenerateContentConfig(
            system_instruction=SYSTEM_PROMPT,
            temperature=0.7,
            max_output_tokens=300,
        ),
    )
    reply = (resp.text or "").strip()
    reply = re.sub(r"[*#`_>]+", "", reply).strip()
    if not reply:
        raise HTTPException(502, "empty reply from Gemini")
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
    from google.genai import types

    text = body.text.strip()
    if not text:
        raise HTTPException(400, "empty text")
    resp = gemini().models.generate_content(
        model=TTS_MODEL,
        contents=text,
        config=types.GenerateContentConfig(
            response_modalities=["AUDIO"],
            speech_config=types.SpeechConfig(
                voice_config=types.VoiceConfig(
                    prebuilt_voice_config=types.PrebuiltVoiceConfig(
                        voice_name=body.voice or TTS_VOICE
                    )
                )
            ),
        ),
    )
    pcm = resp.candidates[0].content.parts[0].inline_data.data
    if not pcm:
        raise HTTPException(502, "no audio from Gemini TTS")
    return Response(content=wrap_wav(pcm), media_type="audio/wav")
