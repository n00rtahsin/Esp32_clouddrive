/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║        ESP32 UNIFIED CLOUD DRIVE — PROFESSIONAL         ║
 * ║     Local Web Server + Remote Telegram Bot Access       ║
 * ╠══════════════════════════════════════════════════════════╣
 * ║  Author  : Md Nazmun Nur                                ║
 * ║  Version : 2.0 Pro                                      ║
 * ║  Features:                                              ║
 * ║   • Password-Protected Web UI (Login Page + Session)    ║
 * ║   • Stunning Dark Glassmorphism Web Interface           ║
 * ║   • File Upload via Web Browser                         ║
 * ║   • File Delete via Web Browser                         ║
 * ║   • SD Card Stats (Used / Free Space)                   ║
 * ║   • Telegram: /list, /send, /delete, /stats, /reboot    ║
 * ║   • OLED Status Display with Boot Animation             ║
 * ║   • Unauthorized Access Logging & Alerts                ║
 * ╚══════════════════════════════════════════════════════════╝
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h> // For SHA-256 hashing

// ═══════════════════════════════════════════════════════════
//  SECTION 1 ── CONFIGURATION  (Edit these values)
// ═══════════════════════════════════════════════════════════

// Network
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

// Telegram
const char* BOT_TOKEN = "";
const char* CHAT_ID   = "";

// Web Dashboard Credentials
const char* WEB_USERNAME = "admin";
const char* WEB_PASSWORD = "esp32drive";  // ← Change this!

// Session token (randomly generated at boot — no plain-text cookies)
String SESSION_TOKEN = "";

// ═══════════════════════════════════════════════════════════
//  SECTION 2 ── HARDWARE PINS & OBJECTS
// ═══════════════════════════════════════════════════════════

#define SD_CS_PIN     5
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer        server(80);
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

unsigned long lastBotCheck   = 0;
unsigned long botCheckDelay  = 1500;
unsigned long sessionExpiry  = 0;
const unsigned long SESSION_DURATION = 1800000UL; // 30 minutes

// ═══════════════════════════════════════════════════════════
//  SECTION 3 ── UTILITY FUNCTIONS
// ═══════════════════════════════════════════════════════════

// Generate a pseudo-random session token from ESP chip ID + millis
String generateSessionToken() {
  uint64_t chipId = ESP.getEfuseMac();
  unsigned long t = millis();
  String raw = String((uint32_t)chipId, HEX) + String(t);
  // Simple hash using XOR folding
  uint32_t hash = 0x811C9DC5;
  for (char c : raw) {
    hash ^= (uint8_t)c;
    hash *= 0x01000193;
  }
  return String(hash, HEX) + String((uint32_t)(chipId >> 32), HEX);
}

// Get SD card total/used space in MB
String getSDStats() {
  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes  = SD.usedBytes();
  float totalMB = totalBytes / (1024.0 * 1024.0);
  float usedMB  = usedBytes  / (1024.0 * 1024.0);
  float freeMB  = totalMB - usedMB;
  char buf[80];
  snprintf(buf, sizeof(buf),
    "Total: %.1f MB | Used: %.1f MB | Free: %.1f MB",
    totalMB, usedMB, freeMB);
  return String(buf);
}

// Count files on SD root
int countFiles() {
  int count = 0;
  File root = SD.open("/");
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) count++;
    f = root.openNextFile();
  }
  return count;
}

// OLED display helper — 3 lines
void updateDisplay(String l1, String l2 = "", String l3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Top bar
  display.fillRect(0, 0, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 1);
  display.print("ESP32 CLOUD DRIVE");
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 14); display.println(l1);
  if (l2 != "") { display.setCursor(0, 26); display.println(l2); }
  if (l3 != "") { display.setCursor(0, 38); display.println(l3); }
  display.display();
}

// OLED boot animation
void bootAnimation() {
  for (int i = 0; i <= 128; i += 4) {
    display.clearDisplay();
    display.drawRect(0, 52, 128, 10, SSD1306_WHITE);
    display.fillRect(0, 52, i, 10, SSD1306_WHITE);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 20);
    display.print("CLOUD DRIVE v2.0");
    display.setCursor(40, 36);
    display.print("Booting...");
    display.display();
    delay(20);
  }
  delay(500);
}

// Check if this request has a valid session cookie
bool isAuthenticated() {
  if (SESSION_TOKEN == "") return false;
  if (millis() > sessionExpiry)  return false; // Session expired
  String cookie = server.header("Cookie");
  return cookie.indexOf("ESPSESSION=" + SESSION_TOKEN) >= 0;
}

// ═══════════════════════════════════════════════════════════
//  SECTION 4 ── WEB SERVER HTML PAGES
// ═══════════════════════════════════════════════════════════

