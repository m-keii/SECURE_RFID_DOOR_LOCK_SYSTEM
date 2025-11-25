// Secure RFID Door Lock System
  
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "SPIFFS.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <ArduinoJson.h>

// ----------- Pin Assignments -----------
#define SS_PIN 5        // RC522 SDA
#define RST_PIN 4       // RC522 Reset
#define RELAY_PIN 15    // Door Lock Output
#define BUZZER_PIN 12   // Buzzer Output
#define EXIT_BUTTON 13  // Exit Button
#define LED_READY 2     // Status LED
#define RESET_BTN 4     // System Reset (same as RC522 RST, will keep as is)

// LCD I2C pins handled by library: SDA 21, SCL 22

// ----------- Objects -----------
LiquidCrystal_I2C lcd(0x27, 20, 4);
MFRC522 rfid(SS_PIN, RST_PIN);
WebServer server(80);

// ----------- WiFi credentials -----------
char ssid[] = "DOOR";
char pass[] = "12345678";

// Admin credentials
const char* adminUser = "admin";
const char* adminPass = "1234";

bool wifiConnected = false;
bool loggedIn = false;

#define MAX_USERS 50
byte authorizedUIDs[MAX_USERS][4];
String userNames[MAX_USERS];
int userCount = 0;
int deniedAttempts = 0; // Tracks consecutive denied access attempts
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 5000; // check every 5 seconds
bool ledState = false;

// NTP client
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 28800, 60000); // GMT+8

// SPIFFS files
const char* logFile = "/logs.csv";
const char* userFile = "/users.json";

// ---------- Function Prototypes ----------
void connectToWiFi();
void unlockDoor(String user, bool isExit, bool authorized, String uid);
void logEvent(String name, String uid, String event);
String getTimeStamp();
void handleLogin();
void handleLogout();
void handleDashboard();
void handleManageUsers();
void handleAddUser();
void handleDeleteUser();
void handleLogs();
void handleCSV();
void handleClearLogs();
void saveUsersToFile();
void loadUsersFromFile();

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  SPI.begin();
  rfid.PCD_Init();
  lcd.init();
  lcd.backlight();
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Relay normally off
  pinMode(EXIT_BUTTON, INPUT_PULLUP);
  pinMode(LED_READY, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RESET_BTN, INPUT_PULLUP);

  // SPIFFS init
  if(!SPIFFS.begin(true)) {
    Serial.println("SPIFFS failed");
  }

  // Load persistent users
  loadUsersFromFile();

  // If no users exist, add initial users
  if(userCount == 0){
    byte initUIDs[5][4] = {
      {0xDE,0xAD,0xBE,0xEF},
      {0xAA,0xBB,0xCC,0xDD},
      {0x11,0x22,0x33,0x44},
      {0x55,0x66,0x77,0x88},
      {0x99,0xAA,0xBB,0xCC}
    };
    String initNames[5] = {"Alice","Bob","Charlie","David","Eve"};
    for(int i=0;i<5;i++){
      memcpy(authorizedUIDs[i],initUIDs[i],4);
      userNames[i] = initNames[i];
    }
    userCount=5;
    saveUsersToFile();
  }

  lcd.setCursor(0,1);
  lcd.print("Connecting WiFi...");
  connectToWiFi();
  if(wifiConnected){
    digitalWrite(LED_READY,HIGH);
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi Connected");
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP());
    timeClient.begin();
    timeClient.update();
  } else {
    digitalWrite(LED_READY,LOW);
    lcd.clear();
    lcd.setCursor(4,1);
    lcd.print("Offline Mode");
  }

  delay(2000);
  lcd.clear();
  lcd.setCursor(3,1);
  lcd.print("Scan Your Card");

  // --- Web Routes ---
  server.on("/", handleDashboard);
  server.on("/login", handleLogin);
  server.on("/logout", handleLogout);
  server.on("/manage", handleManageUsers);
  server.on("/add", handleAddUser);
  server.on("/delete", handleDeleteUser);
  server.on("/logs", handleLogs);
  server.on("/csv", handleCSV);
  server.on("/clearlogs", handleClearLogs);
  server.begin();
  Serial.println("System Ready");
}

