#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Fingerprint.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

/* ---------- Hardware Config ---------- */
HardwareSerial mySerial(1);                 
Adafruit_Fingerprint finger(&mySerial);

#define RELAY_PIN 26
#define BUTTON_WIFI 5
#define BUTTON_RESET 25
#define LONG_PRESS_DURATION 5000   // 5 sec

/* ---------- WiFi Hotspot Config ---------- */
const char* apSSID = "SmartBikeAPI";
const char* apPassword = "123456789";

/* ---------- WebServer ---------- */
WebServer server(80);

/* ---------- Global Variables ---------- */
bool hotspotMode = true;      
unsigned long pressStart = 0;
uint8_t lastID = 0;
#define MAX_FINGERPRINTS 300  

/* ---------- Button Variables ---------- */
unsigned long wifiPressStart = 0;
bool wifiButtonState = false;
unsigned long resetPressStart = 0;
bool resetButtonState = false;

/* ---------- LittleFS JSON File ---------- */
#define FILE_PATH "/fingerprints.json"
DynamicJsonDocument jsonDoc(4096);
Preferences prefs;
String adminUser = "admin";
String adminPass = "";
String currentToken = "";

/* ---------- Relay Control ---------- */
unsigned long relayStart = 0;
bool relayOn = false;
const int RELAY_DURATION = 2000;  // 2 sec

/* ---------- Helper Functions ---------- */
void saveFingerprintData(JsonArray& arr) {
  File file = LittleFS.open(FILE_PATH, "w");
  if (!file) { Serial.println("Failed to open file for writing"); return; }
  serializeJson(arr, file);
  file.close();
}

JsonArray loadFingerprintData(DynamicJsonDocument& doc) {
  if (!LittleFS.exists(FILE_PATH)) return doc.to<JsonArray>();
  File file = LittleFS.open(FILE_PATH, "r");
  if (!file) { Serial.println("Failed to open file for reading"); return doc.to<JsonArray>(); }
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) { Serial.println("Failed to parse JSON"); return doc.to<JsonArray>(); }
  return doc.as<JsonArray>();
}

uint8_t getNextAvailableID() {
  for (int i = lastID + 1; i <= MAX_FINGERPRINTS; i++) {
    if (finger.loadModel(i) != FINGERPRINT_OK) return i;
  }
  for (int i = 1; i <= lastID; i++) {
    if (finger.loadModel(i) != FINGERPRINT_OK) return i;
  }
  return 0;
}

/* ---------- Fingerprint Enrollment ---------- */
uint8_t enrollFingerprint(uint8_t id) {
  int p = -1;
  Serial.print("Enrollment start for ID "); Serial.println(id);
  Serial.println(">>> Place your finger (1st time)");
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) { delay(50); continue; }
    if (p == FINGERPRINT_OK) { Serial.println("Image taken (1st)"); finger.LEDcontrol(FINGERPRINT_LED_BLUE,0,0); }
    else { Serial.println("Failed to capture (1st)"); return p; }
  }
  if ((p=finger.image2Tz(1))!=FINGERPRINT_OK){ Serial.println("image2Tz(1) failed"); return p; }

  Serial.println("Remove finger...");
  delay(1000); while(finger.getImage()!=FINGERPRINT_NOFINGER){delay(50);}

  Serial.println(">>> Place SAME finger (2nd time)");
  p=-1; while(p!=FINGERPRINT_OK){
    p=finger.getImage();
    if(p==FINGERPRINT_NOFINGER){delay(50); continue;}
    if(p==FINGERPRINT_OK){Serial.println("Image taken (2nd)"); finger.LEDcontrol(FINGERPRINT_LED_BLUE,0,0);}
    else {Serial.println("Failed to capture (2nd)"); return p;}
  }
  if((p=finger.image2Tz(2))!=FINGERPRINT_OK){Serial.println("image2Tz(2) failed"); return p;}
  if((p=finger.createModel())!=FINGERPRINT_OK){Serial.println("createModel failed"); return p;}
  if((p=finger.storeModel(id))==FINGERPRINT_OK){
    Serial.println("Enrollment successful!");
    finger.LEDcontrol(FINGERPRINT_LED_BLUE,100,500);
    delay(400);
    finger.LEDcontrol(FINGERPRINT_LED_OFF,0,0);
    return true;
  }
  return p;
}