// ── Login Page ──────────────────────────────────────────────
void serveLoginPage(bool badCreds = false) {
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32 Drive — Login</title>
<link href="https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Rajdhani:wght@400;600;700&display=swap" rel="stylesheet">
<style>
  :root {
    --bg: #050a0e;
    --panel: rgba(0,255,170,0.05);
    --border: rgba(0,255,170,0.2);
    --accent: #00ffaa;
    --accent2: #00c8ff;
    --danger: #ff4566;
    --text: #c8fff0;
    --muted: #5a8a78;
  }
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    min-height: 100vh;
    display: flex;
    align-items: center;
    justify-content: center;
    font-family: 'Rajdhani', sans-serif;
    color: var(--text);
    overflow: hidden;
  }
  /* Animated grid background */
  body::before {
    content: '';
    position: fixed;
    inset: 0;
    background-image:
      linear-gradient(rgba(0,255,170,0.04) 1px, transparent 1px),
      linear-gradient(90deg, rgba(0,255,170,0.04) 1px, transparent 1px);
    background-size: 40px 40px;
    animation: gridMove 20s linear infinite;
  }
  @keyframes gridMove { from { background-position: 0 0; } to { background-position: 40px 40px; } }

  /* Glowing orbs */
  .orb { position: fixed; border-radius: 50%; filter: blur(80px); opacity: 0.15; pointer-events: none; }
  .orb1 { width: 400px; height: 400px; background: var(--accent); top: -100px; left: -100px; animation: float1 8s ease-in-out infinite; }
  .orb2 { width: 300px; height: 300px; background: var(--accent2); bottom: -80px; right: -80px; animation: float2 10s ease-in-out infinite; }
  @keyframes float1 { 0%,100%{transform:translate(0,0)} 50%{transform:translate(30px,20px)} }
  @keyframes float2 { 0%,100%{transform:translate(0,0)} 50%{transform:translate(-20px,30px)} }

  .card {
    position: relative;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 48px 40px;
    width: 380px;
    backdrop-filter: blur(20px);
    box-shadow: 0 0 60px rgba(0,255,170,0.08), inset 0 1px 0 rgba(255,255,255,0.05);
    animation: fadeIn 0.6s ease;
  }
  @keyframes fadeIn { from { opacity:0; transform:translateY(20px); } to { opacity:1; transform:translateY(0); } }

  .logo {
    text-align: center;
    margin-bottom: 32px;
  }
  .logo-icon {
    font-size: 48px;
    margin-bottom: 8px;
    filter: drop-shadow(0 0 12px var(--accent));
    animation: pulse 2s ease-in-out infinite;
  }
  @keyframes pulse { 0%,100%{filter:drop-shadow(0 0 8px var(--accent))} 50%{filter:drop-shadow(0 0 20px var(--accent))} }
  .logo h1 {
    font-family: 'Share Tech Mono', monospace;
    font-size: 20px;
    color: var(--accent);
    letter-spacing: 3px;
    text-transform: uppercase;
  }
  .logo p {
    font-size: 13px;
    color: var(--muted);
    margin-top: 4px;
    letter-spacing: 1px;
  }
  .field { margin-bottom: 18px; }
  .field label {
    display: block;
    font-size: 11px;
    letter-spacing: 2px;
    text-transform: uppercase;
    color: var(--muted);
    margin-bottom: 6px;
    font-family: 'Share Tech Mono', monospace;
  }
  .field input {
    width: 100%;
    background: rgba(0,0,0,0.4);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 16px;
    color: var(--text);
    font-family: 'Share Tech Mono', monospace;
    font-size: 14px;
    transition: border-color 0.2s, box-shadow 0.2s;
    outline: none;
  }
  .field input:focus {
    border-color: var(--accent);
    box-shadow: 0 0 0 3px rgba(0,255,170,0.12);
  }
  .btn {
    width: 100%;
    padding: 14px;
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    border: none;
    border-radius: 8px;
    color: #050a0e;
    font-family: 'Share Tech Mono', monospace;
    font-size: 14px;
    font-weight: bold;
    letter-spacing: 2px;
    text-transform: uppercase;
    cursor: pointer;
    margin-top: 8px;
    transition: opacity 0.2s, transform 0.1s;
  }
  .btn:hover  { opacity: 0.9; }
  .btn:active { transform: scale(0.98); }
  .error {
    background: rgba(255,69,102,0.12);
    border: 1px solid rgba(255,69,102,0.4);
    border-radius: 8px;
    padding: 10px 14px;
    font-size: 13px;
    color: var(--danger);
    margin-bottom: 18px;
    font-family: 'Share Tech Mono', monospace;
    letter-spacing: 1px;
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .corner { position: absolute; width: 12px; height: 12px; }
  .corner.tl { top:12px; left:12px; border-top:2px solid var(--accent); border-left:2px solid var(--accent); }
  .corner.tr { top:12px; right:12px; border-top:2px solid var(--accent); border-right:2px solid var(--accent); }
  .corner.bl { bottom:12px; left:12px; border-bottom:2px solid var(--accent); border-left:2px solid var(--accent); }
  .corner.br { bottom:12px; right:12px; border-bottom:2px solid var(--accent); border-right:2px solid var(--accent); }
</style>
</head>
<body>
<div class="orb orb1"></div>
<div class="orb orb2"></div>
<div class="card">
  <div class="corner tl"></div><div class="corner tr"></div>
  <div class="corner bl"></div><div class="corner br"></div>
  <div class="logo">
    <div class="logo-icon">🗄️</div>
    <h1>ESP32 Drive</h1>
    <p>SECURE ACCESS PORTAL</p>
  </div>
)rawhtml";

  if (badCreds) {
    html += R"(<div class="error">⚠ Invalid credentials. Access denied.</div>)";
  }

  html += R"rawhtml(
  <form method="POST" action="/login">
    <div class="field">
      <label>Username</label>
      <input type="text" name="username" placeholder="admin" autocomplete="off" required>
    </div>
    <div class="field">
      <label>Password</label>
      <input type="password" name="password" placeholder="••••••••" required>
    </div>
    <button class="btn" type="submit">[ AUTHENTICATE ]</button>
  </form>