// ---------- Loop ----------
void loop() {
  server.handleClient();

  // --- Reset button ---
  if(digitalRead(RESET_BTN) == LOW) {
    lcd.clear();
    lcd.setCursor(5,1);
    lcd.print("RESETTING");
    delay(200);
    ESP.restart();
  }

  // --- Exit button ---
  if(digitalRead(EXIT_BUTTON) == LOW) unlockDoor("Exit Button", true, true, "N/A");

  // --- RFID scan ---
  if(rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = "";
    for(byte i=0;i<rfid.uid.size;i++)
      uid += String(rfid.uid.uidByte[i], HEX);
    uid.toUpperCase();

    bool authorized = false;
    String name = "Unknown";

    for(int i=0;i<userCount;i++){
      String storedUID="";
      for(int j=0;j<4;j++)
        storedUID += String(authorizedUIDs[i][j], HEX);
      storedUID.toUpperCase();
      if(uid == storedUID){
        authorized = true;
        name = userNames[i];
        break;
      }
    }

    unlockDoor(name, false, authorized, uid);
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  // --- WiFi reconnect & LED status ---
  static unsigned long lastWiFiCheck = 0;
  static bool ledState = false;
  const unsigned long wifiCheckInterval = 5000; // check every 5 seconds

  if(millis() - lastWiFiCheck > wifiCheckInterval) {
    lastWiFiCheck = millis();

    if(WiFi.status() != WL_CONNECTED){
      // Attempt reconnect
      WiFi.begin(ssid, pass);

      // Blink LED
      ledState = !ledState;
      digitalWrite(LED_READY, ledState);

      // Show Offline Mode on LCD top row
      lcd.setCursor(3,0);
      lcd.print(" OFFLINE MODE      ");
    } else {
      // WiFi connected, solid LED
      digitalWrite(LED_READY,HIGH);

      // Show connected status on LCD top row
      lcd.setCursor(3,0);
      lcd.print("WiFi Connected    ");
    }

    // Always show scan prompt on the second row
    lcd.setCursor(3,1);
    lcd.print("Scan Your Card    ");
  }
}

// ---------- WiFi ----------
void connectToWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<10000){
    digitalWrite(LED_READY,HIGH);
    delay(200);
    digitalWrite(LED_READY,LOW);
    delay(200);
    if(digitalRead(RESET_BTN)==LOW) ESP.restart();
  }
  wifiConnected = (WiFi.status()==WL_CONNECTED);
}

// ---------- Door Unlock ----------
void unlockDoor(String user, bool isExit, bool authorized, String uid) {
  lcd.clear();
  if(isExit){
    lcd.setCursor(0,1);
    lcd.print("EXIT BUTTON");
    logEvent("Exit Button","N/A","Exit");
    digitalWrite(RELAY_PIN,LOW);
    delay(3000);
    digitalWrite(RELAY_PIN,HIGH);
    tone(BUZZER_PIN, 1000, 200); // short beep
    deniedAttempts = 0; // reset denied attempts
  } else {
    if(authorized){
      lcd.setCursor(3,0);
      lcd.print("ACCESS GRANTED");
      lcd.setCursor(2,1);
      lcd.print("Welcome:");
      lcd.setCursor(2,2);
      lcd.print(user);
      logEvent(user, uid, "Granted");
      digitalWrite(RELAY_PIN,LOW);
      tone(BUZZER_PIN, 1500, 200); // beep granted
      delay(3000);
      digitalWrite(RELAY_PIN,HIGH);
      deniedAttempts = 0; // reset counter on success
    } else {
      lcd.setCursor(4,0);
      lcd.print("ACCESS DENIED");
      lcd.setCursor(1,1);
      lcd.print("Unauthorized Access");
      logEvent(user, uid, "Denied");
      deniedAttempts++; // increment denied attempts
      if(deniedAttempts >= 3){
        lcd.setCursor(7,2);
        lcd.print("ALERT!");
        tone(BUZZER_PIN, 2000); // continuous high-pitch
        unsigned long start = millis();
        while(millis() - start < 10000){
          if(digitalRead(RESET_BTN) == LOW){
            noTone(BUZZER_PIN);
            ESP.restart();
          }
          delay(100);
        }
        noTone(BUZZER_PIN);
        deniedAttempts = 0;
      } else {
        tone(BUZZER_PIN, 500, 500); // short beep for denied attempt
        delay(2000);
      }
    }
  }
  lcd.clear();
  lcd.setCursor(3,1);
  lcd.print("Scan Your Card");
}

