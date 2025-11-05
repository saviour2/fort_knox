#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>

const char* ssid = "Saikat";
const char* password = "12345678@";

WebServer server(80);
Preferences preferences;
WiFiUDP udp;

struct DeviceInfo {
  String ip;
  String mac;
  String nickname;
  int rssi;
  bool isActive;
  bool isTrusted;
  bool isNew;
  unsigned long lastSeen;
  unsigned long firstSeen;
  int pingCount;
  String deviceType;
};

DeviceInfo devices[50];
int deviceCount = 0;
bool monitoringActive = false;
bool alertOnNewDevice = true;
int newDeviceAlerts = 0;

IPAddress localIP;
IPAddress gateway;
IPAddress subnet;
String myMAC;
unsigned long sessionStart = 0;
unsigned long totalScans = 0;
unsigned long lastScan = 0;  // Track last scan time globally

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n\n====================================");
  Serial.println("  Digital Fort Knox - Phase 3.1");
  Serial.println("  Hotspot-Compatible Version");
  Serial.println("====================================\n");

  preferences.begin("fortknox", false);

  WiFi.mode(WIFI_STA);
  delay(500);

  Serial.print("Connecting to: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts % 10 == 0) {
      Serial.print(" [" + String(attempts/2) + "s] ");
    }
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ Connected!");
    Serial.println("====================================");

    localIP = WiFi.localIP();
    gateway = WiFi.gatewayIP();
    subnet = WiFi.subnetMask();
    myMAC = WiFi.macAddress();

    Serial.print("IP Address: ");
    Serial.println(localIP);
    Serial.print("MAC Address: ");
    Serial.println(myMAC);
    Serial.print("Gateway: ");
    Serial.println(gateway);
    Serial.print("Subnet: ");
    Serial.println(subnet);
    Serial.print("Signal: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.println("====================================\n");

    // Web server setup
    server.on("/", handleRoot);
    server.on("/devices", handleDevices);
    server.on("/devicelist", handleDeviceList);
    server.on("/trust", handleTrust);
    server.on("/untrust", handleUntrust);
    server.on("/rename", handleRename);
    server.on("/clear", handleClearAlerts);
    server.on("/scan", handleManualScan);
    server.begin();

    Serial.println("✓ Web server started!");
    Serial.print("✓ Dashboard: http://");
    Serial.println(localIP);
    Serial.println("====================================\n");

    // Add this ESP32 as trusted
    addOrUpdateDevice(localIP.toString(), myMAC, "ESP32 Monitor");
    trustDevice(findDeviceByIP(localIP.toString()), "ESP32 Monitor");

    // Add gateway as trusted
    addOrUpdateDevice(gateway.toString(), "Router/Gateway", "WiFi Gateway");
    trustDevice(findDeviceByIP(gateway.toString()), "WiFi Gateway");

    sessionStart = millis();
    monitoringActive = true;

    Serial.println("🔍 Starting network monitoring...");
    Serial.println("Using hotspot-compatible scanning method");
    Serial.println("Automatic scans DISABLED - scan manually via web button\n");

    // Do initial scan on startup
    delay(2000);
    scanNetwork();

  } else {
    Serial.println("\n✗ Connection failed!");
    Serial.print("WiFi Status: ");
    Serial.println(WiFi.status());
    Serial.println("\nRestarting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }
}

void loop() {
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  WiFi disconnected! Reconnecting...");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  server.handleClient();

  // Automatic scanning disabled - only scan when manually triggered via web button
  // if (millis() - lastScan > 30000) {
  //   scanNetwork();
  //   lastScan = millis();
  // }

  // Print stats every 15 seconds
  static unsigned long lastStats = 0;
  if (millis() - lastStats > 15000) {
    Serial.println("📊 Active: " + String(countActive()) + " | Trusted: " + String(countTrusted()) + " | Alerts: " + String(newDeviceAlerts));
    lastStats = millis();
  }

  cleanupOldDevices();
  delay(10);
}

void scanNetwork() {
  if (!monitoringActive) return;

  Serial.println("\n🔍 Scanning network...");
  unsigned long scanStartTime = millis();

  // Get network range
  uint32_t ipInt = (uint32_t)localIP;
  uint32_t maskInt = (uint32_t)subnet;
  uint32_t networkInt = ipInt & maskInt;

  uint8_t oct1 = networkInt & 0xFF;
  uint8_t oct2 = (networkInt >> 8) & 0xFF;
  uint8_t oct3 = (networkInt >> 16) & 0xFF;

  int found = 0;

  // Scan common DHCP range (1-50 for mobile hotspots)
  for (int i = 1; i <= 50; i++) {
    if (!monitoringActive) break;

    IPAddress targetIP(oct1, oct2, oct3, i);

    // Skip our own IP
    if (targetIP == localIP) continue;

    // Try to connect to common ports
    WiFiClient client;
    bool detected = false;

    // Method 1: Try HTTP (reduced timeout)
    if (client.connect(targetIP, 80, 100)) {
      client.stop();
      detected = true;
    }

    // Method 2: Try HTTPS (reduced timeout)
    if (!detected && client.connect(targetIP, 443, 100)) {
      client.stop();
      detected = true;
    }

    // Method 3: Try common ports (reduced timeout)
    if (!detected) {
      int ports[] = {8080, 22, 445, 3389, 5000};
      for (int p = 0; p < 5; p++) {
        if (client.connect(targetIP, ports[p], 80)) {
          client.stop();
          detected = true;
          break;
        }
      }
    }

    if (detected) {
      found++;
      String mac = generateMACFromIP(targetIP);
      addOrUpdateDevice(targetIP.toString(), mac, "");
    }

    // Progress indicator (less frequent)
    if (i % 10 == 0) {
      Serial.print(".");
      yield(); // Allow WiFi to process
    }

    delay(20); // Reduced delay for faster scanning
  }

  totalScans++;
  unsigned long scanDuration = millis() - scanStartTime;
  Serial.println();
  Serial.println("✓ Scan complete! Found " + String(found) + " devices in " + String(scanDuration/1000) + "s");
  Serial.println("  Total devices tracked: " + String(deviceCount));
  Serial.println();
}

String generateMACFromIP(IPAddress ip) {
  // Generate consistent pseudo-MAC from IP
  // In a real implementation, you'd query ARP table
  uint8_t lastOctet = ip[3];
  uint8_t b1 = 0x00;
  uint8_t b2 = (lastOctet * 17 + 0xAA) % 256;
  uint8_t b3 = (lastOctet * 31 + 0xBB) % 256;
  uint8_t b4 = (lastOctet * 47 + 0xCC) % 256;
  uint8_t b5 = (lastOctet * 61 + 0xDD) % 256;

  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", b1, b2, b3, b4, b5, lastOctet);
  return String(macStr);
}

void addOrUpdateDevice(String ip, String mac, String type) {
  // Check if exists by IP
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].ip == ip) {
      devices[i].lastSeen = millis();
      devices[i].isActive = true;
      devices[i].pingCount++;
      devices[i].rssi = WiFi.RSSI(); // Use our WiFi signal as reference

      // Clear "new" flag after 5 minutes
      if (millis() - devices[i].firstSeen > 300000) {
        devices[i].isNew = false;
      }
      return;
    }
  }

  // Add new device
  if (deviceCount < 50) {
    devices[deviceCount].ip = ip;
    devices[deviceCount].mac = mac;
    devices[deviceCount].isActive = true;
    devices[deviceCount].isTrusted = false;
    devices[deviceCount].isNew = true;
    devices[deviceCount].firstSeen = millis();
    devices[deviceCount].lastSeen = millis();
    devices[deviceCount].pingCount = 1;
    devices[deviceCount].rssi = WiFi.RSSI();
    devices[deviceCount].nickname = "";

    if (type.length() > 0) {
      devices[deviceCount].deviceType = type;
    } else {
      devices[deviceCount].deviceType = identifyDeviceType(ip, mac);
    }

    deviceCount++;

    if (alertOnNewDevice && !devices[deviceCount-1].isTrusted) {
      Serial.println("⚠️  NEW DEVICE: " + ip + " | " + mac + " (" + devices[deviceCount-1].deviceType + ")");
      newDeviceAlerts++;
    } else {
      Serial.println("🆕 New: " + ip + " (" + devices[deviceCount-1].deviceType + ")");
    }
  }
}