</div>
</body>
</html>
)rawhtml";

  server.send(200, "text/html", html);
}

// ── Main Dashboard Page ─────────────────────────────────────
void serveDashboard() {
  File root = SD.open("/");
  String sdStats = getSDStats();

  // Build file rows
  String fileRows = "";
  int fileCount = 0;
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String name = String(file.name());
      float sizeKB = file.size() / 1024.0;
      String sizeStr;
      if (sizeKB < 1024.0) {
        char buf[16]; dtostrf(sizeKB, 4, 1, buf);
        sizeStr = String(buf) + " KB";
      } else {
        char buf[16]; dtostrf(sizeKB / 1024.0, 4, 2, buf);
        sizeStr = String(buf) + " MB";
      }
      // Detect icon by extension
      String ext = name.substring(name.lastIndexOf('.') + 1);
      ext.toLowerCase();
      String icon = "📄";
      if      (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp") icon = "🖼️";
      else if (ext == "mp3" || ext == "wav")  icon = "🎵";
      else if (ext == "mp4" || ext == "avi")  icon = "🎬";
      else if (ext == "pdf")                  icon = "📕";
      else if (ext == "zip" || ext == "gz")   icon = "📦";
      else if (ext == "txt" || ext == "log")  icon = "📝";
      else if (ext == "csv" || ext == "json") icon = "📊";

      fileRows += "<tr>";
      fileRows += "<td><span class='icon'>" + icon + "</span>" + name + "</td>";
      fileRows += "<td>" + sizeStr + "</td>";
      fileRows += "<td><a class='btn-dl' href='/download?f=" + name + "'>⬇ Download</a>"
                  "<a class='btn-del' href='/delete?f=" + name + "' onclick=\"return confirm('Delete " + name + "?')\">🗑 Delete</a></td>";
      fileRows += "</tr>";
      fileCount++;
    }
    file = root.openNextFile();
  }

  if (fileCount == 0) {
    fileRows = "<tr><td colspan='3' class='empty'>No files found on SD card.</td></tr>";
  }

  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32 Drive Dashboard</title>