// ---------- Logging ----------
String getTimeStamp(){
  timeClient.update();
  unsigned long epochTime = timeClient.getEpochTime();
  time_t rawTime = epochTime;
  struct tm * timeInfo = localtime(&rawTime);
  String t = "";
  t += String(timeInfo->tm_mday) + "/";
  t += String(timeInfo->tm_mon + 1) + "/";
  t += String(timeInfo->tm_year + 1900) + " ";
  t += (timeInfo->tm_hour < 10 ? "0" : "") + String(timeInfo->tm_hour) + ":";
  t += (timeInfo->tm_min < 10 ? "0" : "") + String(timeInfo->tm_min) + ":";
  t += (timeInfo->tm_sec < 10 ? "0" : "") + String(timeInfo->tm_sec);
  return t;
}

void logEvent(String name, String uid, String event){
  String ts = getTimeStamp();
  File f = SPIFFS.open(logFile, FILE_APPEND);
  if(f){
    f.printf("%s,%s,%s,%s\n",ts.c_str(),name.c_str(),uid.c_str(),event.c_str());
    f.close();
  }
}

// ---------- Persistent Users ----------
void saveUsersToFile() {
  DynamicJsonDocument doc(1024);
  for (int i = 0; i < userCount; i++) {
    JsonObject user = doc.createNestedObject();
    user["name"] = userNames[i];
    String uidStr = "";
    for (int j = 0; j < 4; j++) uidStr += String(authorizedUIDs[i][j], HEX);
    user["uid"] = uidStr;
  }
  File f = SPIFFS.open(userFile, FILE_WRITE);
  if (f) { serializeJson(doc, f); f.close(); }
}

void loadUsersFromFile() {
  if (!SPIFFS.exists(userFile)) return;
  File f = SPIFFS.open(userFile, FILE_READ);
  if (!f) return;
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, f);
  if (err) { f.close(); return; }
  userCount = 0;
  for (JsonObject user : doc.as<JsonArray>()) {
    String name = user["name"].as<String>();
    String uid = user["uid"].as<String>();
    userNames[userCount] = name;
    for (int j = 0; j < 4; j++)
      authorizedUIDs[userCount][j] = strtoul(uid.substring(j*2,j*2+2).c_str(), NULL, 16);
    userCount++;
    if (userCount >= MAX_USERS) break;
  }
  f.close();
}

// ---------- Web Handlers ----------
// The rest (login, logout, dashboard, manage users) stays same, except title changed to SECURE RFID DOOR LOCK SYSTEM
// Dashboard: h1 text and login page title updated