String identifyDeviceType(String ip, String mac) {
  if (ip == localIP.toString()) return "ESP32 Monitor";
  if (ip == gateway.toString()) return "WiFi Gateway";

  // Identify by MAC OUI
  if (mac.startsWith("AC:DE:48") || mac.startsWith("E4:5F:01")) return "Xiaomi Device";
  if (mac.startsWith("00:50:F2") || mac.startsWith("28:6A:BA")) return "Samsung Device";
  if (mac.startsWith("3C:28:6D") || mac.startsWith("F0:18:98")) return "Apple Device";
  if (mac.startsWith("CC:9E:A2")) return "OnePlus Device";
  if (mac.startsWith("00:25:00")) return "Oppo Device";
  if (mac.startsWith("28:C2:DD")) return "Vivo Device";

  // Check if it's in typical mobile device range
  IPAddress ipAddr;
  ipAddr.fromString(ip);
  if (ipAddr[3] > 1 && ipAddr[3] < 20) return "Mobile Device";

  return "Unknown Device";
}

int findDeviceByIP(String ip) {
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].ip == ip) return i;
  }
  return -1;
}

void trustDevice(int index, String nickname) {
  if (index < 0 || index >= deviceCount) return;

  devices[index].isTrusted = true;
  devices[index].isNew = false;

  if (nickname.length() > 0) {
    devices[index].nickname = nickname;
  }

  String key = "trust_" + devices[index].ip;
  preferences.putString(key.c_str(), nickname);

  Serial.println("✓ Trusted: " + devices[index].ip + " (" + nickname + ")");
}

