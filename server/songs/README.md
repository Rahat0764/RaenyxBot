# Song library

Drop song files in this folder. The server scans it automatically and
rebuilds `/songs.json` on every request - **you never edit songs.json by
hand.**

For each song, use the **same base name** for every file:

| File        | Required | What it is                                            |
|-------------|----------|-------------------------------------------------------|
| `blue.mp3`  | yes      | The song (128-192 kbps is plenty for the small speaker) |
| `blue.lrc`  | yes      | Timed lyrics: `[00:12.50] lyric line`                  |
| `blue.jpg`  | no       | Background image, exactly **320x240**, baseline JPG    |
| `blue.json` | no       | Extra info, see below                                  |

Optional `blue.json`:

```json
{
  "title": "Blue",
  "artist": "Yung Kai",
  "aliases": ["blue by yungkai"],
  "lrc_offset_ms": 0
}
```

- Without a `.json`, the title/artist are read from the LRC `[ti:...]` /
  `[ar:...]` tags, or guessed from the file name.
- `aliases` are extra names the voice matcher should accept.
- `lrc_offset_ms`: shift all lyrics earlier (negative) or later (positive)
  if they feel out of sync on the device.
- The voice matcher ignores case, punctuation and spaces, so
  "play the song blue", "Blue by Yung Kai" and "blue bajao" style requests
  can all find the same song as long as an alias covers the words used.

> Note: songs are committed to this GitHub repo and deployed to Render
> together with the server. Keep the repo's visibility in mind before
> adding music you do not have the rights to share.