<link href="https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Rajdhani:wght@400;500;600;700&display=swap" rel="stylesheet">
<style>
  :root {
    --bg: #050a0e;
    --surface: rgba(0,255,170,0.04);
    --surface2: rgba(0,255,170,0.08);
    --border: rgba(0,255,170,0.18);
    --border2: rgba(0,200,255,0.18);
    --accent: #00ffaa;
    --accent2: #00c8ff;
    --danger: #ff4566;
    --warn: #ffa040;
    --text: #c8fff0;
    --muted: #4a7a68;
    --mono: 'Share Tech Mono', monospace;
    --sans: 'Rajdhani', sans-serif;
  }
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  html, body {
    background: var(--bg);
    color: var(--text);
    font-family: var(--sans);
    min-height: 100vh;
  }
  /* Background grid */
  body::before {
    content: '';
    position: fixed;
    inset: 0;
    background-image:
      linear-gradient(rgba(0,255,170,0.03) 1px, transparent 1px),
      linear-gradient(90deg, rgba(0,255,170,0.03) 1px, transparent 1px);
    background-size: 40px 40px;
    pointer-events: none;
    z-index: 0;
  }

  /* ── Header ── */
  header {
    position: sticky; top: 0; z-index: 100;
    background: rgba(5,10,14,0.85);
    backdrop-filter: blur(20px);
    border-bottom: 1px solid var(--border);
    padding: 0 32px;
    height: 64px;
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .logo { display: flex; align-items: center; gap: 12px; }
  .logo-icon { font-size: 28px; filter: drop-shadow(0 0 8px var(--accent)); }
  .logo-text { font-family: var(--mono); font-size: 16px; color: var(--accent); letter-spacing: 3px; }
  .logo-sub   { font-size: 11px; color: var(--muted); letter-spacing: 1px; }
  .header-actions { display: flex; gap: 12px; align-items: center; }
  .chip {
    font-family: var(--mono);
    font-size: 11px;
    background: var(--surface2);
    border: 1px solid var(--border);
    border-radius: 20px;
    padding: 4px 12px;
    color: var(--accent);
    letter-spacing: 1px;
  }
  .btn-logout {
    font-family: var(--mono);
    font-size: 11px;
    background: transparent;
    border: 1px solid var(--danger);
    border-radius: 6px;
    padding: 6px 14px;
    color: var(--danger);
    cursor: pointer;
    letter-spacing: 1px;
    text-decoration: none;
    transition: all 0.2s;
  }
  .btn-logout:hover { background: rgba(255,69,102,0.1); }

  /* ── Layout ── */
  main { position: relative; z-index: 1; max-width: 1100px; margin: 0 auto; padding: 32px 24px; }

  /* ── Stats Cards ── */
  .stats-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 16px; margin-bottom: 28px; }
  .stat-card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 20px 24px;
    position: relative;
    overflow: hidden;
    transition: border-color 0.2s, box-shadow 0.2s;
  }
  .stat-card:hover { border-color: var(--accent); box-shadow: 0 0 20px rgba(0,255,170,0.08); }
  .stat-card::before {
    content: '';
    position: absolute;
    top: 0; left: 0; right: 0;
    height: 2px;
    background: linear-gradient(90deg, var(--accent), var(--accent2));
    opacity: 0.6;
  }
  .stat-label { font-family: var(--mono); font-size: 10px; letter-spacing: 2px; color: var(--muted); text-transform: uppercase; margin-bottom: 8px; }
  .stat-value { font-size: 28px; font-weight: 700; color: var(--accent); line-height: 1; }
  .stat-unit  { font-size: 13px; color: var(--muted); margin-top: 4px; }

  /* ── Panel ── */
  .panel {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 14px;
    overflow: hidden;
    margin-bottom: 24px;
  }
  .panel-header {
    background: var(--surface2);
    border-bottom: 1px solid var(--border);
    padding: 14px 24px;
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .panel-title { font-family: var(--mono); font-size: 13px; letter-spacing: 2px; color: var(--accent); text-transform: uppercase; }
  .panel-body  { padding: 24px; }

  /* ── Upload Form ── */
  .upload-zone {
    border: 2px dashed var(--border2);
    border-radius: 10px;
    padding: 28px;
    text-align: center;
    transition: all 0.2s;
    cursor: pointer;
  }
  .upload-zone:hover { border-color: var(--accent2); background: rgba(0,200,255,0.04); }
  .upload-zone p { font-size: 15px; color: var(--muted); margin-bottom: 14px; }
  .upload-row { display: flex; gap: 10px; justify-content: center; align-items: center; flex-wrap: wrap; }
  input[type=file] { color: var(--text); font-family: var(--mono); font-size: 12px; }
  .btn-upload {
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    border: none;
    border-radius: 8px;
    padding: 10px 24px;
    font-family: var(--mono);
    font-size: 12px;
    letter-spacing: 2px;
    color: #050a0e;
    font-weight: bold;
    cursor: pointer;
    transition: opacity 0.2s;
  }
  .btn-upload:hover { opacity: 0.85; }

  /* ── File Table ── */
  table { width: 100%; border-collapse: collapse; font-size: 14px; }
  thead tr { background: var(--surface2); }
  thead th {
    font-family: var(--mono);
    font-size: 10px;
    letter-spacing: 2px;
    text-transform: uppercase;
    color: var(--muted);
    padding: 12px 16px;
    text-align: left;
    font-weight: 400;
  }
  tbody tr {
    border-bottom: 1px solid rgba(0,255,170,0.05);
    transition: background 0.15s;
  }
  tbody tr:hover { background: rgba(0,255,170,0.04); }
  tbody tr:last-child { border-bottom: none; }
  td { padding: 13px 16px; vertical-align: middle; }
  td:first-child { font-family: var(--mono); font-size: 13px; display: flex; align-items: center; gap: 8px; }
  .icon { font-size: 16px; }
  td:nth-child(2) { color: var(--muted); font-family: var(--mono); font-size: 12px; }
  td:nth-child(3) { display: flex; gap: 8px; align-items: center; }
  .empty { text-align: center; color: var(--muted); font-family: var(--mono); padding: 32px !important; font-size: 13px; }

  .btn-dl {
    font-family: var(--mono);
    font-size: 11px;
    padding: 6px 12px;
    background: rgba(0,200,255,0.1);
    border: 1px solid var(--border2);
    border-radius: 6px;
    color: var(--accent2);
    text-decoration: none;
    transition: all 0.2s;
    white-space: nowrap;
  }
  .btn-dl:hover { background: rgba(0,200,255,0.2); border-color: var(--accent2); }
  .btn-del {
    font-family: var(--mono);
    font-size: 11px;
    padding: 6px 12px;
    background: rgba(255,69,102,0.08);
    border: 1px solid rgba(255,69,102,0.25);
    border-radius: 6px;
    color: var(--danger);
    text-decoration: none;
    transition: all 0.2s;
    white-space: nowrap;
  }
  .btn-del:hover { background: rgba(255,69,102,0.18); border-color: var(--danger); }

  /* ── Alert Banner ── */
  .alert {
    background: rgba(0,255,170,0.06);
    border: 1px solid rgba(0,255,170,0.2);
    border-left: 4px solid var(--accent);
    border-radius: 8px;
    padding: 12px 16px;
    font-family: var(--mono);
    font-size: 12px;
    color: var(--accent);
    margin-bottom: 20px;
    letter-spacing: 1px;
  }
  .alert.error {
    background: rgba(255,69,102,0.06);
    border-color: rgba(255,69,102,0.3);
    border-left-color: var(--danger);
    color: var(--danger);
  }

  /* ── Footer ── */
  footer {
    text-align: center;
    padding: 24px;
    font-family: var(--mono);
    font-size: 11px;
    color: var(--muted);
    letter-spacing: 1px;
    border-top: 1px solid var(--border);
    margin-top: 16px;
  }
  footer span { color: var(--accent); }

  /* ── Progress Bar ── */
  .progress-wrap { background: rgba(0,0,0,0.4); border-radius: 4px; height: 6px; margin-top: 8px; overflow: hidden; }
  .progress-bar  { height: 100%; border-radius: 4px; background: linear-gradient(90deg, var(--accent), var(--accent2)); transition: width 0.5s; }

  @media (max-width: 600px) {
    header { padding: 0 16px; }
    main   { padding: 20px 12px; }
    td:nth-child(3) { flex-direction: column; gap: 4px; }
  }
</style>
</head>
<body>
<header>
  <div class="logo">
    <span class="logo-icon">🗄️</span>
    <div>
      <div class="logo-text">ESP32 DRIVE</div>
      <div class="logo-sub">CLOUD STORAGE SYSTEM</div>
    </div>
  </div>
  <div class="header-actions">
    <span class="chip">● ONLINE</span>
    <span class="chip" id="clock" style="color:var(--accent2)">--:--:--</span>
    <a href="/logout" class="btn-logout">⏻ LOGOUT</a>
  </div>
</header>

<main>
)rawhtml";

  // Flash message support (via query param)
  String msg = server.hasArg("msg") ? server.arg("msg") : "";
  if (msg == "uploaded")  html += "<div class='alert'>✔ File uploaded successfully.</div>";
  if (msg == "deleted")   html += "<div class='alert'>✔ File deleted successfully.</div>";
  if (msg == "err_upload")html += "<div class='alert error'>✘ Upload failed. Check SD card.</div>";
  if (msg == "err_delete")html += "<div class='alert error'>✘ Delete failed. File may not exist.</div>";

  // Stats bar
  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes  = SD.usedBytes();
  int usedPct = (totalBytes > 0) ? (int)((usedBytes * 100ULL) / totalBytes) : 0;
  float totalMB = totalBytes / (1024.0f * 1024.0f);
  float usedMB  = usedBytes  / (1024.0f * 1024.0f);
  float freeMB  = totalMB - usedMB;

  char totalBuf[10], usedBuf[10], freeBuf[10];
  dtostrf(totalMB, 5, 1, totalBuf);
  dtostrf(usedMB,  5, 1, usedBuf);
  dtostrf(freeMB,  5, 1, freeBuf);

  html += "<div class='stats-grid'>";
  html += "<div class='stat-card'><div class='stat-label'>Total Files</div><div class='stat-value'>" + String(fileCount) + "</div><div class='stat-unit'>on SD card</div></div>";
  html += "<div class='stat-card'><div class='stat-label'>Used Space</div><div class='stat-value'>" + String(usedBuf) + "</div><div class='stat-unit'>MB of " + String(totalBuf) + " MB<div class='progress-wrap'><div class='progress-bar' style='width:" + String(usedPct) + "%'></div></div></div></div>";
  html += "<div class='stat-card'><div class='stat-label'>Free Space</div><div class='stat-value'>" + String(freeBuf) + "</div><div class='stat-unit'>MB available</div></div>";
  html += "<div class='stat-card'><div class='stat-label'>Local IP</div><div class='stat-value' style='font-size:18px;font-family:var(--mono)'>" + WiFi.localIP().toString() + "</div><div class='stat-unit'>http access</div></div>";
  html += "</div>";

  // Upload panel
  html += R"rawhtml(
  <div class="panel">
    <div class="panel-header"><span class="panel-title">⬆ Upload File</span></div>
    <div class="panel-body">
      <div class="upload-zone">
        <p>Select a file from your device to upload to SD card</p>
        <form method="POST" action="/upload" enctype="multipart/form-data">
          <div class="upload-row">
            <input type="file" name="file" required>
            <button class="btn-upload" type="submit">[ UPLOAD ]</button>
          </div>
        </form>
      </div>
    </div>
  </div>
)rawhtml";

  // File list panel
  html += "<div class='panel'>";
  html += "<div class='panel-header'><span class='panel-title'>📂 SD Card Files</span><span class='chip'>" + String(fileCount) + " FILES</span></div>";
  html += "<table><thead><tr><th>Filename</th><th>Size</th><th>Actions</th></tr></thead><tbody>";
  html += fileRows;
  html += "</tbody></table></div>";

  html += R"rawhtml(