/* ---------- Token & Login ---------- */
String generateToken(){String t=""; for(int i=0;i<24;i++) t+=String(random(0,16),HEX); return t;}

bool checkToken(){
  if(!server.hasHeader("Authorization")) return false;
  String auth=server.header("Authorization");
  if(!auth.startsWith("Bearer ")) return false;
  String tok=auth.substring(7);
  return tok==currentToken && currentToken.length()>0;
}

/* ---------- API Handlers ---------- */
void handleLogin() {
  if(!server.hasArg("plain")) { 
    server.send(400,"application/json","{\"error\":\"Missing payload\"}"); return; 
  }
  DynamicJsonDocument d(256);
  deserializeJson(d, server.arg("plain"));
  String u = d["username"] | "";
  String p = d["password"] | "";

  if(u == adminUser && p == adminPass){
    currentToken = generateToken();
    DynamicJsonDocument res(256);
    res["token"] = currentToken;
    String out; serializeJson(res,out);
    server.send(200,"application/json",out);
  } else server.send(401,"application/json","{\"error\":\"Invalid credentials\"}");
}

void handleAdd() {
  if(!checkToken()){ server.send(401,"application/json","{\"error\":\"Unauthorized\"}"); return; }
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing payload\"}"); return; }

  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, body);

  if (error || !doc.containsKey("name")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON or missing name\"}"); return; }

  jsonDoc.clear();
  JsonArray arr = loadFingerprintData(jsonDoc);
  if (arr.size() >= MAX_FINGERPRINTS) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Max 300 fingerprints reached\"}"); return; }

  String name = doc["name"];
  uint8_t newID = getNextAvailableID();
  if (newID == 0) { server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"No empty slot available\"}"); return; }

  int res = enrollFingerprint(newID);
  if (res == true) {
    lastID = newID;
    JsonObject obj = arr.createNestedObject();
    obj["id"] = newID;
    obj["name"] = name;
    saveFingerprintData(arr);

    server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Finger added successfully\",\"id\":" + String(newID) + "}");
  } else {
    server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Enrollment failed\"}");
  }
}

void handleList() {
  if(!checkToken()){ server.send(401,"application/json","{\"error\":\"Unauthorized\"}"); return; }
  jsonDoc.clear();
  JsonArray arr = loadFingerprintData(jsonDoc);
  String output; serializeJson(arr, output);
  server.send(200, "application/json", output);
}

void handleDelete() {
  if(!checkToken()){ server.send(401,"application/json","{\"error\":\"Unauthorized\"}"); return; }
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing payload\"}"); return; }

  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, body);

  if (error || !doc.containsKey("id")) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON or missing id\"}"); return; }

  int idToDelete = doc["id"];
  jsonDoc.clear();
  JsonArray arr = loadFingerprintData(jsonDoc);
  int foundIndex = -1;
  for (int i = 0; i < (int)arr.size(); i++) if ((int)arr[i]["id"] == idToDelete) { foundIndex = i; break; }

  if (foundIndex == -1) { server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"ID not found\"}"); return; }

  finger.deleteModel(idToDelete);
  arr.remove(foundIndex);
  saveFingerprintData(arr);
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Deleted\",\"id\":" + String(idToDelete) + "}");
}

void handleChangePassword(){
  if(!checkToken()){ server.send(401,"application/json","{\"error\":\"Unauthorized\"}"); return; }
  if(!server.hasArg("plain")){ server.send(400,"application/json","{\"error\":\"Missing payload\"}"); return; }

  DynamicJsonDocument d(256);
  deserializeJson(d, server.arg("plain"));
  if(!d.containsKey("oldpassword") || !d.containsKey("newpassword")){ server.send(400,"application/json","{\"error\":\"Missing fields\"}"); return; }

  String oldPass = d["oldpassword"].as<String>();
  String newPass = d["newpassword"].as<String>();

  prefs.begin("bike", false);
  String storedPass = prefs.getString("adminPass", "admin");

  if(oldPass != storedPass){
    prefs.end();
    server.send(403,"application/json","{\"error\":\"Old password incorrect\"}");
    return; 
  }

  prefs.putString("adminPass", newPass);
  prefs.end();

  adminPass = newPass;
  server.send(200,"application/json","{\"status\":\"Password changed\"}");
}

