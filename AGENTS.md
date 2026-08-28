# MIO — AI Robot Companion (ESP32-S3)

## Project Overview

MIO is a voice-controlled AI companion built on an **ESP32-S3** microcontroller. It connects over WiFi to a **Flask cloud brain** (hosted on Render.com) that runs an LLM (via Ollama-compatible API), performs TTS synthesis (gTTS), and returns structured JSON commands. The ESP32 executes those commands to control an RGB LED, a 128×128 ST7735 TFT display, and a PWM speaker.

---

## ESP32 Skill — ACTIVE

> **This project uses the ESP32 embedded systems skill.**  
> See `skills/esp32/SKILL.md` for full guidelines on chip selection, GPIO validation, code generation, and safety checks.

**Target chip:** `ESP32-S3` (WROOM module)  
**Framework:** Arduino (via Arduino IDE / arduino-esp32 core)  
**WiFi:** Enabled — ADC2 conflict rules apply  

### Quick Reference: MIO Hardware Pin Map

| GPIO | Function | Device | Notes |
|------|----------|--------|-------|
| 14 | SPI CLK (TFT_CLK) | ST7735 TFT | HSPI CLK default |
| 21 | SPI MOSI (TFT_MOSI) | ST7735 TFT | GPIO matrix routed |
| 47 | TFT CS | ST7735 TFT | Safe GPIO |
| 40 | TFT DC | ST7735 TFT | Safe GPIO |
| 38 | TFT RST | ST7735 TFT | ⚠ Strapping pin on S3 — safe after boot |
| 15 | TFT Backlight | ST7735 TFT | PWM-capable |
| 48 | NeoPixel RGB LED | WS2812B | Built-in on many S3 dev boards |
| 16 | PWM Speaker | Piezo/speaker | ledcAttach @ 120kHz, 8-bit |
| 19 | USB D- | Native USB | Do not reassign |
| 20 | USB D+ | Native USB | Do not reassign |

### Known Safety Constraints (ESP32-S3)

- **GPIO 26–32**: Flash pins — **NEVER USE**
- **GPIO 0, 3, 45, 46**: Strapping pins — boot-sensitive, avoid driving at startup
- **ADC2**: Unavailable when WiFi is active (MIO always has WiFi on)
- **GPIO 19/20**: Reserved for native USB OTG

---

## Project Structure

```
mio_project/
├── AGENTS.md                     ← You are here (project instructions for AI)
├── sketch_aug22a/
│   └── sketch_aug22a.ino         ← ESP32-S3 Arduino firmware
├── mio_cloud_brain/
│   ├── app.py                    ← Flask cloud AI server (Render.com)
│   └── requirements.txt
├── .agents/
│   ├── skills/
│   │   └── esp32/
│   │       └── SKILL.md              ← ESP32 embedded skill (auto-loaded)
│   └── references/                   ← Hardware reference docs (loaded on demand)
│   ├── platforms/
│   │   ├── esp32-pins.md         ← GPIO database (always load)
│   │   └── esp32-specifics.md    ← Strapping, ADC, flash, PSRAM
│   ├── esp32-s3/specs.md         ← S3-specific constraints
│   ├── protocol-quick-ref.md     ← I2C, SPI, UART, PWM, ADC
│   ├── electrical-constraints.md ← Voltage, current, pull-ups
│   ├── common-devices.md         ← ST7735, WS2812B, gTTS, etc.
│   ├── lvgl/                     ← LVGL GUI references (v8.2–v9.5)
│   └── waveshare/                ← Waveshare board/display refs
└── scripts/
    ├── validate_pinmap.py        ← GPIO safety validator
    └── generate_config.py        ← Arduino/ESP-IDF boilerplate generator
```

---

## Architecture

