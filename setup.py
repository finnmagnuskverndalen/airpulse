#!/usr/bin/env python3
import os, json, sys, subprocess, time
from urllib.request import urlopen, Request
from urllib.error import URLError

RED='\033[91m'; GREEN='\033[92m'; YELLOW='\033[93m'
BLUE='\033[94m'; CYAN='\033[96m'; BOLD='\033[1m'; RESET='\033[0m'

def p(color, msg): print(f"{color}{msg}{RESET}")
def ok(msg): p(GREEN, f"  ✓ {msg}")
def warn(msg): p(YELLOW, f"  ⚠ {msg}")
def err(msg): p(RED, f"  ✗ {msg}")
def info(msg): p(CYAN, f"  → {msg}")
def header(msg): print(f"\n{BOLD}{BLUE}{'─'*50}\n  {msg}\n{'─'*50}{RESET}")

def ask(prompt, default=None):
    suffix = f" [{default}]" if default else ""
    val = input(f"{CYAN}  {prompt}{suffix}: {RESET}").strip()
    return val if val else default

def api_call(key, model, prompt="say hello in 5 words"):
    req = Request(
        "https://openrouter.ai/api/v1/chat/completions",
        data=json.dumps({
            "model": model,
            "max_tokens": 30,
            "messages": [{"role": "user", "content": prompt}]
        }).encode(),
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
            "HTTP-Referer": "https://github.com/finnmagnuskverndalen/airpulse"
        }
    )
    try:
        with urlopen(req, timeout=15) as r:
            return json.loads(r.read())
    except Exception as e:
        return {"error": {"message": str(e)}}

def fetch_models(key):
    req = Request(
        "https://openrouter.ai/api/v1/models",
        headers={"Authorization": f"Bearer {key}"}
    )
    try:
        with urlopen(req, timeout=10) as r:
            return json.loads(r.read())["data"]
    except:
        return []

def get_ip():
    try:
        result = subprocess.run(
            ["ip", "route", "get", "1"],
            capture_output=True, text=True
        )
        for part in result.stdout.split():
            if part.count('.') == 3 and part != '1':
                return part
    except:
        pass
    return "192.168.1.1"

def load_env(path):
    env = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if '=' in line and not line.startswith('#'):
                k, v = line.split('=', 1)
                env[k.strip()] = v.strip()
    return env

def save_env(path, env):
    lines = []
    for k, v in env.items():
        lines.append(f"{k}={v}\n")
    with open(path, 'w') as f:
        f.writelines(lines)

# ── main ──────────────────────────────────────────────────────

print(f"""
{BOLD}{CYAN}
  ◉ airpulse setup
  real-time wifi packet intelligence
{RESET}""")

script_dir = os.path.dirname(os.path.abspath(__file__))
env_path = os.path.join(script_dir, ".env")
env = load_env(env_path)

# ── step 1: openrouter api key ────────────────────────────────
header("1 / 5  OpenRouter API key")

current_key = env.get("OPENROUTER_API_KEY", "")
if current_key and current_key != "your-key-here":
    ok(f"existing key found: {current_key[:8]}...")
    change = ask("change it? (y/N)", "n").lower()
    if change != 'y':
        api_key = current_key
    else:
        api_key = ask("paste your OpenRouter API key")
else:
    info("get a free key at https://openrouter.ai/keys")
    api_key = ask("paste your OpenRouter API key")

env["OPENROUTER_API_KEY"] = api_key

# ── step 2: test key + fetch models ───────────────────────────
header("2 / 5  Testing API key & fetching models")

info("fetching available models...")
models = fetch_models(api_key)

if not models:
    err("could not fetch models — check your API key and internet connection")
    sys.exit(1)

ok(f"found {len(models)} models")

# separate free and paid
free_models = [m for m in models if ":free" in m["id"]]
paid_models = [m for m in models if ":free" not in m["id"] and m.get("pricing", {}).get("completion")]

# sort paid by cost
def cost(m):
    try: return float(m.get("pricing", {}).get("completion", 9999))
    except: return 9999

paid_models.sort(key=cost)

top_free = free_models[:8]
top_paid = paid_models[:8]

# ── step 3: pick model ────────────────────────────────────────
header("3 / 5  Choose LLM model")

print(f"\n  {BOLD}Free models:{RESET}")
for i, m in enumerate(top_free[:5], 1):
    print(f"  {CYAN}{i}{RESET}  {m['id']}")