// ---------- WEB HANDLERS ----------
// ... (All web handler code remains same as your original)
// Login page
void handleLogin() {
  if (server.hasArg("user") && server.hasArg("pass")) {
    if (server.arg("user") == adminUser && server.arg("pass") == adminPass) {
      loggedIn = true;
      server.sendHeader("Location", "/");
      server.send(302);
      return;
    }
  }

  String loginPage = R"rawliteral(
  <!DOCTYPE html>
  <html lang='en'>
  <head>
    <meta charset='UTF-8'>
    <title>Secure RFID Door Lock System</title>
    <style>
      @import url('https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap');
      body {
        background: linear-gradient(135deg, #0f2027, #203a43, #2c5364);
        font-family: 'Roboto', sans-serif;
        display: flex;
        justify-content: center;
        align-items: center;
        height: 100vh;
        margin: 0;
        color: #fff;
      }
      .login-container {
        background: rgba(255,255,255,0.05);
        padding: 40px;
        border-radius: 15px;
        box-shadow: 0 0 30px rgba(0,255,255,0.4);
        width: 360px;
        text-align: center;
        backdrop-filter: blur(10px);
      }
      .login-container img {
        width: 80px;
        margin-bottom: 20px;
      }
      .login-container h2 {
        margin-bottom: 30px;
        color: #0ff;
        font-weight: 700;
        letter-spacing: 1px;
      }
      input[type='text'], input[type='password'] {
        width: 100%;
        padding: 14px;
        margin: 10px 0;
        border-radius: 10px;
        border: none;
        background: rgba(255,255,255,0.1);
        color: #fff;
        font-size: 16px;
      }
      input::placeholder {
        color: #ccc;
      }
      button {
        width: 100%;
        padding: 14px;
        margin-top: 20px;
        border: none;
        border-radius: 10px;
        background: #0ff;
        color: #000;
        font-size: 16px;
        font-weight: bold;
        cursor: pointer;
        transition: 0.3s;
      }
      button:hover {
        background: #08f;
        color: #fff;
      }
      .footer {
        margin-top: 20px;
        font-size: 12px;
        color: #aaa;
      }
    </style>
  </head>
  <body>
    <div class='login-container'>
      <img src='https://img.icons8.com/ios-filled/100/0ff/lock--v1.png' alt='Padlock'>
      <h2>SECURE RFID DOOR LOCK SYSTEM</h2>
      <form action='/login'>
        <input name='user' placeholder='Username'><br>
        <input name='pass' type='password' placeholder='Password'><br>
        <button type='submit'>Login</button>
      </form>
      <div class='footer'>© 2025 CSUA - Access Control</div>
    </div>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", loginPage);
}


// Logout
void handleLogout(){ loggedIn=false; server.sendHeader("Location","/login"); server.send(302); }
void handleDashboard(){
  if(!loggedIn){ server.sendHeader("Location","/login"); server.send(302); return; }

  String html = R"rawliteral(
  <!DOCTYPE html><html><head><meta charset='utf-8'>
  <title>SECURE RFID DOOR LOCK SYSTEM Dashboard</title>
  <style>
  body{background:#0b0c10;color:#dfffe0;font-family:Arial;margin:0;}
  h1{color:#00ffcc;text-align:center;margin:20px;font-family:Verdana, Geneva, sans-serif;}
  button{background:#00ffcc;color:#000;padding:8px;border:none;border-radius:6px;cursor:pointer;margin:5px;}
  button:hover{background:#00cc99;color:#fff;}
  table{width:90%;margin:auto;border-collapse:collapse;}
  th,td{border:1px solid #333;padding:6px;text-align:center;}
  th{background:#00ffcc;color:#000;}
  tr:nth-child(even){background:#1c1c1c;}
  </style></head><body>
  <h1>SECURE RFID DOOR LOCK SYSTEM</h1>
  <div style='text-align:center; margin-bottom:20px;'>
    <a href='/manage'><button>Manage Users</button></a>
    <a href='/csv' target='_blank'><button>Download CSV</button></a>
    <a href='/clearlogs'><button>Clear Logs</button></a>
    <a href='/logout'><button>Logout</button></a>
  </div>
  <h2 style='text-align:center;'>Access Logs (Latest on Top)</h2>
  <table id='logs'><tr><th>Time</th><th>Name</th><th>UID</th><th>Event</th></tr></table>
  <script>
  async function refreshLogs(){
    let resp=await fetch('/logs'); 
    let text=await resp.text();
    let lines=text.trim().split('\n').reverse();
    let t=document.getElementById('logs'); 
    t.innerHTML='<tr><th>Time</th><th>Name</th><th>UID</th><th>Event</th></tr>';
    lines.forEach(l=>{
      if(l.trim().length==0) return;
      let c=l.split(',');
      let row=t.insertRow();
      row.insertCell(0).innerText=c[0]; // Time
      row.insertCell(1).innerText=c[1]; // Name
      row.insertCell(2).innerText=c[2]; // UID (full HEX, not altered)
      row.insertCell(3).innerText=c[3]; // Event
    });
  }
  setInterval(refreshLogs,3000);
  refreshLogs();
  </script></body></html>
  )rawliteral";

  server.send(200,"text/html",html);
}



// Manage Users
void handleManageUsers(){
  if(!loggedIn){ server.sendHeader("Location","/login"); server.send(302); return; }

  String html=R"rawliteral(
  <!DOCTYPE html>
  <html lang='en'>
  <head>
    <meta charset='UTF-8'>
    <title>Manage Users</title>
    <link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>
    <style>
      body{background:#121212;color:#eee;font-family:'Roboto',sans-serif;margin:0;padding:0;}
      header{background:#0ff;color:#000;padding:15px;text-align:center;font-size:24px;font-weight:bold;}
      .container{width:90%;max-width:800px;margin:auto;padding:20px;}
      .card{background:#1e1e1e;padding:20px;border-radius:12px;margin-bottom:20px;box-shadow:0 0 15px rgba(0,255,255,0.2);}
      input{padding:10px;margin:5px;border-radius:6px;border:none;width:calc(50% - 12px);}
      button{padding:10px;margin:5px;border:none;border-radius:6px;background:#0ff;color:#000;font-weight:bold;cursor:pointer;transition:0.3s;}
      button:hover{background:#08f;color:#fff;}
      table{width:100%;border-collapse:collapse;margin-top:10px;}
      th,td{border:1px solid #333;padding:8px;text-align:center;}
      th{background:#0ff;color:#000;}
      tr:nth-child(even){background:#1a1a1a;}
      tr:hover{background:#222;}
      h2{text-align:center;color:#0ff;margin-bottom:15px;}
    </style>
  </head>
  <body>
    <header>Manage Users</header>
    <div class='container'>
      <div class='card'>
        <h2>Add New User</h2>
        <input id='uid' placeholder='UID (HEX)'>
        <input id='name' placeholder='Full Name'>
        <button onclick='addUser()'>Add</button>
      </div>
      <div class='card'>
        <h2>Authorized Users</h2>
        <table id='users'><tr><th>Name</th><th>UID</th><th>Action</th></tr></table>
      </div>
    </div>
    <script>
      async function fetchJSON(u){ let r=await fetch(u); return await r.json(); }
      async function refreshUsers(){
        let u=await fetchJSON('/add?list=1');
        let t=document.getElementById('users'); t.innerHTML='<tr><th>Name</th><th>UID</th><th>Action</th></tr>';
        u.forEach(e=>{
          let row=t.insertRow();
          row.insertCell(0).innerText=e.name;
          row.insertCell(1).innerText=e.uid;
          let btn=row.insertCell(2).appendChild(document.createElement('button'));
          btn.innerText='Delete';
          btn.onclick=()=>{fetch('/delete?uid='+e.uid).then(()=>refreshUsers());};
        });
      }

      async function addUser(){
        let uid=document.getElementById('uid').value.trim();
        let name=document.getElementById('name').value.trim();
        if(uid && name){ await fetch('/add?uid='+uid+'&name='+name); refreshUsers(); }
      }

      setInterval(refreshUsers,3000);
      refreshUsers();
    </script>
  </body>
  </html>
  )rawliteral";

  server.send(200,"text/html",html);
}

// Add/Delete users
void handleAddUser(){
  if(!loggedIn){ server.send(403,"text/plain","Unauthorized"); return; }
  if(server.hasArg("list")){
    String json="["; for(int i=0;i<userCount;i++){ String uid=""; for(int j=0;j<4;j++) uid+=String(authorizedUIDs[i][j],HEX);
    json+="{\"name\":\""+userNames[i]+"\",\"uid\":\""+uid+"\"}"; if(i<userCount-1) json+=","; } json+="]";
    server.send(200,"application/json",json); return;
  }
  if(server.hasArg("uid") && server.hasArg("name") && userCount<MAX_USERS){
    String uid = server.arg("uid"); String name=server.arg("name");
    for(int j=0;j<4;j++) authorizedUIDs[userCount][j]= strtoul(uid.substring(j*2, j*2+2).c_str(),NULL,16);
    userNames[userCount]=name; userCount++;
    logEvent(name,uid,"Added");
    saveUsersToFile(); // persist
  }
  server.send(200,"text/plain","OK");
}

void handleDeleteUser(){
  if(!loggedIn){ server.send(403,"text/plain","Unauthorized"); return; }
  if(!server.hasArg("uid")){ server.send(400,"text/plain","Missing UID"); return; }
  String uid=server.arg("uid");
  for(int i=0;i<userCount;i++){
    String storedUID=""; for(int j=0;j<4;j++) storedUID+=String(authorizedUIDs[i][j],HEX);
    if(uid==storedUID){
      for(int k=i;k<userCount-1;k++){ memcpy(authorizedUIDs[k],authorizedUIDs[k+1],4); userNames[k]=userNames[k+1]; }
      userCount--; logEvent("Admin",uid,"Deleted"); saveUsersToFile(); break;
    }
  }
  server.send(200,"text/plain","OK");
}

// Logs
void handleLogs(){
  if(!loggedIn){ server.send(403,"text/plain","Unauthorized"); return; }
  File f=SPIFFS.open(logFile,"r"); String txt="";
  if(f){ while(f.available()){ txt+=char(f.read()); } f.close(); }
  server.send(200,"text/plain",txt);
}

// CSV download
void handleCSV(){
  if(!loggedIn){ server.send(403,"text/plain","Unauthorized"); return; }
  server.sendHeader("Content-Type","text/csv");
  server.sendHeader("Content-Disposition","attachment; filename=logs.csv");
  File f=SPIFFS.open(logFile,"r"); String txt="";
  if(f){ while(f.available()){ txt+=char(f.read()); } f.close(); }
  server.send(200,"text/csv",txt);
}

// Clear logs
void handleClearLogs(){
  if(!loggedIn){ server.send(403,"text/plain","Unauthorized"); return; }
  if(SPIFFS.exists(logFile)){ SPIFFS.remove(logFile); delay(100); }
  server.sendHeader("Location","/", true);
  server.send(302,"text/plain","Logs Cleared");
}



