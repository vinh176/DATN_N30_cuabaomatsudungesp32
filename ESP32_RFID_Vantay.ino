#define BLYNK_TEMPLATE_ID "TMPL6s6cM6q7f"
#define BLYNK_TEMPLATE_NAME "Z Lab"
#define BLYNK_AUTH_TOKEN "xDkoVnT2TyDyVmzzMzUjlCOrjMnvOZuO"

#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <EEPROM.h>
#include <Keypad.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <time.h>

char auth[] = "xDkoVnT2TyDyVmzzMzUjlCOrjMnvOZuO";
char ssid[] = "ZET Third Floor2";
char pass[] = "ZET6868@";

#define RX_PIN 16
#define TX_PIN 17
#define BUZZER_PIN 4
#define LOCK_PIN 2
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger(&mySerial);

#define SS_PIN 5
#define RST_PIN 13
MFRC522 rfid(SS_PIN, RST_PIN);

LiquidCrystal_I2C lcd(0x27, 16, 2);
WidgetTerminal terminal(V0);

uint8_t fingerID = 1;
String knownCards[100];
int cardCount = 0;

// Flags
bool addCardMode = false;
bool deleteCardMode = false;
bool changePasswordMode = false;
bool systemLocked = false;
bool isAddingFingerprint = false;

bool isOnline = false;
bool expectingPinEntry = false;
bool offlineAdminModeActive = false;
byte offlineAdminMenuPage = 0;

// New flags and constants for Blynk card operations
bool blynkCardOpActive = false;         // True if a card add/delete was initiated by Blynk and is awaiting swipe
unsigned long blynkCardOpStartTime = 0; // Timestamp for Blynk card operation start
const unsigned long BLYNK_CARD_OP_TIMEOUT = 15000; // 15 seconds for Blynk card operation

enum FingerprintMode { FINGER_IDLE, FINGER_CHECK, FINGER_ADD, FINGER_DELETE };
FingerprintMode fingerprintMode = FINGER_CHECK;

#define EEPROM_SIZE 512
#define PASSWORD_ADDR 400
String defaultPassword = "8888";
const String MASTER_OFFLINE_ADMIN_PIN = "2003";

String inputPassword = "";
String displayPassword = "";
unsigned long lastKeyTime = 0;
const unsigned long PASSWORD_TIMEOUT = 15000; // Timeout for keypad input modes

const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};
byte rowPins[ROWS] = {12, 14, 27, 26};
byte colPins[COLS] = {25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

String pendingCardUID = ""; // Used by Blynk V2 (delete by UID)
String pendingFingerID = ""; // Used by Blynk V9 (delete by ID)
int failedAttempts = 0;
unsigned long lastErrorTime = 0;
const unsigned long ERROR_COOLDOWN = 5000;
unsigned long lastV6TriggerTime = 0;
const unsigned long V6_DEBOUNCE = 10000; // Debounce for V6 button
int lastV6Value = 0;
bool v6Triggered = false;

QueueHandle_t keypadQueue;
QueueHandle_t rfidQueue;
TaskHandle_t fingerprintTaskHandle = NULL;
unsigned long deleteModeStartTime = 0; // For fingerprint delete by scan
const unsigned long DELETE_TIMEOUT = 10000; // For fingerprint delete by scan

// Forward declarations
void showMainMenu();
void addFingerprint_internal();
int enrollFingerprint(int id);
void showOfflineAdminMenuPage();
void handleOfflineAdminAction(byte page, char choice);
void checkBlynkCardOpTimeout(); // New function

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) {
    return "TIME N/A";
  }
  char timeString[20];
  strftime(timeString, sizeof(timeString), "%y-%m-%d %H:%M", &timeinfo);
  return String(timeString);
}

void showCentered(String msg1, String msg2) {
  lcd.clear();
  msg1.toUpperCase();
  msg2.toUpperCase();
  int len1 = msg1.length();
  int pad1 = (16 - len1) / 2;
  lcd.setCursor(max(0, pad1), 0);
  lcd.print(msg1.substring(0, min(len1, 16)));
  int len2 = msg2.length();
  int pad2 = (16 - len2) / 2;
  lcd.setCursor(max(0, pad2), 1);
  lcd.print(msg2.substring(0, min(len2, 16)));
}

void beepSuccess() {
  tone(BUZZER_PIN, 1000, 100); delay(120); noTone(BUZZER_PIN);
}
void beepFailure() {
  tone(BUZZER_PIN, 300, 200); delay(220); noTone(BUZZER_PIN);
}
void unlockDoor() {
  digitalWrite(LOCK_PIN, HIGH);
  delay(3000);
  digitalWrite(LOCK_PIN, LOW);
}

void setup() {
  Serial.begin(115200);
  mySerial.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);
  finger.begin(57600);
  EEPROM.begin(EEPROM_SIZE);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LOCK_PIN, OUTPUT);
  digitalWrite(LOCK_PIN, LOW);
  for (byte i = 0; i < ROWS; i++) pinMode(rowPins[i], INPUT_PULLUP);

  delay(500);
  lcd.init();
  lcd.backlight();

  showCentered("CONNECTING...", "PLEASE WAIT...");
  Serial.println("Attempting WiFi connection...");
  WiFi.begin(ssid, pass);
  unsigned long wifiConnectStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiConnectStart < 15000)) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!"); Serial.print("IP Address: "); Serial.println(WiFi.localIP());
    Blynk.config(auth);
    unsigned long blynkConnectStart = millis();
    bool blynkConnected = false;
    while (!blynkConnected && (millis() - blynkConnectStart < 10000)) {
      blynkConnected = Blynk.connect(1000); if (blynkConnected) break; delay(500);
    }
    if (blynkConnected) {
      isOnline = true; Serial.println("Blynk Connected!");
      terminal.println(getFormattedTime() + " >> SYSTEM :: ONLINE - BLYNK CONNECTED"); terminal.flush();
      configTime(7 * 3600, 0, "pool.ntp.org");
    } else {
      isOnline = false; Serial.println("Blynk Connection Failed!");
      showCentered("BLYNK FAILED", "OFFLINE MODE"); delay(2000);
    }
  } else {
    isOnline = false; Serial.println("\nWiFi Connection Failed!");
    showCentered("WIFI FAILED", "OFFLINE MODE"); delay(2000);
  }
   if (!isOnline) { Serial.println(getFormattedTime() + " >> SYSTEM :: OFFLINE MODE"); }


  SPI.begin();
  rfid.PCD_Init();
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print(F("MFRC522 Version: 0x")); Serial.println(version, HEX);
  if (version == 0x00 || version == 0xFF) {
    Serial.println(F("WARNING: MFRC522 Communication failure. Check wiring."));
    showCentered("RFID ERROR", "CHECK WIRING");
    if(isOnline) { terminal.println(getFormattedTime() + " >> RFID :: INIT FAIL - CHECK WIRING"); terminal.flush(); }
  } else {
    Serial.println(F("RFID Reader Initialized."));
    if(isOnline) { terminal.println(getFormattedTime() + " >> RFID :: INIT SUCCESS"); terminal.flush(); }
  }

  int sensorRetries = 3; bool sensorReady = false;
  while (sensorRetries > 0 && !sensorReady) {
    if (finger.verifyPassword()) {
      beepSuccess(); sensorReady = true; Serial.println("Fingerprint Sensor Initialized");
      if(isOnline) { terminal.println(getFormattedTime() + " >> FINGERPRINT :: INIT SUCCESS"); terminal.flush(); }
    } else {
      Serial.println("Fingerprint Sensor Error. Retries Left: " + String(sensorRetries));
      sensorRetries--; delay(1000);
    }
  }
  if (!sensorReady) {
    showCentered("AS608 FAILED", "CHECK SENSOR");
    String errMsg = getFormattedTime() + " >> FINGERPRINT :: INIT FAIL - SENSOR ERROR";
    if (isOnline) { terminal.println(errMsg); terminal.flush(); } else { Serial.println(errMsg + " (OFFLINE)"); }
    while (1) { delay(1); } // Halt system
  }

  loadCardsFromEEPROM();
  loadPasswordFromEEPROM();

  keypadQueue = xQueueCreate(10, sizeof(char));
  rfidQueue = xQueueCreate(10, sizeof(String*));
  xTaskCreate(keypadTask, "KeypadTask", 2048, NULL, 6, NULL);
  xTaskCreate(rfidTask, "RFIDTask", 2560, NULL, 5, NULL);
  xTaskCreate(continuousFingerprintTask, "FingerprintTask", 4096, NULL, 5, &fingerprintTaskHandle);
  
  Serial.println("System setup complete. Tasks running.");
  delay(1000);
  showMainMenu();
}