</main>
<footer>
  ESP32 CLOUD DRIVE v2.0 &nbsp;|&nbsp; Designed by <span>Md Nazmun Nur</span> &nbsp;|&nbsp; Session secured
</footer>
<script>
  function updateClock() {
    const d = new Date();
    document.getElementById('clock').textContent =
      d.getHours().toString().padStart(2,'0') + ':' +
      d.getMinutes().toString().padStart(2,'0') + ':' +
      d.getSeconds().toString().padStart(2,'0');
  }
  updateClock();
  setInterval(updateClock, 1000);
</script>
</body>
</html>
)rawhtml";

  server.send(200, "text/html", html);
}

// ═══════════════════════════════════════════════════════════
//  SECTION 5 ── WEB SERVER ROUTE HANDLERS
// ═══════════════════════════════════════════════════════════

// GET /  →  redirect or show dashboard
void handleRoot() {
  if (!isAuthenticated()) {
    server.sendHeader("Location", "/login");
    server.send(302);
    return;
  }
  serveDashboard();
}

// GET /login  →  show login form
void handleLoginGet() {
  if (isAuthenticated()) {
    server.sendHeader("Location", "/");
    server.send(302);
    return;
  }
  serveLoginPage(false);
}

// POST /login  →  verify credentials
void handleLoginPost() {
  String user = server.arg("username");
  String pass = server.arg("password");

  if (user == WEB_USERNAME && pass == WEB_PASSWORD) {
    SESSION_TOKEN = generateSessionToken();
    sessionExpiry = millis() + SESSION_DURATION;
    server.sendHeader("Set-Cookie", "ESPSESSION=" + SESSION_TOKEN + "; Path=/; HttpOnly");
    server.sendHeader("Location", "/");
    server.send(302);
    updateDisplay("Web Login OK", WiFi.localIP().toString(), "Session started");
    Serial.println("[AUTH] Login successful.");
  } else {
    Serial.println("[AUTH] Failed login attempt.");
    updateDisplay("Login FAILED", "Bad credentials!", "");
    serveLoginPage(true);
  }
}

