# RaenyxBot

An ESP32-S3 voice assistant with a cute animated face. It listens, talks
with the **Gemini API** (through its own small server), speaks with a
realistic Gemini TTS voice with **lip sync**, and has a **song mode** with
background artwork and real-time synced lyrics.

```
[ESP32 device]  --WiFi-->  [RaenyxBot server on Render]  -->  [Gemini API]
  face, mic,                  Gemini key lives here only        chat + TTS
  speaker, songs              song library (this repo)
```

## Features

- **English greeting**, then replies in whatever language you speak
  (Bangla or English), short and spoken-style
- **Cute face** - sparkle eyes, blush cheeks, blinking, expressions for
  idle / listening / thinking / speaking / error
- **Lip sync** - the mouth follows the loudness of the voice being played
- **Song mode** - say *"play the song blue"* or *"blue by yung kai"*:
  - streams the MP3 from the server
  - shows the song's JPG as the display background
  - shows the `.lrc` lyrics line by line, in sync with the music
  - big on-screen **pause button**: touch it, the music pauses and the
    device listens - say **"resume"** to continue, or
    **"back to homescreen"** to return to the face and keep chatting
- **Auto song library** - the server scans `server/songs/` and rebuilds
  `songs.json` by itself. Adding a song = adding files, never editing JSON
  by hand (see `server/songs/README.md`)

## Repo layout

```
firmware/RaenyxBot/RaenyxBot.ino   the ESP32 sketch - the ONLY file you upload
server/app.py                      FastAPI server: Gemini proxy + song library
server/requirements.txt
server/songs/                      blue.mp3 / blue.lrc / blue.jpg go here
render.yaml                        one-click Render blueprint
```

## Setup

### 1. Server (Render)

1. Push this repo to GitHub (it already is) and in Render choose
   **New -> Blueprint**, pick the repo - `render.yaml` does the rest.
2. In the service's **Environment** tab set the variables below.
   Never commit real values.

   | Variable | Value |
   |---|---|
   | `GEMINI_API_KEYS` | One or more Gemini keys, **comma-separated**: `key1,key2,key3`. If a key fails or hits its limit, the server automatically tries the next one and remembers the working key. If the main model is overloaded, chat/transcribe fall back to `gemini-3.5-flash-lite`. |
   | `GROQ_API_KEYS` | Optional, comma-separated Groq keys from [console.groq.com](https://console.groq.com). If **every** Gemini path fails, transcribe (Whisper `whisper-large-v3-turbo`) and chat (`openai/gpt-oss-120b`) automatically fall back to Groq — the provider this bot ran on originally. TTS stays on Gemini (Groq's TTS has no Bangla voice). Use fresh keys — never ones that were shared in chat. |
   | `TELEGRAM_BOT_TOKEN` | Bot token from [@BotFather](https://t.me/BotFather) - enables Telegram logs. |
   | `TELEGRAM_CHAT_IDS` | Comma-separated Telegram user IDs that **receive the server logs** and are the only users the bot answers. Get your ID from [@userinfobot](https://t.me/userinfobot). |
   | `SERVER_PUBLIC_URL` | This service's own URL - the Telegram webhook is registered automatically on startup. |

   With Telegram set up you get a log message on server start, for every
   chat question/answer, and whenever a Gemini key fails over - and you
   can send the bot `/status`, `/songs` and `/keys`. Anyone not in
   `TELEGRAM_CHAT_IDS` is ignored.
3. Your server URL will be like `https://raenyxbot.onrender.com`.
4. Keep it awake: create a free **UptimeRobot** HTTP monitor for
   `https://raenyxbot.onrender.com/health` with a 5-minute interval,
   so Render's free tier never spins down (no cold starts).

### 2. Device

1. Open `firmware/RaenyxBot/RaenyxBot.ino` in the Arduino IDE.
2. At the **top of the .ino file** fill in the three marked lines:
   `WIFI_SSID`, `WIFI_PASS` and `SERVER_BASE` (your Render URL, step 1.3).
   Nothing else needs editing - it is a single-file sketch.
3. Libraries: **LovyanGFX**, **ArduinoJson**, **ESP8266Audio**.
   Board: ESP32-S3, **PSRAM enabled**.
4. Upload, touch the screen, and talk.

### 3. Add a song

Put `name.mp3` + `name.lrc` (+ optional `name.jpg`, exactly 320x240,
baseline JPG) into `server/songs/`, commit and push. Render redeploys and
the song is playable - the device fetches the fresh `songs.json` at boot.

## Notes / known limits (v1)

- The device uses `setInsecure()` TLS towards your own server for now;
  pinning Render's root CA is a planned hardening step.
- Song position (and lyric sync) is timed from when streaming starts;
  if a song's lyrics feel early/late, set `lrc_offset_ms` in that song's
  optional `.json` file - no code change needed.
- Pausing holds the MP3 stream open. Very long pauses can make the
  server connection drop; if that happens, just say the song command
  again. (The pause is meant for giving a quick voice command.)
- Song files are committed to this repo - keep its visibility in mind
  for music you do not have the rights to share.
