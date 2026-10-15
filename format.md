# ears format

Speech-to-text output for audio and video files. Full text is derived by joining segment `text` values. Language is auto-detected.

```json
{
  "media": {
    "durationSec": 3.42,
    "sampleRate": 16000,
    "channels": 1
  },
  "transcript": {
    "language": "en",
    "segments": [
      {
        "start": 0.0,
        "end": 3.1,
        "text": " Hello, this is a test of the ears speech transcription module.",
        "confidence": 0.93,
        "noSpeech": 0.01,
        "tokens": [
          {
            "text": " Hello",
            "start": 0.0,
            "end": 0.32,
            "confidence": 0.95
          },
          {
            "text": ",",
            "start": 0.32,
            "end": 0.36,
            "confidence": 0.91
          }
        ]
      }
    ]
  }
}
```
