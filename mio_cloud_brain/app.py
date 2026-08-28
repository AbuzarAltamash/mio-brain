import os
from flask import Flask, request, jsonify, url_for
import requests, json, re, datetime, uuid
import urllib.parse
from gtts import gTTS

app = Flask(__name__, static_folder='static')
os.makedirs('static', exist_ok=True)

# Use Environment Variables for Cloud Deployment!
# You will set these in the Render.com Dashboard
OLLAMA_URL = os.environ.get("OLLAMA_URL", "https://ollama.com/api/chat")
OLLAMA_API_KEY = os.environ.get("OLLAMA_API_KEY", "")
MODEL = os.environ.get("OLLAMA_MODEL", "gemma4:cloud")

SYSTEM = """
You are MIO, a warm, extremely friendly, and enthusiastic AI companion controlling an ESP32. Keep answers very short and conversational but no emojis or emoticons allowed. Always sound happy to help!
Understand natural language; never require exact phrases.
You can use tools to fetch real-world information.

Return ONLY one valid JSON object with exactly these keys:
{
 "reply":"short natural response",
 "action":"none or set_light",
 "r":0,"g":0,"b":0,
 "mode":"solid or blink or breathe or off",
 "speed_ms":120,
 "brightness":255,
 "screen":"none or dog_running or clear or face_idle or face_happy or face_sad or face_angry or face_surprised or face_suspicious or face_sleepy",
 "sound":"none or beep or success or error or alarm",
 "tool":"none or weather or joke",
 "tool_args":""
}

LIGHT INTERPRETATION:
- Any request about changing, turning, dimming, brightening, blinking, pulsing, breathing, fading, flashing, or smoothly turning an RGB light is action="set_light".
- Convert requested/implied colors to RGB creatively.
- "blink fast": blink with speed_ms around 80-180.
- "blink slow": around 500-1200.
- "dim": lower brightness appropriately, usually 20-100, while preserving requested/current implied color if specified.
- "bright": brightness 200-255.
- "smooth", "fade", "pulse", "breathe", "smooth on and off": mode="breathe".
- "turn off": action="set_light", mode="off", RGB 0,0,0, brightness 0.
- "turn on" without a color should use a sensible white unless context provides a color.

SCREEN INTERPRETATION:
- To show expressive robot eyes, set screen to one of: "face_idle", "face_happy", "face_sad", "face_angry", "face_surprised", "face_suspicious", "face_sleepy".
- Choose the emotion that perfectly matches your reply. Get creative!

SOUND INTERPRETATION:
- Use "success" for completing a task (like turning on a light).
- Use "error" if a tool fails or you can't do something.
- Use "alarm" for serious alerts.
- Use "beep" for minor acknowledgments.
- Use "none" for normal conversation without any special sound effects.

TOOL USAGE:
- If you need to know the current weather, set "tool":"weather" and "tool_args":"City Name". (If no city is given, leave it empty).
- If you are asked to tell a joke, set "tool":"joke".

Keep reply short. No markdown. No extra text outside JSON.
"""

def clean_json(text):
    text = re.sub(r"^```(?:json)?|```$", "", text.strip(), flags=re.I).strip()
    try: return json.loads(text)
    except Exception:
        m = re.search(r"\{.*\}", text, re.S)
        if not m: raise
        return json.loads(m.group(0))

def clamp(v, lo, hi, default):
    try: return max(lo, min(hi, int(v)))
    except Exception: return default

# Simple conversational memory (held on the server!)
chat_history = []

def run_tool(tool_name, tool_args):
    try:
        if tool_name == "joke":
            r = requests.get("https://official-joke-api.appspot.com/random_joke", timeout=5)
            if r.status_code == 200:
                d = r.json()
                return f"{d['setup']} ... {d['punchline']}"
            return "Joke API failed."
        elif tool_name == "weather":
            lat, lon = None, None
            if not tool_args or tool_args.strip() == "":
                r = requests.get("https://ipapi.co/json/", timeout=5)
                if r.status_code == 200:
                    d = r.json()
                    lat, lon = d.get("latitude"), d.get("longitude")
                    tool_args = d.get("city", "your location")
            else:
                r = requests.get(f"https://geocoding-api.open-meteo.com/v1/search?name={urllib.parse.quote(tool_args)}&count=1", timeout=5)
                if r.status_code == 200 and r.json().get("results"):
                    res = r.json()["results"][0]
                    lat, lon = res["latitude"], res["longitude"]

            if lat is not None and lon is not None:
                r = requests.get(f"https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current_weather=true", timeout=5)
                if r.status_code == 200:
                    w = r.json().get("current_weather", {})
                    return f"Weather in {tool_args}: {w.get('temperature')}C, wind {w.get('windspeed')}km/h."
            return f"Could not find weather data for {tool_args}."
    except Exception as e:
        print(f"Tool {tool_name} error: {e}")
        return f"Tool {tool_name} encountered an error."
    return "Unknown tool."