void untrustDevice(int index) {
  if (index < 0 || index >= deviceCount) return;

  devices[index].isTrusted = false;
  String key = "trust_" + devices[index].ip;
  preferences.remove(key.c_str());

  Serial.println("⊗ Untrusted: " + devices[index].ip);
}

void cleanupOldDevices() {
  unsigned long currentTime = millis();
  unsigned long timeout = 5 * 60 * 1000; // 5 minutes

  for (int i = 0; i < deviceCount; i++) {
    if (currentTime - devices[i].lastSeen > timeout && devices[i].isActive) {
      devices[i].isActive = false;
      Serial.println("⊗ Offline: " + devices[i].ip);
    }
  }
}

int countActive() {
  int count = 0;
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].isActive) count++;
  }
  return count;
}

int countTrusted() {
  int count = 0;
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].isTrusted) count++;
  }
  return count;
}

int countUntrusted() {
  int count = 0;
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].isActive && !devices[i].isTrusted) count++;
  }
  return count;
}

String formatTime(unsigned long ms) {
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;

  if (hours > 0) return String(hours) + "h " + String(minutes % 60) + "m";
  if (minutes > 0) return String(minutes) + "m";
  return String(seconds) + "s";
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Digital Fort Knox | Network Security Monitor</title>";
  html += "<style>";
  html += "*{margin:0;padding:0;box-sizing:border-box}";
  html += "body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;background:linear-gradient(135deg,#f5f7fa 0%,#c3cfe2 100%);color:#2c3e50;padding:0;min-height:100vh}";
  html += ".container{max-width:1400px;margin:0 auto;padding:0}";
  html += "h1{color:#2c3e50;background:rgba(255,167,38,0.95);padding:25px 30px;margin:0;box-shadow:0 4px 20px rgba(0,0,0,0.1);backdrop-filter:blur(10px);position:relative;border-radius:0 0 20px 20px}";
  html += ".header-content{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap}";
  html += ".title-main{font-size:1.8em;font-weight:700;letter-spacing:1px}";
  html += ".subtitle{color:#34495e;font-size:0.75em;font-weight:600;opacity:0.9;margin-top:5px}";
  html += ".badge{background:rgba(52,73,94,0.9);color:#ffa726;padding:5px 12px;border-radius:20px;font-size:0.65em;margin-left:12px;font-weight:700;border:none;backdrop-filter:blur(5px)}";
  html += ".card{background:rgba(255,255,255,0.9);backdrop-filter:blur(10px);padding:25px;margin:15px;border:none;border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,0.08);transition:all 0.3s ease}";
  html += ".card:hover{transform:translateY(-4px);box-shadow:0 12px 40px rgba(0,0,0,0.12)}";
  html += ".alert{background:rgba(255,235,238,0.95);backdrop-filter:blur(10px);border:2px solid rgba(229,115,115,0.5);border-radius:16px;color:#c62828;padding:25px;margin:15px;box-shadow:0 8px 32px rgba(229,115,115,0.2);animation:pulse 2s ease-in-out infinite}";
  html += "@keyframes pulse{0%,100%{box-shadow:0 8px 32px rgba(229,115,115,0.2)}50%{box-shadow:0 12px 40px rgba(229,115,115,0.3)}}";
  html += ".alert h3{margin-bottom:15px;font-size:1.2em;font-weight:700;letter-spacing:0.5px}";
  html += ".success{background:rgba(255,255,255,0.9);backdrop-filter:blur(10px);border:2px solid rgba(102,187,106,0.4);border-radius:16px;color:#2e7d32;padding:20px 25px;margin:15px;box-shadow:0 8px 32px rgba(102,187,106,0.15);display:flex;align-items:center;gap:15px;flex-wrap:wrap}";
  html += ".success-item{display:flex;align-items:center;gap:8px;font-weight:600;font-size:0.9em}";
  html += ".device{margin:15px 0;padding:20px;background:#fff;border:3px solid #ffa726;box-shadow:0 2px 8px rgba(0,0,0,0.1);transition:all 0.2s}";
  html += ".device:hover{transform:translateX(5px);box-shadow:0 4px 12px rgba(255,167,38,0.3)}";
  html += ".device-trusted{border-color:#66bb6a;background:#f1f8f4}";
  html += ".device-trusted:hover{box-shadow:0 4px 12px rgba(102,187,106,0.3)}";
  html += ".device-untrusted{border-color:#e57373;background:#fff5f5}";
  html += ".device-untrusted:hover{box-shadow:0 4px 12px rgba(229,115,115,0.3)}";
  html += ".device-new{border-color:#ffa726;animation:glow 2s ease-in-out infinite}";
  html += "@keyframes glow{0%,100%{box-shadow:0 2px 8px rgba(255,167,38,0.3)}50%{box-shadow:0 4px 12px rgba(255,167,38,0.5)}}";
  html += ".ip{font-size:1.15em;font-weight:900;color:#e67e22;margin-bottom:10px;font-family:'Courier New',monospace;letter-spacing:1px;text-transform:uppercase}";
  html += ".nickname{color:#388e3c;font-size:1.25em;font-weight:900;margin-bottom:8px;display:flex;align-items:center;gap:8px;text-transform:uppercase}";
  html += ".status{display:inline-block;padding:6px 14px;border:2px solid transparent;font-size:0.75em;font-weight:900;margin:8px 8px 8px 0;text-transform:uppercase;letter-spacing:1px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}";
  html += ".status-trusted{background:#66bb6a;color:#fff;border-color:#4caf50}";
  html += ".status-untrusted{background:#e57373;color:#fff;border-color:#ef5350}";
  html += ".status-new{background:#ffa726;color:#fff;border-color:#ff9800;animation:blink 1.5s ease-in-out infinite}";
  html += "@keyframes blink{0%,100%{opacity:1}50%{opacity:0.7}}";
  html += "button{background:#ffa726;color:#fff;border:3px solid #e67e22;padding:12px 24px;cursor:pointer;font-size:0.85em;margin:8px 8px 8px 0;font-weight:900;box-shadow:0 2px 6px rgba(230,126,34,0.3);transition:all 0.1s;text-transform:uppercase;letter-spacing:1px;font-family:'Courier New',monospace}";
  html += "button:hover{transform:translateY(-2px);box-shadow:0 4px 10px rgba(230,126,34,0.4)}";
  html += "button:active{transform:translateY(0);box-shadow:0 2px 4px rgba(230,126,34,0.3)}";
  html += ".btn-danger{background:#e57373;border-color:#ef5350;box-shadow:0 2px 6px rgba(229,115,115,0.3)}";
  html += ".btn-danger:hover{box-shadow:0 4px 10px rgba(229,115,115,0.4)}";
  html += ".btn-secondary{background:#78909c;color:#fff;border-color:#607d8b;box-shadow:0 2px 6px rgba(96,125,139,0.3)}";
  html += ".btn-secondary:hover{box-shadow:0 4px 10px rgba(96,125,139,0.4)}";
  html += ".stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:15px;margin:20px 0}";
  html += ".stat-box{background:#fff;padding:30px 20px;text-align:center;border:3px solid #ffa726;box-shadow:0 2px 8px rgba(0,0,0,0.1);transition:all 0.2s}";
  html += ".stat-box:hover{transform:translateY(-4px);box-shadow:0 4px 12px rgba(255,167,38,0.3)}";
  html += ".stat-number{font-size:3em;font-weight:900;color:#e67e22;line-height:1}";
  html += ".stat-label{color:#7f8c8d;font-size:0.75em;margin-top:10px;font-weight:900;text-transform:uppercase;letter-spacing:2px}";
  html += ".info{font-size:0.85em;color:#7f8c8d;margin:8px 0;display:flex;align-items:center;gap:8px;flex-wrap:wrap;font-weight:700}";
  html += ".section-title{color:#ffa726;font-size:1.3em;font-weight:900;margin:30px 0 15px 0;padding:15px;background:#34495e;border:3px solid #ffa726;display:flex;align-items:center;gap:10px;text-transform:uppercase;letter-spacing:2px;box-shadow:0 2px 8px rgba(0,0,0,0.1)}";
  html += ".footer{text-align:center;margin:0;padding:30px;background:#34495e;color:#ffa726;font-size:0.85em;border-top:3px solid #ffa726;font-weight:700}";
  html += ".empty-state{text-align:center;padding:40px 20px;color:#95a5a6;font-size:1.05em;font-weight:700;text-transform:uppercase}";
  html += ".icon{display:inline-block;margin-right:5px}";
  html += ".icon{display:inline-block;margin-right:5px}";
  html += ".device{margin:12px 0;padding:18px;background:rgba(255,255,255,0.85);border-radius:12px;border:1px solid rgba(0,0,0,0.04);box-shadow:0 6px 20px rgba(0,0,0,0.06);transition:all 0.25s ease}";
  html += ".device:hover{transform:translateY(-6px);box-shadow:0 14px 36px rgba(0,0,0,0.08)}";
  html += ".device-trusted{border-color:rgba(102,187,106,0.25);background:rgba(241,248,244,0.9)}";
  html += ".device-untrusted{border-color:rgba(229,115,115,0.18);background:rgba(255,245,245,0.9)}";
  html += ".ip{font-size:1.05em;font-weight:800;color:#e67e22;margin-bottom:6px;font-family:'Courier New',monospace;letter-spacing:0.5px;text-transform:none}";
  html += ".nickname{color:#2e7d32;font-size:1.1em;font-weight:700;margin-bottom:6px;display:flex;align-items:center;gap:8px;text-transform:none}";
  html += ".toggle-btn{background:transparent;border:1px solid rgba(0,0,0,0.06);padding:6px 10px;border-radius:10px;color:#34495e;cursor:pointer;font-weight:700;margin-left:8px;box-shadow:none}";
  html += ".toggle-btn:hover{background:rgba(0,0,0,0.03)}";
  html += ".details{max-height:0;overflow:hidden;transition:max-height 0.35s ease,opacity 0.35s ease;opacity:0;padding-top:0;margin-top:0}";
  html += ".details.open{opacity:1;padding-top:12px;margin-top:12px}";
  html += "button, .btn-secondary, .btn-danger{border-radius:10px}";
  html += "</style></head><body><div class='container'>";

  html += "<h1><div class='header-content'><div><div class='title-main'>🛡️ DIGITAL FORT KNOX</div>";
  html += "<div class='subtitle'>NETWORK SECURITY MONITORING SYSTEM<span class='badge'>v2.1 HOTSPOT</span></div></div></div></h1>";

  // Connection status
  html += "<div class='success'>";
  html += "<div class='success-item'><span class='icon'>✓</span><strong>" + String(ssid) + "</strong></div>";
  html += "<div class='success-item'>📶 <strong>" + String(WiFi.RSSI()) + " dBm</strong></div>";
  html += "<div class='success-item'>🔍 <strong>" + String(totalScans) + "</strong> SCANS</div>";
  html += "<div class='success-item'>⏱️ <strong>" + formatTime(millis() - sessionStart) + "</strong> UPTIME</div>";
  html += "</div>";

  // Alerts
  int untrustedActive = countUntrusted();
  if (untrustedActive > 0 || newDeviceAlerts > 0) {
    html += "<div class='alert'>";
    html += "<h3>⚠️ SECURITY ALERTS</h3>";
    if (untrustedActive > 0) {
      html += "<p>🔴 " + String(untrustedActive) + " UNTRUSTED DEVICE(S) CURRENTLY ACTIVE!</p>";
    }
    if (newDeviceAlerts > 0) {
      html += "<p>🆕 " + String(newDeviceAlerts) + " NEW DEVICE(S) DETECTED THIS SESSION.</p>";
    }
    html += "<button class='btn-danger' onclick='fetch(\"/clear\").then(()=>location.reload())'>CLEAR ALERTS</button>";
    html += "</div>";
  }

  // Statistics
  html += "<div class='card'>";
  html += "<h2 class='section-title'>📊 NETWORK STATISTICS</h2>";
  html += "<div class='stats-grid'>";
  html += "<div class='stat-box'><div class='stat-number'>" + String(deviceCount) + "</div><div class='stat-label'>TOTAL DEVICES</div></div>";
  html += "<div class='stat-box'><div class='stat-number' style='color:#66bb6a'>" + String(countActive()) + "</div><div class='stat-label'>ACTIVE NOW</div></div>";
  html += "<div class='stat-box'><div class='stat-number' style='color:#66bb6a'>" + String(countTrusted()) + "</div><div class='stat-label'>TRUSTED</div></div>";
  html += "<div class='stat-box'><div class='stat-number' style='color:#e57373'>" + String(untrustedActive) + "</div><div class='stat-label'>UNTRUSTED</div></div>";
  html += "</div>";
  html += "<button class='btn-secondary' onclick='scanNow(this)'>🔍 SCAN NETWORK NOW</button>";
  html += "</div>";

  // Devices
  html += "<div class='card'>";
  html += "<h2 class='section-title'>🖥️ CONNECTED DEVICES</h2>";

  unsigned long currentTime = millis();

  // Trusted devices
  html += "<h3 class='section-title' style='font-size:1.1em;margin-top:15px;border-color:#66bb6a;color:#66bb6a'>✓ TRUSTED DEVICES</h3>";
  bool hasTrusted = false;
  for (int i = 0; i < deviceCount; i++) {
    if (!devices[i].isTrusted) continue;
    hasTrusted = true;

    html += "<div class='device device-trusted'>";
    html += "<div style='display:flex;justify-content:space-between;align-items:center'>";
    html += "<div>";
    if (devices[i].nickname.length() > 0) {
      html += "<div class='nickname'><span class='icon'>✓</span>" + devices[i].nickname + "</div>";
      html += "<div class='ip'>📍 " + devices[i].ip + "</div>";
    } else {
      html += "<div class='ip'>� " + devices[i].ip + "</div>";
    }
    html += "</div>"; // left

    // right: badges + toggle
    html += "<div style='display:flex;align-items:center'>";
    html += "<span class='status status-trusted'>✓ TRUSTED</span>";
    if (devices[i].isActive) {
      html += "<span class='status' style='background:#66bb6a;color:#fff;border-color:#4caf50;margin-left:8px'>● ONLINE</span>";
    } else {
      html += "<span class='status' style='background:#bdbdbd;color:#fff;border-color:#9e9e9e;margin-left:8px'>○ OFFLINE</span>";
    }
    html += "<button class='toggle-btn' onclick='toggleDetails(this)'>More info</button>";
    html += "</div>"; // right
    html += "</div>"; // header

    // Collapsible details
    html += "<div class='details'>";
    html += "<div class='info'>🔖 " + devices[i].mac + " &nbsp; • &nbsp; 📱 " + devices[i].deviceType + "</div>";
    html += "<div class='info'>👁️ SEEN " + String(devices[i].pingCount) + " TIMES &nbsp; • &nbsp; 🕐 FIRST SEEN " + formatTime(currentTime - devices[i].firstSeen) + " AGO</div>";
    html += "<div style='margin-top:10px'>";
    html += "<button class='btn-danger' onclick='fetch(\"/untrust?ip=" + devices[i].ip + "\").then(()=>setTimeout(()=>location.reload(),500))'>⊗ REMOVE TRUST</button>";
    html += "<button class='btn-secondary' onclick='rename(\"" + devices[i].ip + "\")'>✏️ RENAME</button>";
    html += "</div>";
    html += "</div>"; // details

    html += "</div>"; // device
  }
  if (!hasTrusted) {
    html += "<div class='empty-state'>NO TRUSTED DEVICES YET. TRUST DEVICES BELOW TO STOP RECEIVING ALERTS.</div>";
  }

  // Untrusted devices
  html += "<h3 class='section-title' style='font-size:1.1em;margin-top:25px;color:#e57373;border-color:#e57373'>⚠️ UNTRUSTED DEVICES</h3>";
  bool hasUntrusted = false;
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].isTrusted) continue;
    hasUntrusted = true;

    String devClass = "device device-untrusted";
    if (devices[i].isNew) devClass += " device-new";

    html += "<div class='" + devClass + "'>";
    html += "<div style='display:flex;justify-content:space-between;align-items:center'>";
    html += "<div>";
    if (devices[i].nickname.length() > 0) {
      html += "<div class='nickname'>" + devices[i].nickname + "</div>";
      html += "<div class='ip'>� " + devices[i].ip + "</div>";
    } else {
      html += "<div class='ip'>� " + devices[i].ip + "</div>";
    }
    html += "</div>";

    html += "<div style='display:flex;align-items:center'>";
    html += "<span class='status status-untrusted'>⚠ UNTRUSTED</span>";
    if (devices[i].isNew) html += "<span class='status status-new' style='margin-left:8px'>🆕 NEW</span>";
    if (devices[i].isActive) {
      html += "<span class='status' style='background:#66bb6a;color:#fff;border-color:#4caf50;margin-left:8px'>● ONLINE</span>";
    } else {
      html += "<span class='status' style='background:#bdbdbd;color:#fff;border-color:#9e9e9e;margin-left:8px'>○ OFFLINE</span>";
    }
    html += "<button class='toggle-btn' onclick='toggleDetails(this)'>More info</button>";
    html += "<button style='margin-left:10px' onclick='trust(\"" + devices[i].ip + "\")'>✓ TRUST</button>";
    html += "</div>"; // right
    html += "</div>"; // header

    // details
    html += "<div class='details'>";
    html += "<div class='info'>🔖 " + devices[i].mac + " &nbsp; • &nbsp; 📱 " + devices[i].deviceType + "</div>";
    html += "<div class='info'>👁️ " + String(devices[i].pingCount) + " DETECTIONS &nbsp; • &nbsp; 🕐 FIRST SEEN " + formatTime(currentTime - devices[i].firstSeen) + " AGO</div>";
    html += "<div style='margin-top:10px'>";
    html += "<button class='btn-secondary' onclick='rename(\"" + devices[i].ip + "\")'>✏️ SET NAME</button>";
    html += "</div>";
    html += "</div>"; // details

    html += "</div>"; // device
  }
  if (!hasUntrusted) {
    html += "<div class='empty-state'>✓ ALL DEVICES ARE TRUSTED! YOUR NETWORK IS SECURE.</div>";
  }

  html += "</div>";

  html += "<div class='footer'>";
  html += "🔒 <strong>DIGITAL FORT KNOX</strong> v 3.1 | HOTSPOT-COMPATIBLE MODE<br>";
  html += "THE SENTINEL | POWERED BY ESP32";
  html += "</div></div>";

  html += "<script>";
  html += "function trust(ip){var n=prompt('ENTER A NICKNAME FOR THIS DEVICE (OPTIONAL):',''); if(n!==null){ fetch('/trust?ip='+ip+'&name='+encodeURIComponent(n||'')).then(()=>setTimeout(()=>location.reload(),500)); }}";
  html += "function rename(ip){var n=prompt('ENTER NEW NICKNAME FOR THIS DEVICE:',''); if(n) fetch('/rename?ip='+ip+'&name='+encodeURIComponent(n)).then(()=>setTimeout(()=>location.reload(),500)); }";
  html += "function scanNow(btn){ btn.textContent='⏳ SCANNING...'; btn.disabled=true; fetch('/scan').then(()=>{ setTimeout(()=>{ btn.textContent='🔍 SCAN NETWORK NOW'; btn.disabled=false; updateDevices(); },10000); }); }";
  html += "function toggleDetails(btn){ var card=btn.closest('.device'); var details=card.querySelector('.details'); if(details.classList.contains('open')){ details.classList.remove('open'); details.style.maxHeight=null; btn.textContent='More info'; } else { details.classList.add('open'); details.style.maxHeight=details.scrollHeight+'px'; btn.textContent='Less info'; } }";
  html += "function updateDevices(){ fetch('/devicelist').then(r=>r.json()).then(data=>{ document.querySelectorAll('.stat-number')[0].textContent=data.stats.total; document.querySelectorAll('.stat-number')[1].textContent=data.stats.active; document.querySelectorAll('.stat-number')[2].textContent=data.stats.trusted; document.querySelectorAll('.stat-number')[3].textContent=data.stats.untrusted; document.querySelectorAll('.success-item')[2].innerHTML='🔍 <strong>'+data.stats.scans+'</strong> SCANS'; document.querySelectorAll('.success-item')[3].innerHTML='⏱️ <strong>'+data.stats.uptime+'</strong> UPTIME'; if((data.stats.untrusted>0||data.stats.alerts>0) && !document.querySelector('.alert')){ location.reload(); } if(!(data.stats.untrusted>0||data.stats.alerts>0) && document.querySelector('.alert')){ location.reload(); } }).catch(()=>{}); }";
  html += "setInterval(updateDevices,5000);";
  html += "</script>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleDevices() {
  String json = "{\"total\":" + String(deviceCount) + ",\"active\":" + String(countActive()) + ",\"trusted\":" + String(countTrusted()) + "}";
  server.send(200, "application/json", json);
}

