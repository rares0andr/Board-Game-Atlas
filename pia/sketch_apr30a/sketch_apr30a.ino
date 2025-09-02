#include <Arduino.h>            // Include biblioteca Arduino: oferă funcții de bază (setup, loop, Serial, etc.)
#include <WiFi.h>               // Include controlul modulului Wi-Fi pe ESP32
#include <HTTPClient.h>         // Include clientul HTTP pentru a face cereri GET/POST
#include <ArduinoJson.h>        // Include biblioteca ArduinoJson pentru serializare/deserializare JSON
#include <BLEDevice.h>          // Include inițializarea și operațiile de bază Bluetooth Low Energy
#include <BLEServer.h>          // Include crearea unui server BLE (GATT)
#include <BLEUtils.h>           // Include utilitare BLE, de ex. pentru lucrul cu UUID-uri
#include <BLE2902.h>            // Include descriptorul care permite notificări pe caracteristici BLE

// --- BLE & API setup ---
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"  
                                // UUID unic pentru serviciul BLE expus de ESP32
#define CHARACTERISTIC_UUID "abcdefab-cdef-cdef-cdef-abcdefabcdef"  
                                 // UUID unic pentru caracteristica BLE prin care comunicăm
const char* API_BASE = "http://proiectia.bogdanflorea.ro/api/board-game-atlas";  
                                 // URL de bază al API-ului web care listează board games

// --- Pending request struct ---
enum ActionType { NONE, GET_NET, CONNECT, GET_DATA, GET_DETAILS };  
                                 // Reprezintă tipurile de cereri primite de la aplicația mobilă
struct Request {
  ActionType action = NONE;      // Tipul cererii (inițial NONE)
  String param1;                 // Primul parametru: teamId, ssid sau id
  String param2;                 // Al doilea parametru: parola Wi-Fi (doar pentru CONNECT)
} pendingReq;                    // Instanță globală care reține cererea în așteptare

// --- Globals ---
BLECharacteristic* pCharacteristic;  
                                 // Pointer la caracteristica BLE folosită pentru read/write/notify
String currentTeam;              // Reține teamId-ul curent pentru toate răspunsurile

// --- Helper: serialize & notify ---
void sendNotify(JsonDocument& doc) {
  String payload;                // Buffer pentru JSON-ul serializat
  serializeJson(doc, payload);   // Transformă `doc` într-un șir JSON și îl pune în `payload`
  pCharacteristic->setValue(payload.c_str());  
                                 // Încarcă textul JSON în caracteristica BLE
  pCharacteristic->notify();     // Trimite o notificare (push) către clientul mobil
  delay(20);                     // Mic delay pentru a asigura trimiterea completă
}

// --- Queue a request from BLE callback ---
void queueRequest(ActionType a, const String& p1 = "", const String& p2 = "") {
  pendingReq.action = a;         // Salvează tipul cererii
  pendingReq.param1 = p1;        // Salvează primul parametru
  pendingReq.param2 = p2;        // Salvează al doilea parametru
}

// --- Handlers executed in loop() ---
void doGetNetworks() {
  currentTeam = pendingReq.param1;  // Preia teamId din cererea pusă în coadă
  int n = WiFi.scanNetworks();      // Scanează rețele Wi-Fi disponibile
  for (int i = 0; i < n; ++i) {      // Pentru fiecare rețea găsită:
    StaticJsonDocument<256> doc;     // Creează un document JSON mic
    doc["ssid"]       = WiFi.SSID(i);              // Numele rețelei
    doc["strength"]   = WiFi.RSSI(i);              // Puterea semnalului
    doc["encryption"] = (int)WiFi.encryptionType(i);// Tipul de criptare
    doc["teamId"]     = currentTeam;               // Echipa care a cerut
    sendNotify(doc);                                // Trimite JSON-ul prin BLE
  }
}

void doConnect() {
  const char* ssid = pendingReq.param1.c_str();    // SSID-ul din param1
  const char* pass = pendingReq.param2.c_str();    // Parola din param2
  WiFi.begin(ssid, pass);                          // Începe conectarea la rețeaua Wi-Fi
  unsigned long start = millis();                  // Reține momentul de start
  bool ok = false;                                 // Flag pentru succes
  while (millis() - start < 10000) {               // Așteaptă până la 10 secunde
    if (WiFi.status() == WL_CONNECTED) { ok = true; break; }  
    delay(100);                                    // Pauză mică între verificări
  }
  StaticJsonDocument<256> doc;                     // Document JSON de răspuns
  doc["ssid"]      = String(ssid);                 // SSID
  doc["connected"] = ok;                           // True dacă s-a conectat
  doc["teamId"]    = currentTeam;                  // Echipa curentă
  sendNotify(doc);                                 // Trimite răspunsul prin BLE
}