// GET /logout
void handleLogout() {
  SESSION_TOKEN = "";
  server.sendHeader("Set-Cookie", "ESPSESSION=deleted; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
  server.sendHeader("Location", "/login");
  server.send(302);
}

// GET /download?f=filename
void handleDownload() {
  if (!isAuthenticated()) { server.sendHeader("Location", "/login"); server.send(302); return; }
  String filename = "/" + server.arg("f");
  if (SD.exists(filename)) {
    File file = SD.open(filename, FILE_READ);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + server.arg("f") + "\"");
    server.streamFile(file, "application/octet-stream");
    file.close();
    updateDisplay("Download sent", server.arg("f"), "");
  } else {
    server.send(404, "text/plain", "404 — File not found.");
  }
}

// GET /delete?f=filename
void handleDelete() {
  if (!isAuthenticated()) { server.sendHeader("Location", "/login"); server.send(302); return; }
  String filename = "/" + server.arg("f");
  if (SD.remove(filename)) {
    server.sendHeader("Location", "/?msg=deleted");
    server.send(302);
    updateDisplay("File Deleted", server.arg("f"), "");
  } else {
    server.sendHeader("Location", "/?msg=err_delete");
    server.send(302);
  }
}

// POST /upload  →  multipart file upload to SD
File uploadFile;
void handleUploadPage() {
  if (!isAuthenticated()) { server.sendHeader("Location", "/login"); server.send(302); return; }
  server.sendHeader("Location", "/?msg=uploaded");
  server.send(302);
}