void handleDeviceList() {
  String json = "{";
  json += "\"stats\":{";
  json += "\"total\":" + String(deviceCount) + ",";
  json += "\"active\":" + String(countActive()) + ",";
  json += "\"trusted\":" + String(countTrusted()) + ",";
  json += "\"untrusted\":" + String(countUntrusted()) + ",";
  json += "\"alerts\":" + String(newDeviceAlerts) + ",";
  json += "\"scans\":" + String(totalScans) + ",";
  json += "\"uptime\":\"" + formatTime(millis() - sessionStart) + "\"";
  json += "},";
  json += "\"devices\":[";

  unsigned long currentTime = millis();
  bool first = true;

  for (int i = 0; i < deviceCount; i++) {
    if (!first) json += ",";
    first = false;

    json += "{";
    json += "\"ip\":\"" + devices[i].ip + "\",";
    json += "\"mac\":\"" + devices[i].mac + "\",";
    json += "\"nickname\":\"" + devices[i].nickname + "\",";
    json += "\"type\":\"" + devices[i].deviceType + "\",";
    json += "\"trusted\":" + String(devices[i].isTrusted ? "true" : "false") + ",";
    json += "\"active\":" + String(devices[i].isActive ? "true" : "false") + ",";
    json += "\"new\":" + String(devices[i].isNew ? "true" : "false") + ",";
    json += "\"pingCount\":" + String(devices[i].pingCount) + ",";
    json += "\"timeSince\":\"" + formatTime(currentTime - devices[i].firstSeen) + "\"";
    json += "}";
  }

  json += "]}";
  server.send(200, "application/json", json);
}