void handleReset(){
  if(!checkToken()){ server.send(401,"application/json","{\"error\":\"Unauthorized\"}"); return; }

  finger.emptyDatabase();
  prefs.begin("bike", false);
  prefs.clear();
  prefs.putString("adminPass", "admin");
  prefs.end();

  jsonDoc.clear();
  JsonArray arr = jsonDoc.to<JsonArray>();
  saveFingerprintData(arr);

  adminPass = "admin";

  server.send(200,"application/json","{\"status\":\"Factory Reset Done\"}");
}

/* ---------- Fingerprint Check (Normal Mode) ---------- */
void checkFingerprint(){
  int p = finger.getImage();
  if(p == FINGERPRINT_NOFINGER){ 
    return;
  }
  if(p != FINGERPRINT_OK){ 
    Serial.println("Error reading finger"); 
    return; 
  }
  if((p = finger.image2Tz()) != FINGERPRINT_OK){ 
    Serial.println("Image2Tz error"); 
    return; 
  }
  if((p = finger.fingerSearch()) == FINGERPRINT_OK){
    Serial.print("✅ Fingerprint matched! ID: "); 
    Serial.println(finger.fingerID);
    finger.LEDcontrol(FINGERPRINT_LED_BLUE,0,0); 

    relayOn = true;
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("🔒 Relay ON - stays ON until power off");
  } 
  else { 
    Serial.println("❌ Fingerprint not found"); 
    finger.LEDcontrol(FINGERPRINT_LED_OFF,0,0);
    digitalWrite(RELAY_PIN, LOW);
  }
}

/* ---------- Button Handlers ---------- */
void handleButtons(){
  bool wifiCurrent = digitalRead(BUTTON_WIFI) == LOW;
  if(wifiCurrent && !wifiButtonState){ wifiPressStart=millis(); wifiButtonState=true; }
  if(!wifiCurrent && wifiButtonState){
    wifiButtonState=false;
    if(millis()-wifiPressStart >= LONG_PRESS_DURATION){
      hotspotMode=!hotspotMode;
      if(hotspotMode){ WiFi.softAP(apSSID,apPassword); Serial.println("🌐 Hotspot Enabled"); }
      else { WiFi.softAPdisconnect(true); Serial.println("❌ Hotspot Disabled"); }
    }
  }

  bool resetCurrent = digitalRead(BUTTON_RESET) == LOW;
  if(resetCurrent && !resetButtonState){ resetPressStart=millis(); resetButtonState=true; }
  if(!resetCurrent && resetButtonState){
    resetButtonState=false;
    if(millis()-resetPressStart >= LONG_PRESS_DURATION){
      Serial.println("⚠️ Factory Reset Triggered!");
      handleReset();
    }
  }
}

/* ---------- Setup ---------- */
void setup() {
  Serial.begin(9600);
  pinMode(RELAY_PIN,OUTPUT);
  pinMode(BUTTON_WIFI,INPUT_PULLUP);
  pinMode(BUTTON_RESET,INPUT_PULLUP);
  digitalWrite(RELAY_PIN,LOW);
  mySerial.begin(57600,SERIAL_8N1,16,17);
  
  if(!finger.verifyPassword()){Serial.println("❌ Sensor not found!"); while(1);}
  if(!LittleFS.begin(true)){Serial.println("LittleFS mount failed!"); while(1);}

  prefs.begin("bike", false);
  adminPass = prefs.getString("adminPass", "admin");
  prefs.end();

  WiFi.softAP(apSSID,apPassword); Serial.println("🌍 Hotspot started: SmartBikeAPI");

  server.on("/login", HTTP_POST, handleLogin);
  server.on("/add", HTTP_POST, handleAdd);
  server.on("/list", HTTP_GET, handleList);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/changepassword", HTTP_POST, handleChangePassword);
  server.on("/reset", HTTP_POST, handleReset);

  server.begin(); Serial.println("🌐 WebServer started");
}

/* ---------- Loop ---------- */
void loop() {
  server.handleClient();
  handleButtons();
  checkFingerprint();

  if(relayOn) digitalWrite(RELAY_PIN,HIGH);
}