print(f"\n  {BOLD}Cheap paid models:{RESET}")
for i, m in enumerate(top_paid[:5], 1):
    c = cost(m)
    cost_str = f"${c*1e6:.2f}/1M tokens" if c < 1 else f"${c:.4f}/token"
    print(f"  {CYAN}{i+5}{RESET}  {m['id']:55s} {YELLOW}{cost_str}{RESET}")

print(f"\n  {CYAN}0{RESET}  enter model ID manually")

current_model = env.get("LLM_MODEL", "openai/gpt-4o-mini")
choice = ask(f"pick a number (current: {current_model})", "0")

try:
    idx = int(choice)
    if idx == 0:
        chosen_model = ask("model ID")
    elif 1 <= idx <= 5:
        chosen_model = top_free[idx-1]["id"]
    elif 6 <= idx <= 10:
        chosen_model = top_paid[idx-6]["id"]
    else:
        chosen_model = current_model
except:
    chosen_model = current_model

info(f"testing {chosen_model}...")
result = api_call(api_key, chosen_model)

if "choices" in result:
    reply = result["choices"][0]["message"]["content"]
    ok(f"model works: \"{reply.strip()[:60]}\"")
    env["LLM_MODEL"] = chosen_model
elif "error" in result:
    warn(f"model error: {result['error']['message'][:80]}")
    keep = ask("keep it anyway? (y/N)", "n").lower()
    if keep == 'y':
        env["LLM_MODEL"] = chosen_model
    else:
        env["LLM_MODEL"] = current_model
        warn(f"keeping current model: {current_model}")

# ── step 4: network config ────────────────────────────────────
header("4 / 5  Network configuration")

detected_ip = get_ip()
ok(f"detected laptop IP: {detected_ip}")

server_port = env.get("SERVER_PORT", "8765")
esp_port = env.get("ESP_WS_PORT", "8766")

server_port = ask("browser dashboard port", server_port)
esp_port = ask("M5StickC WebSocket port", esp_port)

env["SERVER_HOST"] = "0.0.0.0"
env["SERVER_PORT"] = server_port
env["ESP_WS_PORT"] = esp_port
env["OUI_CSV_PATH"] = env.get("OUI_CSV_PATH", "data/oui.csv")
env["LLM_INTERVAL_SECS"] = env.get("LLM_INTERVAL_SECS", "30")

info(f"firmware should use SERVER_IP = {detected_ip}")
info(f"firmware should use SERVER_PORT = {esp_port}")

# ── step 5: oui database ──────────────────────────────────────
header("5 / 5  OUI database")

oui_path = os.path.join(script_dir, "data", "oui.csv")
os.makedirs(os.path.join(script_dir, "data"), exist_ok=True)

if os.path.exists(oui_path):
    lines = sum(1 for _ in open(oui_path))
    ok(f"OUI database exists: {lines-1} entries")
    refresh = ask("re-download? (y/N)", "n").lower()
else:
    refresh = 'y'

if refresh == 'y':
    info("downloading OUI database from IEEE...")
    try:
        with urlopen("https://standards-oui.ieee.org/oui/oui.txt", timeout=30) as r:
            raw = r.read().decode('utf-8', errors='ignore')
        out = ["mac_prefix,vendor_name\n"]
        for line in raw.splitlines():
            if "(hex)" in line:
                parts = line.split("(hex)")
                if len(parts) == 2:
                    mac = parts[0].strip().replace("-","").lower()
                    vendor = parts[1].strip().strip('"')
                    out.append(f"{mac},{vendor}\n")
        with open(oui_path, 'w') as f:
            f.writelines(out)
        ok(f"downloaded {len(out)-1} OUI entries")
    except Exception as e:
        err(f"download failed: {e}")
        warn("run manually: curl -L -o /tmp/oui.txt https://standards-oui.ieee.org/oui/oui.txt")

# ── save and done ─────────────────────────────────────────────
save_env(env_path, env)

print(f"""
{BOLD}{GREEN}{'─'*50}
  setup complete
{'─'*50}{RESET}

  {CYAN}run airpulse:{RESET}
    cargo run

  {CYAN}open dashboard:{RESET}
    http://localhost:{server_port}

  {CYAN}firmware config:{RESET}
    SERVER_IP   = {detected_ip}
    SERVER_PORT = {esp_port}

  {CYAN}model:{RESET} {env.get('LLM_MODEL')}
  {CYAN}llm interval:{RESET} every {env.get('LLM_INTERVAL_SECS')}s
""")