void keypadTask(void *parameter) {
  for (;;) {
    char key = keypad.getKey();
    if (key) {
      xQueueSend(keypadQueue, &key, portMAX_DELAY);
    }
    vTaskDelay(20 / portTICK_PERIOD_MS); 
  }
}
void rfidTask(void *parameter) {
  for (;;) {
    if (rfid.PICC_IsNewCardPresent()) {
      if (rfid.PICC_ReadCardSerial()) {
        String uid = "";
        for (byte i = 0; i < rfid.uid.size; i++) {
          uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : ""); 
          uid += String(rfid.uid.uidByte[i], HEX);
        }
        uid.toUpperCase(); 
        rfid.PICC_HaltA();       
        rfid.PCD_StopCrypto1();   
        String* uidPtr = new String(uid); 
        if (uidPtr) {
            if (xQueueSend(rfidQueue, &uidPtr, pdMS_TO_TICKS(100)) != pdPASS) { 
                delete uidPtr; 
                Serial.println("RFID Task: Failed to send to queue");
            }
        } else {
            Serial.println("RFID Task: Failed to allocate memory for UID");
        }
      } 
    }
    vTaskDelay(50 / portTICK_PERIOD_MS); 
  }
}
void continuousFingerprintTask(void *parameter) {
  for (;;) {
    if (systemLocked || fingerprintMode == FINGER_ADD || isAddingFingerprint || blynkCardOpActive) { // Also pause if Blynk card op active
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    if (fingerprintMode == FINGER_DELETE && (millis() - deleteModeStartTime > DELETE_TIMEOUT)) {
      Serial.println("FingerprintTask: Delete mode timed out");
      String msg = getFormattedTime() + " >> FINGERPRINT :: DELETE MODE TIMEOUT (SCAN)";
      if (isOnline) { terminal.println(msg); terminal.flush(); }
      else { Serial.println(msg + " (OFFLINE)"); }
      fingerprintMode = FINGER_CHECK; // Reset mode
      showMainMenu(); 
    }

    if (fingerprintMode == FINGER_CHECK || fingerprintMode == FINGER_DELETE) {
      int p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        if (fingerprintMode == FINGER_CHECK) processFingerprint();
        else if (fingerprintMode == FINGER_DELETE) deleteFingerprintByScan();
      } else if (p != FINGERPRINT_NOFINGER) {
        if (millis() - lastErrorTime > ERROR_COOLDOWN) {
           Serial.println("FingerprintTask: getImage failed with code " + String(p));
           String msg = getFormattedTime() + " >> FINGERPRINT :: GET_IMAGE ERROR - Code: " + String(p);
           if(isOnline) { terminal.println(msg); terminal.flush(); } else { Serial.println(msg + " (OFFLINE)"); }
           lastErrorTime = millis();
        }
        vTaskDelay(100 / portTICK_PERIOD_MS); // Short delay on error before retry
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS); 
  }
}

void loop() {
  if (systemLocked) return;
  if (isOnline) Blynk.run();
  
  checkKeypad();
  checkRFID();
  checkBlynkCardOpTimeout(); // Check for Blynk card operation timeouts
}

// --- BLYNK_WRITE Functions ---
BLYNK_WRITE(V6) { // Add Fingerprint (Online)
  if (!isOnline) { Serial.println("V6: Ignored, offline."); return; }
  int value = param.asInt();
  if (value == lastV6Value && v6Triggered) { return; } // Prevent re-trigger
  if (millis() - lastV6TriggerTime < V6_DEBOUNCE && value != 0) { return; } // Debounce

  if (value == 1 && !expectingPinEntry && !offlineAdminModeActive && !isAddingFingerprint && fingerprintMode != FINGER_ADD && !blynkCardOpActive) {
    v6Triggered = true;
    lastV6Value = value;
    isAddingFingerprint = true; // Set flag
    fingerprintMode = FINGER_ADD; // Set mode
    lastV6TriggerTime = millis();
    Serial.println("V6 Triggered: Starting Fingerprint Addition for ID " + String(fingerID));
    showCentered("ADD FINGER ID " + String(fingerID), "PLACE FINGER");
    if (isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V6 :: ADD FINGERPRINT ID " + String(fingerID) + " - PLACE FINGER"); terminal.flush();}
    
    if(fingerprintTaskHandle != NULL) vTaskSuspend(fingerprintTaskHandle);
    addFingerprint_internal(); 
    if(fingerprintTaskHandle != NULL) vTaskResume(fingerprintTaskHandle);
    
    isAddingFingerprint = false; // Reset flag
    fingerprintMode = FINGER_CHECK; // Reset mode
    showMainMenu();
  } else if (value == 0) {
    v6Triggered = false; lastV6Value = 0;
  } else {
    Serial.println("V6 Ignored: System busy/conditions not met.");
    if (isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V6 :: IGNORED - SYSTEM BUSY"); terminal.flush();}
  }
}

BLYNK_WRITE(V2) { // Delete Card by UID (Online)
  if (!isOnline) { Serial.println("V2: Ignored, offline."); return; }
  if (!expectingPinEntry && !offlineAdminModeActive && !blynkCardOpActive) { 
    pendingCardUID = param.asStr();
    pendingCardUID.toUpperCase();
    if (pendingCardUID != "") {
      Serial.println("V2 Triggered: Deleting Card UID " + pendingCardUID);
      if (deleteCardFromEEPROM(pendingCardUID)) {
        if(isOnline) terminal.println(getFormattedTime() + " >> BLYNK V2 :: CARD DELETE SUCCESS - UID: " + pendingCardUID);
        showCentered("CARD DELETED", "SUCCESS"); beepSuccess();
      } else {
        if(isOnline) terminal.println(getFormattedTime() + " >> BLYNK V2 :: CARD DELETE FAIL - NOT FOUND: " + pendingCardUID);
        showCentered("CARD NOT FOUND", "CHECK UID"); beepFailure();
      }
      if (isOnline) terminal.flush();
      pendingCardUID = ""; delay(2000); showMainMenu();
    }
  } else {
    Serial.println("V2 Ignored: System busy");
    if(isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V2 :: IGNORED - SYSTEM BUSY"); terminal.flush();}
  }
}

BLYNK_WRITE(V9) { // Delete Fingerprint by ID (Online)
  if (!isOnline) { Serial.println("V9: Ignored, offline."); return; }
  if (!expectingPinEntry && !offlineAdminModeActive && !blynkCardOpActive) {
    pendingFingerID = param.asStr();
    if (pendingFingerID != "") {
      Serial.println("V9 Triggered: Deleting Fingerprint ID " + pendingFingerID);
      int id = pendingFingerID.toInt();
      // deleteFingerprint() will log to terminal and handle LCD
      deleteFingerprint(id); 
      pendingFingerID = "";
      // deleteFingerprint calls showMainMenu()
    }
  } else {
    Serial.println("V9 Ignored: System busy");
    if(isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V9 :: IGNORED - SYSTEM BUSY"); terminal.flush();}
  }
}

BLYNK_WRITE(V5) { // Clear EEPROM (Online)
  if (!isOnline) { Serial.println("V5: Ignored, offline."); return; }
  if (!expectingPinEntry && !offlineAdminModeActive && !blynkCardOpActive) {
    Serial.println("V5 Triggered: Clearing EEPROM");
    clearEEPROM(); 
    if (isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V5 :: ALL DATA CLEARED - SYSTEM RESET"); terminal.flush();}
    showCentered("ALL DATA CLEARED", "SYSTEM RESET"); beepSuccess();
    delay(2000); showMainMenu();
  } else {
    Serial.println("V5 Ignored: System busy");
    if(isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V5 :: IGNORED - SYSTEM BUSY"); terminal.flush();}
  }
}

BLYNK_WRITE(V3) { // Add Card Mode (Online - swipe)
    if (!isOnline) { Serial.println("V3: Ignored, offline."); return; }
    // Check if any other major mode is active or if already in a Blynk card operation
    if (!expectingPinEntry && !offlineAdminModeActive && !addCardMode && !blynkCardOpActive && !deleteCardMode && !isAddingFingerprint && fingerprintMode == FINGER_CHECK) {
        addCardMode = true; 
        deleteCardMode = false; // Ensure delete mode is off
        blynkCardOpActive = true;    // Set the Blynk operation flag
        blynkCardOpStartTime = millis(); // Start the timer for this operation
        
        Serial.println("V3 Triggered: Add Card Mode (Swipe) - Waiting for card...");
        showCentered("ADD CARD (BLY)", "SWIPE CARD"); // Indicate Blynk operation
        if (isOnline) {
            terminal.println(getFormattedTime() + " >> BLYNK V3 :: ADD CARD MODE - SWIPE CARD WITHIN " + String(BLYNK_CARD_OP_TIMEOUT/1000) + "s");
            terminal.flush();
        }
        beepSuccess();
    } else {
        Serial.println("V3 Ignored: System busy or already in a conflicting mode.");
        if (isOnline) {
            terminal.println(getFormattedTime() + " >> BLYNK V3 :: IGNORED - SYSTEM BUSY/MODE ACTIVE");
            terminal.flush();
        }
    }
}

BLYNK_WRITE(V4) { // Delete Card by Swipe (Online)
    if (!isOnline) { Serial.println("V4: Ignored, offline."); return; }
    if (!expectingPinEntry && !offlineAdminModeActive && !deleteCardMode && !blynkCardOpActive && !addCardMode && !isAddingFingerprint && fingerprintMode == FINGER_CHECK) {
        deleteCardMode = true; 
        addCardMode = false; // Ensure add mode is off
        blynkCardOpActive = true;    // Set the Blynk operation flag
        blynkCardOpStartTime = millis(); // Start the timer

        Serial.println("V4 Triggered: Delete Card Mode (Swipe) - Waiting for card...");
        showCentered("DELETE CARD (BLY)", "SWIPE CARD"); // Indicate Blynk operation
        if (isOnline) {
            terminal.println(getFormattedTime() + " >> BLYNK V4 :: DELETE CARD MODE - SWIPE CARD WITHIN " + String(BLYNK_CARD_OP_TIMEOUT/1000) + "s");
            terminal.flush();
        }
        beepSuccess();
    } else {
        Serial.println("V4 Ignored: System busy or already in a conflicting mode.");
        if (isOnline) {
            terminal.println(getFormattedTime() + " >> BLYNK V4 :: IGNORED - SYSTEM BUSY/MODE ACTIVE");
            terminal.flush();
        }
    }
}

BLYNK_WRITE(V1) { // List RFID Cards (Online)
  if (!isOnline) { Serial.println("V1: Ignored, offline."); return; }
  if (!expectingPinEntry && !offlineAdminModeActive && !blynkCardOpActive) {
    Serial.println("V1 Triggered: Listing RFID Cards");
    if(isOnline) terminal.println(getFormattedTime() + " >> BLYNK V1 :: LISTING RFID CARDS...");
    listRFIDCards(); 
    if(isOnline) terminal.println(getFormattedTime() + " >> BLYNK V1 :: CARD LISTING COMPLETE"); terminal.flush();
  } else {
    Serial.println("V1 Ignored: System busy");
    if(isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V1 :: IGNORED - SYSTEM BUSY"); terminal.flush();}
  }
}

BLYNK_WRITE(V10) { // Change Password from Blynk
  if (!isOnline) { Serial.println("V10: Ignored, offline."); return; }
  if (!expectingPinEntry && !offlineAdminModeActive && !blynkCardOpActive) {
    String newPassword = param.asStr();
    Serial.println("V10 Triggered: Changing Password to " + newPassword);
    if (newPassword.length() >= 4 && newPassword.length() <= 8) {
      defaultPassword = newPassword;
      savePasswordToEEPROM(); // This function logs to Serial
      if (isOnline) terminal.println(getFormattedTime() + " >> BLYNK V10 :: USER PASSWORD CHANGED");
      showCentered("PASSWORD CHANGED", "SUCCESS (BLY)"); beepSuccess();
    } else {
      if (isOnline) terminal.println(getFormattedTime() + " >> BLYNK V10 :: INVALID NEW PASSWORD - MUST BE 4-8 CHARS");
      showCentered("INVALID PASSWORD", "4-8 CHARS (BLY)"); beepFailure();
    }
    if (isOnline) terminal.flush(); delay(2000); showMainMenu();
  } else {
    Serial.println("V10 Ignored: System busy");
    if(isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V10 :: IGNORED - SYSTEM BUSY"); terminal.flush();}
  }
}

BLYNK_WRITE(V8) { // Remote Door Unlock (Online)
  if (!isOnline) { Serial.println("V8: Ignored, offline."); return; }
  if (!expectingPinEntry && !offlineAdminModeActive && !blynkCardOpActive) {
    int value = param.asInt();
    if (value == 1) {
      Serial.println("V8 Triggered: Remote Door Unlock");
      if (isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V8 :: REMOTE UNLOCK - DOOR OPENED"); terminal.flush();}
      showCentered("REMOTE UNLOCK", "DOOR OPENED");
      unlockDoor(); beepSuccess(); delay(2000); showMainMenu();
    }
  } else {
    Serial.println("V8 Ignored: System busy");
    if(isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V8 :: IGNORED - SYSTEM BUSY"); terminal.flush();}
  }
}

BLYNK_WRITE(V7) { // Fingerprint Delete by Scan (Online)
  if (!isOnline) { Serial.println("V7: Ignored, offline."); return; }
  if (!expectingPinEntry && !offlineAdminModeActive && !blynkCardOpActive && !isAddingFingerprint) {
    int value = param.asInt();
    if (value == 1 && fingerprintMode != FINGER_DELETE) { 
      fingerprintMode = FINGER_DELETE;
      deleteModeStartTime = millis(); 
      Serial.println("V7 Triggered: Fingerprint Delete Mode Enabled (Scan)");
      showCentered("DELETE FINGER", "PLACE FINGER (BLY)");
      if (isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V7 :: FINGERPRINT DELETE MODE (SCAN) - PLACE FINGER"); terminal.flush();}
      beepSuccess();
    } else if (value == 0 && fingerprintMode == FINGER_DELETE) { 
      fingerprintMode = FINGER_CHECK; // Cancel mode if button is turned off
      if (isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V7 :: FINGERPRINT DELETE MODE (SCAN) DEACTIVATED"); terminal.flush();}
      showMainMenu();
      Serial.println("V7: Fingerprint Delete Mode Deactivated by Blynk");
    }
  } else {
    Serial.println("V7 Ignored: System busy or already in mode");
    if(isOnline) {terminal.println(getFormattedTime() + " >> BLYNK V7 :: IGNORED - SYSTEM BUSY/MODE ACTIVE"); terminal.flush();}
  }
}
// --- END BLYNK_WRITE Functions ---

void showMainMenu() {
  changePasswordMode = false;
  isAddingFingerprint = false; 

  // Only reset card modes if not actively waiting for a Blynk card operation
  // This is generally safe as blynkCardOpActive should be false by now if coming from a completed/timed-out blynk op
  if (!blynkCardOpActive) {
    addCardMode = false;
    deleteCardMode = false;
  }

  // Reset fingerprint delete mode if it's not the one initiated by Blynk (V7) and has timed out, or if simply returning to main menu.
  if (fingerprintMode == FINGER_DELETE) {
      // If it was a Blynk V7 initiated delete by scan, it will timeout via continuousFingerprintTask or be cancelled by V7=0.
      // If it was an Admin offline delete by scan, continuousFingerprintTask also handles timeout.
      // If we are here for other reasons, ensure it's reset if not actively in timed period.
      if (millis() - deleteModeStartTime > DELETE_TIMEOUT) {
          fingerprintMode = FINGER_CHECK;
      }
  } else {
      fingerprintMode = FINGER_CHECK; 
  }
  
  expectingPinEntry = false; 
  offlineAdminModeActive = false; 
  offlineAdminMenuPage = 0;   
  inputPassword = "";
  displayPassword = "";

  if (isOnline) {
    showCentered("SMART LOCKDOOR", "SCAN OR USE KEY");
  } else {
    showCentered("OFFLINE MODE", "SCAN OR USE KEY"); 
  }
  Serial.println("Displayed Main Menu. Mode: " + String(isOnline ? "Online" : "Offline") +
                 ", FP Mode: " + String(fingerprintMode) + 
                 ", AddCard: " + String(addCardMode) + 
                 ", DelCard: " + String(deleteCardMode) +
                 ", BlynkCardOp: " + String(blynkCardOpActive)
                 );
}

void lockoutSystem() {
  systemLocked = true;
  showCentered("SYSTEM LOCKED", "WAIT 30 SEC");
  String logMsg = getFormattedTime() + " >> SYSTEM :: LOCKED - TOO MANY FAILED ATTEMPTS";
  if (isOnline) { terminal.println(logMsg); terminal.flush(); } else { Serial.println(logMsg + " (OFFLINE)"); }
  
  unsigned long lockoutEndTime = millis() + 30000; 
  while (millis() < lockoutEndTime) {
    tone(BUZZER_PIN, 500, 200); delay(800); noTone(BUZZER_PIN); // Visual/Audible indication
    if (isOnline) Blynk.run(); // Keep Blynk connection alive if possible
  }
  systemLocked = false; failedAttempts = 0; 
  if (isOnline) { terminal.println(getFormattedTime() + " >> SYSTEM :: UNLOCKED - LOCKOUT PERIOD ENDED"); terminal.flush(); }
  else { Serial.println(getFormattedTime() + " >> SYSTEM :: UNLOCKED (OFFLINE)");}
  showMainMenu(); 
}

void showOfflineAdminMenuPage() {
  offlineAdminModeActive = true;
  inputPassword = ""; 
  displayPassword = "";
  lastKeyTime = millis(); // Reset timeout for menu navigation

  Serial.print("Showing Offline Admin Menu Page: "); Serial.println(offlineAdminMenuPage);
  if (isOnline) { terminal.println(getFormattedTime() + " >> ADMIN :: MENU PAGE " + String(offlineAdminMenuPage)); terminal.flush(); }

  switch (offlineAdminMenuPage) {
    case 1: showCentered("1.ADD FINGER", "2.DEL FINGER(#>)"); break;
    case 2: showCentered("1.ADD RFID", "2.DEL RFID (#>)"); break;
    case 3: showCentered("1.CHG USER PASS", "2.RESET PASS (*X)"); break;
    default:
      offlineAdminModeActive = false;
      offlineAdminMenuPage = 0;
      showMainMenu();
      break;
  }
}

void handleOfflineAdminAction(byte page, char choice) {
  Serial.print("Admin Action: Page "); Serial.print(page); Serial.print(", Choice "); Serial.println(choice);
  lastKeyTime = millis(); // Reset timeout
  String adminActionLog;

  switch (page) {
    case 1: // Fingerprint Page
      if (choice == '1') { // Add Finger
        adminActionLog = " >> ADMIN :: ADD FINGERPRINT SELECTED";
        Serial.println("Admin: Add Fingerprint selected.");
        if(fingerprintTaskHandle != NULL) vTaskSuspend(fingerprintTaskHandle);
        isAddingFingerprint = true;   
        fingerprintMode = FINGER_ADD;  
        showCentered("ADD FINGER ID " + String(fingerID), "PLACE FINGER");
        if(isOnline) { terminal.println(getFormattedTime() + adminActionLog + " - ID " + String(fingerID)); terminal.flush(); }
        else { Serial.println(adminActionLog + " - ID " + String(fingerID) + " (OFFLINE)");}
        addFingerprint_internal(); 
        isAddingFingerprint = false;
        fingerprintMode = FINGER_CHECK; 
        if(fingerprintTaskHandle != NULL) vTaskResume(fingerprintTaskHandle);
        showOfflineAdminMenuPage(); 
      } else if (choice == '2') { // Delete Finger by scan
        adminActionLog = " >> ADMIN :: DELETE FINGERPRINT BY SCAN SELECTED";
        Serial.println("Admin: Delete Fingerprint by scan selected.");
        fingerprintMode = FINGER_DELETE;
        deleteModeStartTime = millis(); 
        showCentered("DELETE FINGER", "PLACE FINGER");
        if(isOnline) { terminal.println(getFormattedTime() + adminActionLog); terminal.flush(); }
        else { Serial.println(adminActionLog + " (OFFLINE)");}
        // continuousFingerprintTask handles the rest and calls showMainMenu
      } else { beepFailure(); showOfflineAdminMenuPage(); }
      break;

    case 2: // RFID Page
      if (choice == '1') { // Add RFID Card
        adminActionLog = " >> ADMIN :: ADD RFID CARD SELECTED - SWIPE CARD";
        Serial.println("Admin: Add Card selected.");
        addCardMode = true;
        deleteCardMode = false;
        showCentered("ADMIN ADD CARD", "SWIPE CARD NOW");
        if(isOnline) { terminal.println(getFormattedTime() + adminActionLog); terminal.flush(); }
        else { Serial.println(adminActionLog + " (OFFLINE)");}
        // checkRFID will handle, then call showMainMenu, exiting admin mode.
      } else if (choice == '2') { // Delete RFID Card by swipe
        adminActionLog = " >> ADMIN :: DELETE RFID CARD BY SWIPE SELECTED - SWIPE CARD";
        Serial.println("Admin: Delete Card by swipe selected.");
        deleteCardMode = true;
        addCardMode = false;
        showCentered("ADMIN DEL CARD", "SWIPE CARD NOW");
        if(isOnline) { terminal.println(getFormattedTime() + adminActionLog); terminal.flush(); }
        else { Serial.println(adminActionLog + " (OFFLINE)");}
        // checkRFID will handle, then call showMainMenu.
      } else { beepFailure(); showOfflineAdminMenuPage(); }
      break;

    case 3: // Password Page
      if (choice == '1') { // Change User Password
        adminActionLog = " >> ADMIN :: CHANGE USER PASSWORD SELECTED";
        Serial.println("Admin: Change User Password selected.");
        changePasswordMode = true; 
        offlineAdminModeActive = false; // Temporarily exit admin for dedicated password change logic
        offlineAdminMenuPage = 0;
        inputPassword = ""; displayPassword = "";
        showCentered("NEW USER PWD:", ""); 
        if(isOnline) { terminal.println(getFormattedTime() + adminActionLog); terminal.flush(); }
        else { Serial.println(adminActionLog + " (OFFLINE)");}
        // Keypad logic for changePasswordMode will take over.
      } else if (choice == '2') { // Reset User Password
        adminActionLog = " >> ADMIN :: USER PASSWORD RESET TO DEFAULT (8888)";
        Serial.println("Admin: Reset User Password selected.");
        defaultPassword = "8888";
        savePasswordToEEPROM();
        if(isOnline) { terminal.println(getFormattedTime() + adminActionLog); terminal.flush(); }
        else { Serial.println(adminActionLog + " (OFFLINE)"); }
        showCentered("PWD RESET TO", "DEFAULT (8888)"); beepSuccess();
        delay(2000);
        showOfflineAdminMenuPage(); 
      } else { beepFailure(); showOfflineAdminMenuPage(); }
      break;
      
    default:
      beepFailure();
      showOfflineAdminMenuPage(); 
      break;
  }
}

void checkKeypad() {
  char key;
  if (xQueueReceive(keypadQueue, &key, 0) == pdTRUE) {
    Serial.print("Key pressed: "); Serial.println(key);
    lastKeyTime = millis();

    if (systemLocked) { beepFailure(); return; }
    if (blynkCardOpActive) { // If Blynk card operation is active, keypad is mostly ignored
        Serial.println("Keypad ignored: Blynk card operation active.");
        // Optionally beep to indicate keypad is temporarily suspended
        // beepFailure(); 
        return;
    }

    // 1. Handling PIN/Password Entry (after * is pressed)
    if (expectingPinEntry) {
      beepSuccess(); 
      if (key == '#') {
        Serial.print("PIN/Password entered: "); Serial.println(inputPassword);
        String enteredPin = inputPassword; // Store before clearing
        inputPassword = ""; displayPassword = ""; // Clear after use

        if (enteredPin == MASTER_OFFLINE_ADMIN_PIN) {
          Serial.println("Admin PIN correct. Entering Admin Menu.");
          if(isOnline) { terminal.println(getFormattedTime() + " >> KEYPAD :: ADMIN LOGIN SUCCESS"); terminal.flush(); }
          expectingPinEntry = false;
          offlineAdminModeActive = true;
          offlineAdminMenuPage = 1; 
          showOfflineAdminMenuPage();
        } else if (enteredPin == defaultPassword) {
          Serial.println("User password correct. Unlocking door.");
          String msg = getFormattedTime() + " >> KEYPAD :: USER LOGIN SUCCESS - DOOR OPENED";
          if(isOnline) { terminal.println(msg); terminal.flush(); } else { Serial.println(msg + " (OFFLINE)"); }
          showCentered("ACCESS GRANTED", "DOOR UNLOCKED");
          unlockDoor();
          failedAttempts = 0; beepSuccess();
          delay(2000);
          showMainMenu();
        } else {
          Serial.println("Incorrect PIN/Password.");
          String msg = getFormattedTime() + " >> KEYPAD :: LOGIN FAIL - INCORRECT PIN/PASSWORD";
          if(isOnline) { terminal.println(msg); terminal.flush(); } else { Serial.println(msg + " (OFFLINE)"); }
          failedAttempts++;
          showCentered("ACCESS DENIED", "ATTEMPS: " + String(max(0, 5 - failedAttempts)));
          beepFailure();
          if (failedAttempts >= 5) { lockoutSystem(); return; }
          delay(2000);
          expectingPinEntry = false; // Reset this mode
          showMainMenu(); 
        }
      } else if (key == '*') {
        inputPassword = ""; displayPassword = "";
        showCentered("MAT KHAU:", displayPassword);
        Serial.println("PIN entry reset by *.");
      } else if (isdigit(key)) {
        if (inputPassword.length() < 8) { 
          inputPassword += key; displayPassword += "*";
          showCentered("MAT KHAU:", displayPassword);
        } else { beepFailure(); } 
      } else { beepFailure(); }
      return; 
    }

    // 2. Handling Offline Admin Menu Navigation
    if (offlineAdminModeActive) {
      if (key == '#') { // Next page
        beepSuccess();
        offlineAdminMenuPage++;
        if (offlineAdminMenuPage > 3) offlineAdminMenuPage = 1;
        showOfflineAdminMenuPage();
      } else if (key == '*') { // Exit admin menu
        beepSuccess();
        Serial.println("Exiting Admin Menu via *.");
        if(isOnline) { terminal.println(getFormattedTime() + " >> ADMIN :: EXIT MENU"); terminal.flush(); }
        offlineAdminModeActive = false;
        offlineAdminMenuPage = 0;
        showMainMenu();
      } else if (isdigit(key) && (key == '1' || key == '2')) { // Menu choice
         beepSuccess(); 
         handleOfflineAdminAction(offlineAdminMenuPage, key);
      } else {
        beepFailure(); 
        showOfflineAdminMenuPage();
      }
      return; 
    }

    // 3. Handling New User Password Entry (from Admin Menu)
    if (changePasswordMode) {
      beepSuccess();
      if (key == '*') { 
        inputPassword = ""; displayPassword = "";
        showCentered("NEW USER PWD:", "");
        Serial.println("New user password entry reset by *.");
      } else if (key == '#') { 
        if (inputPassword.length() >= 4 && inputPassword.length() <= 8) {
          defaultPassword = inputPassword;
          savePasswordToEEPROM();
          String msg = getFormattedTime() + " >> KEYPAD :: NEW USER PASSWORD SET (ADMIN)";
          if(isOnline) { terminal.println(msg); terminal.flush(); } else { Serial.println(msg + " (OFFLINE)"); }
          showCentered("PWD CHANGED", "SUCCESS (ADMIN)"); beepSuccess();
          Serial.println("User password changed successfully to: " + defaultPassword);
        } else {
          String msg = getFormattedTime() + " >> KEYPAD :: NEW USER PASSWORD FAIL - INVALID LENGTH (4-8 CHARS)";
          if(isOnline) { terminal.println(msg); terminal.flush(); } else { Serial.println(msg + " (OFFLINE)");}
          showCentered("INVALID PWD", "4-8 CHARS REQ"); beepFailure();
          Serial.println("Invalid new password length.");
        }
        delay(2000);
        changePasswordMode = false; 
        inputPassword = ""; displayPassword = ""; // Clear after use
        showMainMenu(); 
      } else if (isdigit(key)) {
        if (inputPassword.length() < 8) {
          inputPassword += key; displayPassword += "*";
          showCentered("NEW USER PWD:", displayPassword);
        } else { beepFailure(); } 
      } else { 
        beepFailure(); 
      }
      return; 
    }

    // 4. Initial '*' press to enter PIN/Password mode (from Idle state)
    if (key == '*' && !expectingPinEntry && !offlineAdminModeActive && !changePasswordMode) {
      beepSuccess();
      expectingPinEntry = true;
      inputPassword = "";
      displayPassword = "";
      showCentered("MAT KHAU:", ""); 
      Serial.println("'*' pressed. Entering PIN/Password mode.");
      if(isOnline) { terminal.println(getFormattedTime() + " >> KEYPAD :: PIN ENTRY MODE ACTIVATED"); terminal.flush(); }
      // No return here, just activates mode. Timeout logic below handles inactivity.
    }
    // Other keys pressed in idle state are ignored or could beepFailure().
  }

  // --- Timeout Logic for Keypad Input Modes (PIN entry, Admin Menu, Change Password) ---
  if ((expectingPinEntry || offlineAdminModeActive || changePasswordMode) &&
      !blynkCardOpActive && // IMPORTANT: Don't timeout keypad modes if a Blynk card op is active
      (millis() - lastKeyTime > PASSWORD_TIMEOUT)) {
    
    String modeTimedOutStr = expectingPinEntry ? "PIN ENTRY" :
                             (offlineAdminModeActive ? "ADMIN MENU" :
                             (changePasswordMode ? "CHANGE PWD" : "INPUT"));
    
    Serial.println(modeTimedOutStr + " timed out due to inactivity.");
    String msg = getFormattedTime() + " >> KEYPAD :: " + modeTimedOutStr + " TIMEOUT";
    if (isOnline) { terminal.println(msg); terminal.flush(); }
    else { Serial.println(msg + " (OFFLINE)"); }
    
    showCentered(modeTimedOutStr, "TIMED OUT");
    beepFailure();
    delay(2000);
    // showMainMenu() will reset all relevant flags:
    // expectingPinEntry, offlineAdminModeActive, changePasswordMode
    showMainMenu(); 
  }
}


void checkBlynkCardOpTimeout() {
    if (blynkCardOpActive && (millis() - blynkCardOpStartTime > BLYNK_CARD_OP_TIMEOUT)) {
        String modeStr = addCardMode ? "ADD CARD" : (deleteCardMode ? "DELETE CARD" : "CARD OP");
        Serial.println("Blynk " + modeStr + " mode timed out. No card swiped.");
        if (isOnline) {
            terminal.println(getFormattedTime() + " >> BLYNK :: " + modeStr + " TIMEOUT - NO CARD SWIPED");
            terminal.flush();
        }
        showCentered(modeStr + " (BLY)", "TIMED OUT");
        beepFailure();

        // Reset all related flags
        addCardMode = false;
        deleteCardMode = false;
        blynkCardOpActive = false;

        delay(2000);
        showMainMenu(); // Go back to main menu
    }
}


// --- Fingerprint, RFID, EEPROM Processing (Terminal messages updated) ---

void processFingerprint() {
  int p = finger.image2Tz(); 
  if (p != FINGERPRINT_OK) {
    if (millis() - lastErrorTime > ERROR_COOLDOWN) {
      Serial.println("processFingerprint: Failed to convert image, code " + String(p));
      String msg = getFormattedTime() + " >> FINGERPRINT :: IMAGE CONVERT FAIL - Code: " + String(p);
      if(isOnline){ terminal.println(msg); terminal.flush(); } else { Serial.println(msg + " (OFFLINE)");}
      showCentered("FP SCAN ERROR", "TRY AGAIN"); lastErrorTime = millis();
    }
    beepFailure(); delay(1000); showMainMenu(); return;
  }
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    int fid = finger.fingerID;
    String logMsg = getFormattedTime() + " >> FINGERPRINT :: ACCESS GRANTED - ID: " + String(fid) + ", Confidence: " + String(finger.confidence);
    if (isOnline) { terminal.println(logMsg); terminal.flush(); } else { Serial.println(logMsg + " (OFFLINE)"); }
    showCentered("ACCESS GRANTED", "FINGER ID: " + String(fid));
    unlockDoor(); failedAttempts = 0; beepSuccess();
    Serial.println("processFingerprint: Access Granted, ID " + String(fid));
  } else {
    String reason = (p == FINGERPRINT_NOTFOUND) ? "NOT FOUND" : ((p == FINGERPRINT_PACKETRECIEVEERR) ? "COMM ERR" : "UNKNOWN ERR (" + String(p) + ")");
    String logMsg = getFormattedTime() + " >> FINGERPRINT :: ACCESS DENIED - Reason: " + reason;
    if (isOnline) { terminal.println(logMsg); terminal.flush(); } else { Serial.println(logMsg + " (OFFLINE)"); }
    failedAttempts++;
    showCentered("ACCESS DENIED", "ATTEMPTS: " + String(max(0,5 - failedAttempts))); beepFailure();
    Serial.println("processFingerprint: Access Denied, Code: " + String(p));
    if (failedAttempts >= 5) { lockoutSystem(); return; }
  }
  delay(2000); showMainMenu();
}

void deleteFingerprintByScan() {
  Serial.println("deleteFingerprintByScan: Image captured. Converting...");
  int p = finger.image2Tz(); 
  if (p != FINGERPRINT_OK) {
    if (millis() - lastErrorTime > ERROR_COOLDOWN) {
      Serial.println("deleteFingerprintByScan: Failed to convert image, code " + String(p));
      String msg = getFormattedTime() + " >> FINGERPRINT :: DELETE SCAN - IMAGE CONVERT FAIL - Code: " + String(p);
      if(isOnline){ terminal.println(msg); terminal.flush(); } else { Serial.println(msg + " (OFFLINE)");}
      showCentered("FP SCAN ERROR", "TRY AGAIN"); lastErrorTime = millis();
    }
    beepFailure(); delay(1000); fingerprintMode = FINGER_CHECK; showMainMenu(); return;
  }
  Serial.println("deleteFingerprintByScan: Searching for match to delete...");
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    int fid = finger.fingerID;
    Serial.println("deleteFingerprintByScan: Match found, ID " + String(fid) + ". Deleting...");
    if (finger.deleteModel(fid) == FINGERPRINT_OK) {
      String logMsg = getFormattedTime() + " >> FINGERPRINT :: DELETE SCAN SUCCESS - ID " + String(fid) + " DELETED";
      if (isOnline) { terminal.println(logMsg); terminal.flush(); } else { Serial.println(logMsg + " (OFFLINE)"); }
      showCentered("FINGER DELETED", "ID: " + String(fid)); beepSuccess();
      Serial.println("deleteFingerprintByScan: Success, ID " + String(fid));
    } else {
      String logMsg = getFormattedTime() + " >> FINGERPRINT :: DELETE SCAN FAIL - SENSOR ERROR ON DELETE ID " + String(fid);
      if (isOnline) { terminal.println(logMsg); terminal.flush(); } else { Serial.println(logMsg + " (OFFLINE)"); }
      showCentered("DELETE FAILED", "SENSOR ERROR?"); beepFailure();
      Serial.println("deleteFingerprintByScan: Failed to delete ID " + String(fid) + " from sensor.");
    }
  } else {
    String reason = (p == FINGERPRINT_NOTFOUND) ? "NOT FOUND" : "SCAN ERR (" + String(p) + ")";
    String logMsg = getFormattedTime() + " >> FINGERPRINT :: DELETE SCAN FAIL - FINGER " + reason;
    if (isOnline) { terminal.println(logMsg); terminal.flush(); } else { Serial.println(logMsg + " (OFFLINE)"); }
    showCentered("FINGER NOT FOUND", "TRY AGAIN"); beepFailure();
    Serial.println("deleteFingerprintByScan: Fingerprint not found, Code: " + String(p));
  }
  delay(2000); 
  fingerprintMode = FINGER_CHECK; 
  // If called from Admin, showOfflineAdminMenuPage() might be desired, but current flow returns to showMainMenu()
  // For Blynk V7, showMainMenu() is correct.
  showMainMenu();
}

void addFingerprint_internal() {
  Serial.println("addFingerprint_internal: Process for ID " + String(fingerID));
  String opContext = offlineAdminModeActive ? "ADMIN" : (isOnline ? "BLYNK V6" : "OFFLINE OP");
  String termPrefix = getFormattedTime() + " >> FINGERPRINT ("+opContext+") ID " + String(fingerID) + " :: ";
  String serialPrefix = "FP Internal ("+opContext+") ID " + String(fingerID) + ": ";

  if (!finger.verifyPassword()) { 
      if (millis() - lastErrorTime > ERROR_COOLDOWN) {
         Serial.println(serialPrefix + "Sensor comm error pre-enroll.");
         if(isOnline) terminal.println(termPrefix + "ADD FAIL - SENSOR COMM ERROR PRE-ENROLL");
         lastErrorTime = millis();
      }
      showCentered("SENSOR ERROR", "CHECK WIRING"); beepFailure(); delay(2000);
      return; 
  }

  int p = enrollFingerprint(fingerID); // enrollFingerprint handles LCD, beeps, and detailed logging

  if (p == FINGERPRINT_OK) {
    if (isOnline) { terminal.println(termPrefix + "ADD SUCCESS"); } 
    else { Serial.println(serialPrefix + "ADD SUCCESS (OFFLINE)"); }
    Serial.println(serialPrefix + "Success. Next available ID: " + String(fingerID + 1));
    fingerID++; 
  } else {
    if (isOnline) { terminal.println(termPrefix + "ADD FAIL - Enroll Error Code: " + String(p)); } 
    else { Serial.println(serialPrefix + "ADD FAIL - Enroll Error Code: " + String(p) + " (OFFLINE)"); }
    Serial.println(serialPrefix + "Failed, error " + String(p));
  }
  if(isOnline) terminal.flush();
  delay(2500); // Allow user to see final message from enrollFingerprint
  // The calling function (Blynk V6 or Admin Handler) will call showMainMenu or showOfflineAdminMenuPage
}

int enrollFingerprint(int id_to_enroll) {
  int p = -1;
  String opContext = offlineAdminModeActive ? "ADMIN" : (isOnline ? "BLYNK V6" : "FP ENROLL");
  String logPrefixBase = "FINGERPRINT ("+opContext+") ID " + String(id_to_enroll) + " :: ENROLL - ";
  String termPrefix = getFormattedTime() + " >> " + logPrefixBase;
  String serialPrefix = logPrefixBase;

#define SEND_LOG(msg_suffix) \
    do { \
        if(isOnline) terminal.println(termPrefix + msg_suffix); \
        Serial.println(serialPrefix + msg_suffix + (isOnline ? "" : " (OFFLINE)")); \
        if(isOnline) terminal.flush(); \
    } while(0)

  SEND_LOG("Waiting for finger (1st scan)...");
  showCentered("ENROLL ID " + String(id_to_enroll), "PLACE FINGER");
  unsigned long startTime = millis();
  while (p != FINGERPRINT_OK && (millis() - startTime < 15000)) { 
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) { vTaskDelay(50 / portTICK_PERIOD_MS); continue; }
    if (p != FINGERPRINT_OK) {
      SEND_LOG("GetImage1 Error: " + String(p));
      showCentered("SCAN ERROR 1", "CODE: " + String(p)); beepFailure(); return p;
    }
  }
  if (p != FINGERPRINT_OK) { 
    SEND_LOG("Timeout Image1");
    showCentered("TIMEOUT IMG 1", "TRY AGAIN"); beepFailure(); return FINGERPRINT_TIMEOUT;
  }
  SEND_LOG("Image 1 taken.");
  p = finger.image2Tz(1); 
  if (p != FINGERPRINT_OK) {
    SEND_LOG("Convert1 Error: " + String(p));
    showCentered("CONVERT ERR 1", "CODE: " + String(p)); beepFailure(); return p;
  }
  SEND_LOG("Image 1 converted. Remove finger.");
  showCentered("REMOVE FINGER", "WAIT..."); beepSuccess(); delay(2000); 
  
  startTime = millis(); p = 0; 
  while (p != FINGERPRINT_NOFINGER && (millis() - startTime < 10000)) { 
    p = finger.getImage(); vTaskDelay(50 / portTICK_PERIOD_MS);
  }
  if (p != FINGERPRINT_NOFINGER) {
    SEND_LOG("Removal Timeout/Error: " + String(p));
    showCentered("REMOVAL TIMEOUT", "TRY AGAIN"); beepFailure(); return FINGERPRINT_TIMEOUT; 
  }
  SEND_LOG("Finger removed. Place same finger again (2nd scan)...");
  showCentered("PLACE SAME FINGER", "AGAIN"); beepSuccess();
  
  startTime = millis(); p = -1;
  while (p != FINGERPRINT_OK && (millis() - startTime < 15000)) { 
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) { vTaskDelay(50 / portTICK_PERIOD_MS); continue; }
    if (p != FINGERPRINT_OK) {
      SEND_LOG("GetImage2 Error: " + String(p));
      showCentered("SCAN ERROR 2", "CODE: " + String(p)); beepFailure(); return p;
    }
  }
  if (p != FINGERPRINT_OK) { 
    SEND_LOG("Timeout Image2");
    showCentered("TIMEOUT IMG 2", "TRY AGAIN"); beepFailure(); return FINGERPRINT_TIMEOUT;
  }
  SEND_LOG("Image 2 taken.");
  p = finger.image2Tz(2); 
  if (p != FINGERPRINT_OK) {
    SEND_LOG("Convert2 Error: " + String(p));
    showCentered("CONVERT ERR 2", "CODE: " + String(p)); beepFailure(); return p;
  }
  SEND_LOG("Image 2 converted. Creating model...");
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    SEND_LOG("CreateModel Error: " + String(p) + (p == FINGERPRINT_ENROLLMISMATCH ? " - FINGERPRINTS DID NOT MATCH!" : ""));
    if (p == FINGERPRINT_ENROLLMISMATCH) {
      showCentered("NO MATCH", "FINGERS DIFFER?");
    } else { showCentered("MODEL ERR", "CODE: " + String(p)); }
    beepFailure(); return p;
  }
  SEND_LOG("Model created. Storing at ID: " + String(id_to_enroll));
  p = finger.storeModel(id_to_enroll);
  if (p != FINGERPRINT_OK) {
    SEND_LOG("StoreModel Error: " + String(p));
    showCentered("STORE ERROR", "CODE: " + String(p)); beepFailure(); return p;
  }
  SEND_LOG("Model stored successfully!");
  showCentered("FINGER ADDED", "ID: " + String(id_to_enroll)); beepSuccess();
  return FINGERPRINT_OK; 
#undef SEND_LOG
}

void deleteFingerprint(int id_to_delete) {
  Serial.println("deleteFingerprint: Attempting to delete ID " + String(id_to_delete));
  // Determine context for logging (Blynk V9 or Admin)
  String opContext = "FINGERPRINT"; // Generic, could be enhanced if more contexts call this
  if (pendingFingerID != "") opContext = "BLYNK V9"; // If pendingFingerID is set, it's likely from Blynk V9
  else if (offlineAdminModeActive) opContext = "ADMIN";


  String termPrefix = getFormattedTime() + " >> " + opContext + " ID " + String(id_to_delete) + " :: ";
  String serialPrefix = opContext + " ID " + String(id_to_delete) + ": ";

  if (id_to_delete < 1 || id_to_delete > 199) { 
      Serial.println(serialPrefix + "Invalid Fingerprint ID for deletion.");
      if (isOnline) terminal.println(termPrefix + "DELETE FAIL - INVALID ID (1-199)");
      showCentered("INVALID ID", "RANGE 1-199"); beepFailure(); delay(2000); showMainMenu(); return;
  }
  if (finger.deleteModel(id_to_delete) == FINGERPRINT_OK) {
    if (isOnline) { terminal.println(termPrefix + "DELETE SUCCESS"); } 
    else { Serial.println(serialPrefix + "DELETE SUCCESS (OFFLINE)"); }
    showCentered("FINGER DELETED", "ID: " + String(id_to_delete)); beepSuccess();
    Serial.println(serialPrefix + "Delete success.");
  } else {
    if (isOnline) { terminal.println(termPrefix + "DELETE FAIL - SENSOR ERROR/NOT FOUND"); } 
    else { Serial.println(serialPrefix + "DELETE FAIL - SENSOR ERROR/NOT FOUND (OFFLINE)"); }
    showCentered("DELETE FAILED", "CHECK ID/SENSOR"); beepFailure();
    Serial.println(serialPrefix + "Delete failed.");
  }
  if(isOnline) terminal.flush();
  delay(2000); showMainMenu(); 
}

void checkRFID() {
  String* uidPtr = NULL; 
  if (xQueueReceive(rfidQueue, &uidPtr, 0) == pdTRUE && uidPtr != NULL) {
    String uid = *uidPtr; delete uidPtr;   
    Serial.print("Processing RFID UID from queue: "); Serial.println(uid);
    beepSuccess(); 
    
    String opContext = "RFID";
    if (addCardMode && blynkCardOpActive) opContext = "BLYNK V3 ADD";
    else if (deleteCardMode && blynkCardOpActive) opContext = "BLYNK V4 DELETE";
    else if (addCardMode && offlineAdminModeActive) opContext = "ADMIN ADD";
    else if (deleteCardMode && offlineAdminModeActive) opContext = "ADMIN DELETE";
    else opContext = "RFID ACCESS";

    String termPrefix = getFormattedTime() + " >> " + opContext + " :: ";
    String serialPrefix = opContext + ": ";


    if (addCardMode) { 
      bool alreadyExists = false;
      for(int i=0; i < cardCount; i++){ if(knownCards[i] == uid){ alreadyExists = true; break; } }
      
      if(alreadyExists){
        if (isOnline) { terminal.println(termPrefix + "ADD FAIL - CARD ALREADY EXISTS - UID: " + uid); }
        Serial.println(serialPrefix + "Card already exists: " + uid + (isOnline ? "" : " (OFFLINE)"));
        showCentered("CARD EXISTS", uid.substring(0, min((int)uid.length(), 8))); beepFailure();
      } else if (cardCount < 100) {
        knownCards[cardCount] = uid; saveCardToEEPROM(cardCount, uid); 
        if (isOnline) { terminal.println(termPrefix + "ADD SUCCESS - UID: " + uid); }
        Serial.println(serialPrefix + "Card added: " + uid + (isOnline ? "" : " (OFFLINE)")); cardCount++;
        showCentered("CARD ADDED", uid.substring(0, min((int)uid.length(), 8))); beepSuccess();
      } else { 
        if (isOnline) { terminal.println(termPrefix + "ADD FAIL - STORAGE FULL - UID: " + uid); }
        Serial.println(serialPrefix + "Card storage full, cannot add: " + uid + (isOnline ? "" : " (OFFLINE)"));
        showCentered("STORAGE FULL", "CANNOT ADD"); beepFailure();
      }
      addCardMode = false; 
      if(blynkCardOpActive) blynkCardOpActive = false; // Reset blynk flag if it was a blynk operation
    
    } else if (deleteCardMode) { 
      if (deleteCardFromEEPROM(uid)) { 
        if (isOnline) { terminal.println(termPrefix + "DELETE SUCCESS - UID: " + uid); }
        Serial.println(serialPrefix + "Card deleted: " + uid + (isOnline ? "" : " (OFFLINE)"));
        showCentered("CARD DELETED", uid.substring(0, min((int)uid.length(), 8))); beepSuccess();
      } else { 
        if (isOnline) { terminal.println(termPrefix + "DELETE FAIL - CARD NOT FOUND - UID: " + uid); }
        Serial.println(serialPrefix + "Card not found to delete: " + uid + (isOnline ? "" : " (OFFLINE)"));
        showCentered("CARD NOT FOUND", "CHECK UID"); beepFailure();
      }
      deleteCardMode = false; 
      if(blynkCardOpActive) blynkCardOpActive = false; // Reset blynk flag

    } else { // Normal access check
      if (isCardKnown(uid)) { 
        if (isOnline) { terminal.println(termPrefix + "ACCESS GRANTED - UID: " + uid); }
        Serial.println(serialPrefix + "Valid card, access granted: " + uid + (isOnline ? "" : " (OFFLINE)"));
        showCentered("ACCESS GRANTED", "CARD OK"); unlockDoor(); failedAttempts = 0; beepSuccess();
      } else { 
        if (isOnline) { terminal.println(termPrefix + "ACCESS DENIED - INVALID CARD - UID: " + uid); }
        Serial.println(serialPrefix + "Invalid card, access denied: " + uid + (isOnline ? "" : " (OFFLINE)"));
        failedAttempts++; showCentered("ACCESS DENIED", "ATTEMPTS: " + String(max(0,5-failedAttempts))); beepFailure();
        if (failedAttempts >= 5) { lockoutSystem(); return; } // lockoutSystem calls showMainMenu
      }
    }
    if (isOnline) terminal.flush();
    delay(2000);
    showMainMenu(); 
  }
}

bool isCardKnown(String uid) {
  for (int i = 0; i < cardCount; i++) if (knownCards[i] == uid) return true;
  return false;
}

void saveCardToEEPROM(int index, String uid) {
  int addr = index * 10; 
  Serial.print("EEPROM Save Card: Index " + String(index) + ", UID: " + uid + " at Addr: " + String(addr));
  for (int i = 0; i < 10; i++) {
    if (i < uid.length()) EEPROM.write(addr + i, uid[i]);
    else EEPROM.write(addr + i, 0); 
  }
  if (EEPROM.commit()) Serial.println(" ...Committed.");
  else Serial.println(" ...Commit FAILED.");
}

void loadCardsFromEEPROM() {
  cardCount = 0; Serial.println("EEPROM Load Cards: Loading cards...");
  int clearedSlots = 0;
  for (int i = 0; i < 100; i++) {
    String uid = ""; int addr = i * 10; bool validCardCharFound = false; 
    for (int j = 0; j < 10; j++) {
      char c = EEPROM.read(addr + j);
      if (c == 0 || (c == 0xFF && j == 0)) break; 
      uid += c; validCardCharFound = true;
    }
    if (uid.length() > 0 && validCardCharFound && uid.length() <= 8) { 
      knownCards[cardCount++] = uid; Serial.println("EEPROM Load Cards: Loaded Card[" + String(i) + "]: " + uid);
    } else if (uid.length() > 0 || (EEPROM.read(addr) != 0xFF && EEPROM.read(addr) != 0x00) ) {
       // Found non-standard/corrupted data that isn't a valid empty slot
       bool needsClearing = false;
       for(int k=0; k < 10; ++k) if(EEPROM.read(addr+k) != 0x00 && EEPROM.read(addr+k) != 0xFF) { needsClearing = true; break; }
       if(needsClearing) {
           Serial.println("EEPROM Load Cards: Slot " + String(i) + " (Addr: " + String(addr) + ") contains non-standard data (" + uid + "). Clearing slot.");
           for (int k=0; k < 10; k++) EEPROM.write(addr+k, 0);
           clearedSlots++;
       }
    }
  }
  if(clearedSlots > 0) {
    if(EEPROM.commit()) Serial.println("EEPROM Load Cards: Committed clearing of " + String(clearedSlots) + " corrupted slots.");
    else Serial.println("EEPROM Load Cards: FAILED to commit clearing of corrupted slots.");
  }
  Serial.println("EEPROM Load Cards: Total cards loaded: " + String(cardCount));
  if(isOnline && cardCount > 0) { terminal.println(getFormattedTime() + " >> EEPROM :: " + String(cardCount) + " CARDS LOADED"); terminal.flush(); }
}

bool deleteCardFromEEPROM(String uid) {
  int foundIndex = -1;
  for (int i = 0; i < cardCount; i++) if (knownCards[i] == uid) { foundIndex = i; break; }
  
  if (foundIndex != -1) {
    Serial.println("EEPROM Delete Card: UID " + uid + " found at index " + String(foundIndex) + ". Deleting.");
    int addr = foundIndex * 10;
    for (int j = 0; j < 10; j++) EEPROM.write(addr + j, 0); 
    
    // Shift remaining cards in the array
    for (int k = foundIndex; k < cardCount - 1; k++) knownCards[k] = knownCards[k+1];
    knownCards[cardCount - 1] = ""; // Clear last element
    cardCount--;

    if(EEPROM.commit()) Serial.println("EEPROM Delete Card: Commit SUCCESS. New count: " + String(cardCount));
    else Serial.println("EEPROM Delete Card: Commit FAIL. UID " + uid);
    return true;
  }
  Serial.println("EEPROM Delete Card: UID " + uid + " not found for deletion.");
  return false;
}

void savePasswordToEEPROM() {
  Serial.print("EEPROM Save Password: " + defaultPassword);
  for (int i = 0; i < 8; i++) { 
    if (i < defaultPassword.length()) EEPROM.write(PASSWORD_ADDR + i, defaultPassword[i]);
    else EEPROM.write(PASSWORD_ADDR + i, 0); 
  }
  if(EEPROM.commit()) Serial.println(" ...Committed.");
  else Serial.println(" ...Commit FAILED.");
}

void loadPasswordFromEEPROM() {
  Serial.print("EEPROM Load Password: Loading... ");
  String pwd = ""; bool validPwdCharFound = false;
  for (int i = 0; i < 8; i++) {
    char c = EEPROM.read(PASSWORD_ADDR + i);
    if (c == 0 || (c == 0xFF && i == 0)) break;
    pwd += c; validPwdCharFound = true;
  }
  if (pwd.length() >= 4 && pwd.length() <= 8 && validPwdCharFound) {
    defaultPassword = pwd; Serial.println("Loaded: " + defaultPassword);
    if(isOnline) { terminal.println(getFormattedTime() + " >> EEPROM :: USER PASSWORD LOADED"); terminal.flush(); }
  } else {
    Serial.println("No valid password found or invalid length. Using default '8888' and saving.");
    defaultPassword = "8888"; savePasswordToEEPROM(); 
    if(isOnline) { terminal.println(getFormattedTime() + " >> EEPROM :: USER PASSWORD RESET TO DEFAULT (8888)"); terminal.flush(); }
  }
}

void clearEEPROM() {
  Serial.println("EEPROM Clear: Clearing all Cards & Password...");
  // Clear card storage area (0 to PASSWORD_ADDR - 1) and password area
  for (int i = 0; i < PASSWORD_ADDR + 8; i++) EEPROM.write(i, 0); 
  
  bool commitSuccess = EEPROM.commit(); 
  if(commitSuccess) Serial.println("EEPROM Clear: Memory commit SUCCESS.");
  else Serial.println("EEPROM Clear: Memory commit FAIL.");

  cardCount = 0; 
  for(int i=0; i<100; i++) knownCards[i] = ""; 
  
  Serial.println("EEPROM Clear: Attempting to clear fingerprint sensor database...");
  String fpClearMsg;
  if(finger.verifyPassword()){
    if(finger.emptyDatabase() == FINGERPRINT_OK){
      fpClearMsg = " >> FINGERPRINT :: SENSOR DATABASE CLEARED";
      Serial.println("Fingerprint DB cleared.");
    } else {
      fpClearMsg = " >> FINGERPRINT :: SENSOR DATABASE CLEAR FAIL";
      Serial.println("Failed to clear FP DB from sensor.");
    }
  } else {
    fpClearMsg = " >> FINGERPRINT :: SENSOR COMM ERROR ON CLEAR DB";
    Serial.println("FP Sensor comm error on attempting to clear DB.");
  }
  if(isOnline) { terminal.println(getFormattedTime() + fpClearMsg); }
  else { Serial.println(fpClearMsg + " (OFFLINE)"); }
  
  fingerID = 1; 
  defaultPassword = "8888"; savePasswordToEEPROM(); // Resave default password
  
  Serial.println("EEPROM Clear: All data reset to defaults.");
  if(isOnline && commitSuccess) { terminal.println(getFormattedTime() + " >> EEPROM :: ALL DATA CLEARED AND RESET"); terminal.flush(); }
  else if (isOnline && !commitSuccess) { terminal.println(getFormattedTime() + " >> EEPROM :: DATA CLEAR ATTEMPTED (COMMIT FAIL)"); terminal.flush(); }
}

void listRFIDCards() {
  if (!isOnline) {
    Serial.println("Offline. Cannot list cards to Blynk terminal."); 
    showCentered("OFFLINE", "CANNOT LIST CARDS");
    delay(2000); showMainMenu(); return;
  }
  terminal.println("==== STORED RFID CARDS (" + String(cardCount) + ") ====");
  if (cardCount == 0) terminal.println("NO CARDS STORED");
  else {
    bool any = false;
    for (int i = 0; i < cardCount; i++) {
      if (knownCards[i].length() > 0) { // Ensure card string is not empty
        terminal.println("CARD[" + String(i) + "]: " + knownCards[i]); 
        any = true; 
      }
    }
    if (!any && cardCount > 0) terminal.println("INFO: Card count is " + String(cardCount) + " but no valid UIDs found in array (potential data issue).");
    else if (!any && cardCount == 0) terminal.println("INFO: No cards currently stored."); // Redundant with above but good for clarity
  }
  terminal.println("============================"); terminal.flush();
}