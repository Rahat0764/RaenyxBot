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
firmware/RaenyxBot/RaenyxBot.ino   the ESP32 sketch (open this in Arduino IDE)
firmware/RaenyxBot/config.h        WiFi + server URL (edit before uploading)
server/app.py                      FastAPI server: Gemini proxy + song library
server/requirements.txt
server/songs/                      blue.mp3 / blue.lrc / blue.jpg go here
render.yaml                        one-click Render blueprint
```

## Setup

### 1. Server (Render)

1. Push this repo to GitHub (it already is) and in Render choose
   **New -> Blueprint**, pick the repo - `render.yaml` does the rest.
2. In the service's **Environment** tab set `GEMINI_API_KEY`
   (get one from Google AI Studio). Never commit the key.
3. Your server URL will be like `https://raenyxbot.onrender.com`.
4. Keep it awake: create a free **UptimeRobot** HTTP monitor for
   `https://raenyxbot.onrender.com/health` with a 5-minute interval,
   so Render's free tier never spins down (no cold starts).

### 2. Device

1. Open `firmware/RaenyxBot/RaenyxBot.ino` in the Arduino IDE.
2. Edit `config.h`: your WiFi name/password and `SERVER_BASE` (step 1.3).
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