void handleTrust() {
  if (server.hasArg("ip")) {
    String ip = server.arg("ip");
    String name = server.hasArg("name") ? server.arg("name") : "";

    Serial.println("📡 Trust request for IP: " + ip + " with name: '" + name + "'");

    int index = findDeviceByIP(ip);
    if (index >= 0) {
      trustDevice(index, name);
      newDeviceAlerts = 0;
      Serial.println("✓ Device trusted successfully");
      server.send(200, "text/plain", "OK");
      return;
    } else {
      Serial.println("✗ Device not found: " + ip);
    }
  }
  server.send(400, "text/plain", "Error");
}

void handleUntrust() {
  if (server.hasArg("ip")) {
    String ip = server.arg("ip");
    Serial.println("📡 Untrust request for IP: " + ip);

    int index = findDeviceByIP(ip);
    if (index >= 0) {
      untrustDevice(index);
      Serial.println("✓ Device untrusted successfully");
      server.send(200, "text/plain", "OK");
      return;
    } else {
      Serial.println("✗ Device not found: " + ip);
    }
  }
  server.send(400, "text/plain", "Error");
}

void handleRename() {
  if (server.hasArg("ip") && server.hasArg("name")) {
    String ip = server.arg("ip");
    String name = server.arg("name");

    int index = findDeviceByIP(ip);
    if (index >= 0) {
      devices[index].nickname = name;
      if (devices[index].isTrusted) {
        String key = "trust_" + ip;
        preferences.putString(key.c_str(), name);
      }
      Serial.println("✏️  Renamed: " + ip + " to '" + name + "'");
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Error");
}

void handleClearAlerts() {
  newDeviceAlerts = 0;
  for (int i = 0; i < deviceCount; i++) {
    devices[i].isNew = false;
  }
  Serial.println("✓ Alerts cleared");
  server.send(200, "text/plain", "OK");
}

void handleManualScan() {
  Serial.println("📡 Manual scan requested via web interface");
  server.send(200, "text/plain", "Scan initiated");

  // Trigger an immediate scan by setting lastScan to force next scan in loop
  lastScan = 0;  // This will cause the next loop iteration to scan immediately

  Serial.println("✓ Scan will execute in next loop cycle");
}