void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = "/" + upload.filename;
    if (SD.exists(filename)) SD.remove(filename);
    uploadFile = SD.open(filename, FILE_WRITE);
    updateDisplay("Uploading...", upload.filename, "");
    Serial.println("[UPLOAD] Start: " + filename);
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      updateDisplay("Upload Done!", upload.filename, String(upload.totalSize / 1024) + " KB");
      Serial.println("[UPLOAD] Done: " + String(upload.totalSize) + " bytes");
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  SECTION 6 ── TELEGRAM BOT HANDLER
// ═══════════════════════════════════════════════════════════

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id   = bot.messages[i].chat_id;
    String text      = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    // Security gate
    if (chat_id != String(CHAT_ID)) {
      bot.sendMessage(chat_id, "🚫 *Unauthorized access denied.*\nThis incident has been logged.", "Markdown");
      Serial.println("[SECURITY] Blocked ID: " + chat_id);
      bot.sendMessage(CHAT_ID, "⚠️ Unauthorized access attempt from ID: " + chat_id, "");
      continue;
    }

    // ── /start ───────────────────────────────────────────────
    if (text == "/start") {
      String msg = "🗄 *ESP32 Cloud Drive v2.0*\n";
      msg += "━━━━━━━━━━━━━━━━━━━━\n";
      msg += "Hello, *" + from_name + "*! Welcome back.\n\n";
      msg += "📋 *Available Commands:*\n";
      msg += "`/list`     — List all SD card files\n";
      msg += "`/stats`    — SD card storage statistics\n";
      msg += "`/send <filename>` — Download a file\n";
      msg += "`/delete <filename>` — Delete a file\n";
      msg += "`/ip`       — Get local web URL\n";
      msg += "`/reboot`   — Restart the ESP32\n";
      msg += "`/help`     — Show this menu\n\n";
      msg += "🌐 Web UI: `http://" + WiFi.localIP().toString() + "`";
      bot.sendMessage(chat_id, msg, "Markdown");
    }

    // ── /help ────────────────────────────────────────────────
    else if (text == "/help") {
      bot.sendMessage(chat_id, "Use /start to see the full command list.", "");
    }

    // ── /ip ─────────────────────────────────────────────────
    else if (text == "/ip") {
      bot.sendMessage(chat_id,
        "🌐 Local Web UI:\n`http://" + WiFi.localIP().toString() + "`\n\nLogin with your configured credentials.",
        "Markdown");
    }

    // ── /stats ───────────────────────────────────────────────
    else if (text == "/stats") {
      float totalMB = SD.totalBytes() / (1024.0f * 1024.0f);
      float usedMB  = SD.usedBytes()  / (1024.0f * 1024.0f);
      float freeMB  = totalMB - usedMB;
      int   pct     = (totalMB > 0) ? (int)((usedMB / totalMB) * 100) : 0;

      String bar = "[";
      int filled = pct / 5;
      for (int b = 0; b < 20; b++) bar += (b < filled) ? "█" : "░";
      bar += "] " + String(pct) + "%";

      char buf[200];
      snprintf(buf, sizeof(buf),
        "📊 *SD Card Statistics*\n━━━━━━━━━━━━━━━━\n"
        "💾 Total : `%.1f MB`\n"
        "📁 Used  : `%.1f MB`\n"
        "✅ Free  : `%.1f MB`\n"
        "📂 Files : `%d`\n\n%s",
        totalMB, usedMB, freeMB, countFiles(), bar.c_str());
      bot.sendMessage(chat_id, String(buf), "Markdown");
    }

    // ── /list ────────────────────────────────────────────────
    else if (text == "/list") {
      File root = SD.open("/");
      String fileList = "📂 *SD Card Contents*\n━━━━━━━━━━━━━━━━\n";
      File f = root.openNextFile();
      bool hasFiles = false;
      while (f) {
        if (!f.isDirectory()) {
          float sz = f.size() / 1024.0f;
          char line[60];
          if (sz < 1024.0f)
            snprintf(line, sizeof(line), "• `%s` (%.1f KB)\n", f.name(), sz);
          else
            snprintf(line, sizeof(line), "• `%s` (%.2f MB)\n", f.name(), sz / 1024.0f);
          fileList += String(line);
          hasFiles = true;
        }
        f = root.openNextFile();
      }
      if (!hasFiles) fileList += "_SD card is empty._";
      bot.sendMessage(chat_id, fileList, "Markdown");
    }

    // ── /send <filename> ─────────────────────────────────────
    // NOTE: UniversalTelegramBot does not support binary file sending.
    // Instead, we reply with the direct download URL for the local web server.
    else if (text.startsWith("/send ")) {
      String fname = text.substring(6);
      fname.trim();
      String fullPath = "/" + fname;
      if (SD.exists(fullPath)) {
        File f = SD.open(fullPath, FILE_READ);
        float sizeKB = f.size() / 1024.0f;
        f.close();
        char sizeBuf[12];
        dtostrf(sizeKB, 4, 1, sizeBuf);
        String dlUrl = "http://" + WiFi.localIP().toString() + "/download?f=" + fname;
        String msg = "📥 *File Ready for Download*\n";
        msg += "━━━━━━━━━━━━━━━━\n";
        msg += "📄 Name : `" + fname + "`\n";
        msg += "💾 Size : `" + String(sizeBuf) + " KB`\n\n";
        msg += "🔗 Download link (on same WiFi):\n";
        msg += "`" + dlUrl + "`\n\n";
        msg += "_Login with your web credentials to download._";
        bot.sendMessage(chat_id, msg, "Markdown");
        updateDisplay("File Link Sent", fname, "via Telegram");
      } else {
        bot.sendMessage(chat_id, "❌ File `" + fname + "` not found on SD card.", "Markdown");
      }
    }

    // ── /delete <filename> ───────────────────────────────────
    else if (text.startsWith("/delete ")) {
      String filename = "/" + text.substring(8);
      filename.trim();
      if (SD.remove(filename)) {
        bot.sendMessage(chat_id, "🗑 File `" + filename + "` deleted successfully.", "Markdown");
        updateDisplay("File Deleted", filename, "via Telegram");
      } else {
        bot.sendMessage(chat_id, "❌ Could not delete `" + filename + "`. File may not exist.", "Markdown");
      }
    }

    // ── /reboot ──────────────────────────────────────────────
    else if (text == "/reboot") {
      bot.sendMessage(chat_id, "🔄 Rebooting ESP32... reconnecting in ~10 seconds.", "");
      delay(500);
      ESP.restart();
    }

    // ── Unknown command ───────────────────────────────────────
    else {
      bot.sendMessage(chat_id, "❓ Unknown command. Send /start for the menu.", "");
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  SECTION 7 ── SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] ESP32 Cloud Drive v2.0");

  // ── 1. OLED ─────────────────────────────────────────────
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("[ERROR] OLED init failed");
    for(;;);
  }
  bootAnimation();
  updateDisplay("Booting v2.0...", "Initializing...", "");

  // ── 2. SD Card ──────────────────────────────────────────
  updateDisplay("Mounting SD...", "", "");
  if (!SD.begin(SD_CS_PIN)) {
    updateDisplay("SD CARD ERROR", "Check wiring!", "System halted.");
    Serial.println("[ERROR] SD Card init failed");
    while (1);
  }
  Serial.println("[OK] SD Card mounted");

  // ── 3. WiFi ─────────────────────────────────────────────
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  updateDisplay("WiFi Connecting", WIFI_SSID, "Please wait...");
  Serial.print("[WIFI] Connecting");

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    updateDisplay("WiFi FAILED", "Check creds!", WIFI_SSID);
    Serial.println("\n[ERROR] WiFi failed");
    while(1);
  }
  Serial.println("\n[OK] WiFi connected: " + WiFi.localIP().toString());

  // ── 4. TLS for Telegram ─────────────────────────────────
  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  // ── 5. Web Server Routes ─────────────────────────────────
  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/login",   HTTP_GET,  handleLoginGet);
  server.on("/login",   HTTP_POST, handleLoginPost);
  server.on("/logout",  HTTP_GET,  handleLogout);
  server.on("/download",HTTP_GET,  handleDownload);
  server.on("/delete",  HTTP_GET,  handleDelete);
  server.on("/upload",  HTTP_POST, handleUploadPage, handleUpload);
  const char* headerKeys[] = { "Cookie" };
  server.collectHeaders(headerKeys, 1);
  server.begin();
  Serial.println("[OK] Web server started on port 80");

  // ── 6. Done ──────────────────────────────────────────────
  String ip = WiFi.localIP().toString();
  updateDisplay("SYSTEM READY", "Web: " + ip, "Bot: Online");

  bot.sendMessage(CHAT_ID,
    "🟢 *ESP32 Cloud Drive v2.0 Online!*\n"
    "━━━━━━━━━━━━━━━━━━━━\n"
    "🌐 Web UI: `http://" + ip + "`\n"
    "🔒 Password protection: ACTIVE\n"
    "📊 " + getSDStats() + "\n"
    "Send /start for command menu.",
    "Markdown");

  Serial.println("[READY] System fully initialized.");
}

// ═══════════════════════════════════════════════════════════
//  SECTION 8 ── LOOP
// ═══════════════════════════════════════════════════════════

void loop() {
  // Handle local web requests
  server.handleClient();

  // Poll Telegram every botCheckDelay ms
  if (millis() - lastBotCheck > botCheckDelay) {
    int newMsgs = bot.getUpdates(bot.last_message_received + 1);
    while (newMsgs) {
      handleNewMessages(newMsgs);
      newMsgs = bot.getUpdates(bot.last_message_received + 1);
    }
    lastBotCheck = millis();
  }
}