def ask_ollama(messages):
    headers = {"Content-Type": "application/json"}
    if OLLAMA_API_KEY:
        headers["Authorization"] = f"Bearer {OLLAMA_API_KEY}"
        
    try:
        res = requests.post(OLLAMA_URL, headers=headers, json={
            "model": MODEL, 
            "messages": messages, 
            "stream": False, 
            "format": "json",
            "options": {"temperature": 0.2}
        }, timeout=120)
        res.raise_for_status()
        return clean_json(res.json().get("message", {}).get("content", ""))
    except Exception as e:
        print("AI ERROR:",repr(e))
        return None

@app.post("/ask")
def ask():
    global chat_history
    data = request.get_json(silent=True) or {}
    text = str(data.get("text","")).strip()
    if not text:
        return jsonify(success=False, reply="No command received.", action="none", r=0,g=0,b=0,mode="solid",speed_ms=120,brightness=255,screen="none", sound="none"),400
    
    # Inject Context
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    context_text = f"[System Context: Current Time is {now}] " + text

    # Build messages
    messages = [{"role": "system", "content": SYSTEM}]
    messages.extend(chat_history)
    messages.append({"role": "user", "content": context_text})
    
    ai = ask_ollama(messages)
    
    if not ai:
        return jsonify(success=False,reply="AI core connection error.",action="none",r=0,g=0,b=0,mode="solid",speed_ms=120,brightness=255,screen="face_sad", sound="error"),500

    tool = ai.get("tool", "none")
    if tool and tool != "none":
        tool_args = ai.get("tool_args", "")
        print(f"MIO running tool: {tool} ({tool_args})")
        tool_result = run_tool(tool, tool_args)
        
        messages.append({"role": "assistant", "content": json.dumps(ai)})
        messages.append({"role": "user", "content": f"TOOL RESULT: {tool_result}\n\nNow provide your final JSON answer."})
        
        ai_final = ask_ollama(messages)
        if ai_final:
            ai = ai_final

    # Append to memory (keep last 4 user-assistant pairs)
    chat_history.append({"role": "user", "content": text})
    chat_history.append({"role": "assistant", "content": json.dumps(ai)})
    if len(chat_history) > 8:
        chat_history = chat_history[-8:]

    action = ai.get("action","none")
    if action not in ("none","set_light"): action="none"
    mode = ai.get("mode","solid")
    if mode not in ("solid","blink","breathe","off"): mode="solid"
    screen = ai.get("screen","none")
    if screen not in ("none","dog_running","clear", "face_idle", "face_happy", "face_sad", "face_angry", "face_surprised", "face_suspicious", "face_sleepy"): screen="none"
    sound = ai.get("sound", "none")
    if sound not in ("none", "beep", "success", "error", "alarm"): sound="none"
    
    
    # Generate TTS Audio
    reply_text = str(ai.get("reply","Done."))
    audio_url = ""
    try:
        tts = gTTS(text=reply_text, lang='en', slow=False)
        filename = f"tts_{uuid.uuid4().hex[:8]}.mp3"
        filepath = os.path.join('static', filename)
        tts.save(filepath)
        audio_url = request.host_url + f"static/{filename}"
    except Exception as e:
        print("TTS Error:", e)

    out = {
        "success": True,
        "reply": reply_text,
        "action": action,
        "r": clamp(ai.get("r",0),0,255,0),
        "g": clamp(ai.get("g",0),0,255,0),
        "b": clamp(ai.get("b",0),0,255,0),
        "mode": mode,
        "speed_ms": clamp(ai.get("speed_ms",120),30,2000,120),
        "brightness": clamp(ai.get("brightness",255),0,255,255),
        "screen": screen,
        "sound": sound,
        "audio_url": audio_url
    }
    print("MIO:", text, "=>", out)
    return jsonify(out)

@app.route("/trigger", methods=["GET", "POST"])
def trigger():
    # Send a free push notification via ntfy.sh
    # The user can subscribe to this topic on their phone/browser
    topic = "mio_brain_alerts_8899"
    try:
        res = requests.post(f"https://ntfy.sh/{topic}",
            data="Alert! The GPIO pins were connected on MIO.".encode(encoding='utf-8'),
            headers={
                "Title": "MIO Hardware Alert",
                "Priority": "urgent",
                "Tags": "warning,robot",
                "Authorization": "Bearer tk_8k3fybdtv0i9dvkpml102h3ovmwx2"
            })
        print(f"ntfy.sh response: {res.status_code} - {res.text}")
        
        if res.status_code == 200:
            return jsonify({"success": True, "message": "Notification sent!"})
        else:
            return jsonify({"success": False, "error": f"ntfy error {res.status_code}: {res.text}"})
    except Exception as e:
        print("Error sending notification:", e)
        return jsonify({"success": False, "error": str(e)})

# Render expects the application object to be named 'app'
if __name__ == "__main__":
    print("MIO CLOUD BRAIN ONLINE")
    print(f"URL: {OLLAMA_URL} | MODEL: {MODEL}")
    app.run(host="0.0.0.0", port=5000, debug=False)