```
User Voice/Text
      │
      ▼
[ESP32-S3 Web UI] ──HTTP──▶ [ESP32-S3 /command]
                                    │
                              [FreeRTOS Task]
                                    │ HTTPS POST
                                    ▼
                          [Render.com Flask Server]
                            app.py / ask_ollama()
                                    │
                            [Ollama LLM API]
                            (gemma4:cloud or any model)
                                    │
                          ┌─────────┴──────────┐
                          │  Structured JSON    │
                          │  + gTTS MP3 URL     │
                          └─────────┬──────────┘
                                    │ HTTP response
                                    ▼
                              [ESP32-S3]
                         ┌────────────────────┐
                         │  RGB LED control   │
                         │  TFT display anim  │
                         │  PWM audio playback│
                         └────────────────────┘
```

---

## Cloud Brain API

**Endpoint:** `POST /ask`  
**Content-Type:** `application/json`

**Request:**
```json
{ "text": "turn on red light" }
```

**Response:**
```json
{
  "success": true,
  "reply": "Sure, red light coming right up!",
  "action": "set_light",
  "r": 255, "g": 0, "b": 0,
  "mode": "solid",
  "speed_ms": 120,
  "brightness": 255,
  "screen": "face_happy",
  "audio_url": "https://mio-brain.onrender.com/static/tts_abc12345.mp3"
}
```

**Supported screen values:** `none`, `dog_running`, `clear`, `face_idle`, `face_happy`, `face_sad`, `face_angry`  
**Supported action values:** `none`, `set_light`  
**Supported mode values:** `solid`, `blink`, `breathe`, `off`  
**Supported tools:** `weather`, `joke`

---

## Environment Variables (Render.com)

| Variable | Description | Example |
|----------|-------------|---------|
| `OLLAMA_URL` | LLM API endpoint | `https://your-ollama.onrender.com/api/chat` |
| `OLLAMA_API_KEY` | Bearer token (if needed) | `sk-...` |
| `OLLAMA_MODEL` | Model name | `gemma4:cloud` |

---

## Firmware States

| State | TFT Display | RGB LED |
|-------|-------------|---------|
| `IDLE` | Pulsing orb animation | Off / last color |
| `LISTENING` | Vertical bar waveform | — |
| `THINKING` | Rotating dot orbit | — |
| `SPEAKING` | Blocked (playing MP3) | — |
| `ERROR_STATE` | Red "ERROR" screen | — |

---

## Libraries Required (Arduino IDE)

- `Adafruit GFX`
- `Adafruit ST7735 and ST7789`
- `Adafruit NeoPixel`
- `ArduinoJson`
- `EspUsbHost` (from Library Manager)
- `ESP8266Audio` (for MP3 playback via `AudioGeneratorMP3`)

---

## Development Notes

- The ESP32-S3 **native USB** is on GPIO 19/20 — do not reassign these
- The `AudioOutputPWM` class uses `ledcAttach()` — requires **arduino-esp32 core 3.x** syntax
- TTS audio files accumulate in `static/` — add a cleanup cron if deploying long-term
- WiFi must connect before any HTTP calls; fallback AP (`MIO_NETWORK`) runs in parallel
- Chat history is stored **server-side** in `chat_history[]` (last 4 turns = 8 messages)
- USB flash drive logging uses `EspUsbHostMscFS` — requires FAT32 formatted drive

---

## Validation

To validate a pin assignment against ESP32-S3 constraints:

```bash
echo '{
  "platform": "esp32s3",
  "wifi_enabled": true,
  "pins": [
    {"gpio": 14, "function": "SPI_CLK",  "protocol_bus": "spi", "device": "ST7735"},
    {"gpio": 21, "function": "SPI_MOSI", "protocol_bus": "spi", "device": "ST7735"},
    {"gpio": 48, "function": "NEOPIXEL", "protocol_bus": "gpio","device": "WS2812B"},
    {"gpio": 16, "function": "PWM_AUDIO","protocol_bus": "pwm", "device": "Speaker"}
  ]
}' | python scripts/validate_pinmap.py
```