void doGetData() {
  if (WiFi.status() != WL_CONNECTED) return;      // Dacă nu suntem conectați, ieșim imediat
  HTTPClient http;                                // Creăm un client HTTP
  http.begin(String(API_BASE) + "/games");        // Deschidem conexiunea la endpoint-ul /games
  if (http.GET() == 200) {                        // Dacă serverul răspunde cu 200 OK
    String pl = http.getString();                 // Citim tot răspunsul JSON ca text
    DynamicJsonDocument arr(20*1024);             // Document JSON destul de mare pentru array
    if (!deserializeJson(arr, pl)) {              // Deserializeaza textul în structuri C++
      for (JsonObject item : arr.as<JsonArray>()) {  // Pentru fiecare obiect din array
        StaticJsonDocument<512> doc;              // Document JSON pentru trimitere
        doc["id"]     = item["id"].as<const char*>();        // ID-ul jocului
        doc["name"]   = item["name"].as<const char*>();      // Numele jocului
        doc["image"]  = item["image_url"].as<const char*>(); // URL-ul imaginii
        doc["teamId"] = currentTeam;                         // Echipa curentă
        sendNotify(doc);                                    // Trimite JSON-ul
      }
    }
  }
  http.end();                                     // Închide conexiunea HTTP
}

void doGetDetails() {
  if (WiFi.status() != WL_CONNECTED) return;      // Ieșim dacă nu avem Wi-Fi
  String id = pendingReq.param1;                  // ID-ul jocului din coadă
  HTTPClient http;                                // Client HTTP nou
  http.begin(String(API_BASE) + "/game?id=" + id);// Cerere GET cu parametru id
  if (http.GET() == 200) {                        // Dacă răspunsul e OK
    String pl = http.getString();                 // Citim textul JSON
    DynamicJsonDocument in(20*1024);              // Document pentru deserializare
    if (!deserializeJson(in, pl)) {               // Deserializam JSON-ul
      JsonObject src = in.as<JsonObject>();       // Obiect JSON sursă
      StaticJsonDocument<1024> doc;               // Document JSON pentru răspuns
      doc["id"]    = src["id"].as<const char*>();       // ID
      doc["name"]  = src["name"].as<const char*>();     // Nume
      doc["image"] = src["image_url"].as<const char*>();// Imagine
      // Construim linie cu preț, jucători, timp și an
      String price    = String("$") + src["price"].as<const char*>();
      String players  = String(src["players"].as<const char*>());
      String playtime = String(src["playtime"].as<const char*>());
      String year     = String(src["year_published"].as<int>());
      String desc = "Price: "    + price   + "\n"
                  + "Players: "  + players + "\n"
                  + "Play time: "+ playtime+ "\n"
                  + "Year: "     + year;
      doc["description"] = desc;                 // Atribuim descrierea
      doc["teamId"]      = currentTeam;          // Echipa curentă
      sendNotify(doc);                           // Trimitem JSON-ul final
    }
  }
  http.end();                                    // Închide conexiunea HTTP
}

// --- BLE callback: queue the request, return immediately ---
class CB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String s = c->getValue();                    // Textul JSON trimis de telefon
    StaticJsonDocument<1024> r;                  // Document pentru parsare
    if (!deserializeJson(r, s)) {                // Deserializam textul JSON
      String a = r["action"].as<String>();       // Extragem câmpul "action"
      // Alegem handler-ul potrivit în funcție de valoarea acțiunii
      if      (a == "getNetworks") queueRequest(GET_NET,    r["teamId"].as<String>());
      else if (a == "connect")     queueRequest(CONNECT,    r["ssid"].as<String>(), r["password"].as<String>());
      else if (a == "getData")     queueRequest(GET_DATA);
      else if (a == "getDetails")  queueRequest(GET_DETAILS, r["id"].as<String>());
    }
  }
};

// --- Setup & loop ---
void setup() {
  Serial.begin(115200);                         // Începem comunicarea prin portul serial
  WiFi.mode(WIFI_STA);                          // Setăm ESP32 ca stație Wi-Fi

  BLEDevice::init("ESP32-BoardGameAPI");        // Inițializăm BLE cu numele device-ului
  BLEServer*  srv = BLEDevice::createServer();  // Creăm un server BLE
  BLEService* svc = srv->createService(SERVICE_UUID); // Adăugăm serviciul GATT cu UUID-ul definit

  // Cream caracteristica cu permisiuni de citire, scriere și notificare
  pCharacteristic = svc->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ  |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902()); // Adăugăm descriptorul necesar pentru notify
  pCharacteristic->setCallbacks(new CB());       // Setăm callback-ul pentru scriere

  svc->start();                                  // Pornim serviciul BLE
  BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID); // Facem advertising
  BLEDevice::getAdvertising()->start();          // Începem advertising-ul

  Serial.println("BLE ready");                   // Afișăm mesaj de stare
}

void loop() {
  // Verificăm ce cerere e în coadă și apelăm handler-ul respectiv
  switch(pendingReq.action) {
    case GET_NET:     doGetNetworks();   break;
    case CONNECT:     doConnect();       break;
    case GET_DATA:    doGetData();       break;
    case GET_DETAILS: doGetDetails();    break;
    default: break;                       // Dacă nu e nicio cerere, nu facem nimic
  }
  pendingReq.action = NONE;                 // Resetăm coada după procesare
  delay(50);                                // Mic delay pentru stabilitate
}
