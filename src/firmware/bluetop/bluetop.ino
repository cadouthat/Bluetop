#include <bluefruit.h>

#define FAN_PIN 16
#define DUMP_PIN 15
#define TRAY_PIN 7
#define DRUM_PIN 11
#define HEAT_PIN 27
#define TEMP1_PIN A0
#define TEMP2_PIN A1

#define LOG_TEMPS false
#define TEMP_SAMPLES 50
#define NOTIFY_INTERVAL_MS 1000
#define HEAT_CYCLE_MS 500

#define HEAT_LEVELS 10
#define FAN_LEVELS 10

enum Mode {
  STANDBY,
  PREHEAT,
  ROAST,
  EJECT,
  COOL
};

#define PREHEAT_TEMP_LOW 170
#define PREHEAT_TEMP_HIGH 190
#define SAFETY_SHUTOFF_TEMP 450
#define EJECT_DURATION_MS 60000
#define COOL_TEMP 160

uint8_t BIG_ENDIAN_UUIDS[][16] = {
  {0x7a,0x61,0x95,0x9e,0x61,0x1c,0x43,0x46,0x9b,0x27,0x69,0x3e,0x3c,0x18,0x63,0x68}, // Service
  {0x34,0x03,0x30,0x01,0xc8,0x04,0x4c,0x95,0xa1,0x23,0xab,0x26,0x8e,0xee,0xd4,0x7e}, // Mode
  {0xae,0x79,0xe4,0x2c,0xae,0x7e,0x4e,0xfa,0x8e,0xd8,0x42,0x10,0xfa,0xd6,0x65,0x2f}, // Heat
  {0x5f,0x71,0x7e,0xa9,0x68,0x3b,0x40,0x50,0x99,0x23,0x32,0xc7,0xfa,0xa7,0xd1,0x1d}, // Fan
  {0xe5,0x84,0xef,0x20,0x0f,0x9c,0x45,0xfa,0xbf,0x13,0x3b,0x30,0xcd,0x28,0x40,0x42}, // Temp1
  {0x01,0x6d,0x72,0x3f,0x5b,0xa7,0x4a,0x62,0xba,0x77,0x4a,0x3f,0x6a,0x08,0x19,0x63}  // Temp2
};
uint8_t UUIDS[8][16];

uint8_t* nextUuid() {
  static int nextConst = 0;
  uint8_t *in = BIG_ENDIAN_UUIDS[nextConst];
  uint8_t *out = UUIDS[nextConst];
  for (int i = 0; i < 16; i++) {
    out[15 - i] = in[i];
  }
  nextConst++;
  return out;
}

BLEService        service = BLEService(nextUuid());
BLECharacteristic modeCh = BLECharacteristic(nextUuid());
BLECharacteristic heatCh = BLECharacteristic(nextUuid());
BLECharacteristic fanCh = BLECharacteristic(nextUuid());
BLECharacteristic temp1Ch = BLECharacteristic(nextUuid());
BLECharacteristic temp2Ch = BLECharacteristic(nextUuid());

// Device Information Service
BLEDis dis;

Mode activeMode = STANDBY;
unsigned long activeModeMs = 0;
int activeHeatLevel = 0;
int activeFanLevel = 0;

int temp1Samples[TEMP_SAMPLES] = {0};
int temp2Samples[TEMP_SAMPLES] = {0};
int tempIndex = 0;
int temp1 = 0;
int temp2 = 0;

void setup() {
  // nrf52840 with native usb
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }

  Serial.println("\nRoast controller booted");
  Serial.println("-----------------------\n");

  pinMode(DRUM_PIN, OUTPUT);
  pinMode(HEAT_PIN, OUTPUT); // PWM
  pinMode(FAN_PIN, OUTPUT); // PWM
  pinMode(TRAY_PIN, OUTPUT);
  pinMode(DUMP_PIN, OUTPUT);
  // TEMP1_PIN is analog input
  // TEMP2_PIN is analog input

  Bluefruit.begin();
  Bluefruit.setName("Bluetop");
  Bluefruit.setConnectCallback(onConnect);
  Bluefruit.setDisconnectCallback(onDisconnect);

  dis.setManufacturer("Bluetop");
  dis.setModel("0.0.1");
  dis.begin();

  Serial.println("Configuring the service");
  setupService();

  Serial.println("Starting advertising");
  startAdvertising();

  Serial.println("Setup complete!");
}

void startAdvertising(void) {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(service);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  // Units of 0.625ms, fast mode 20ms, slow mode 152.5ms
  Bluefruit.Advertising.setInterval(32, 244);
  // Number of seconds in fast mode
  Bluefruit.Advertising.setFastTimeout(30);
  // Start with no timeout
  Bluefruit.Advertising.start(0);
}

void setupService(void) {
  service.begin();

  modeCh.setProperties(CHR_PROPS_WRITE | CHR_PROPS_NOTIFY);
  modeCh.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  modeCh.setFixedLen(1);
  modeCh.setWriteCallback(onWriteMode);
  modeCh.setCccdWriteCallback(onModeCccd);
  modeCh.begin();

  heatCh.setProperties(CHR_PROPS_WRITE | CHR_PROPS_NOTIFY);
  heatCh.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  heatCh.setFixedLen(1);
  heatCh.setWriteCallback(onWriteHeat);
  heatCh.setCccdWriteCallback(onHeatCccd);
  heatCh.begin();

  fanCh.setProperties(CHR_PROPS_WRITE | CHR_PROPS_NOTIFY);
  fanCh.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  fanCh.setFixedLen(1);
  fanCh.setWriteCallback(onWriteFan);
  fanCh.setCccdWriteCallback(onFanCccd);
  fanCh.begin();

  temp1Ch.setProperties(CHR_PROPS_NOTIFY);
  temp1Ch.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  temp1Ch.setFixedLen(2);
  temp1Ch.setCccdWriteCallback(onTemp1Cccd);
  temp1Ch.begin();

  temp2Ch.setProperties(CHR_PROPS_NOTIFY);
  temp2Ch.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  temp2Ch.setFixedLen(2);
  temp2Ch.setCccdWriteCallback(onTemp2Cccd);
  temp2Ch.begin();
}

void notifyMode() {
  if (Bluefruit.connected()) {
    if (!modeCh.notify8((uint8_t)activeMode)) {
      Serial.println("Error: mode notify failed!");
    }
  }
}

void notifyHeat() {
  if (Bluefruit.connected()) {
    if (!heatCh.notify8(activeHeatLevel)) {
      Serial.println("Error: heat notify failed!");
    }
  }
}

void notifyFan() {
  if (Bluefruit.connected()) {
    if (!fanCh.notify8(activeFanLevel)) {
      Serial.println("Error: fan notify failed!");
    }
  }
}

void notifyTemp1() {
  if (Bluefruit.connected()) {
    if (!temp1Ch.notify16(temp1)) {
      Serial.println("Error: temp1 notify failed!");
    }
  }
}

void notifyTemp2() {
  if (Bluefruit.connected()) {
    if (!temp2Ch.notify16(temp2)) {
      Serial.println("Error: temp2 notify failed!");
    }
  }
}

void setHeatLevel(int heatLevel) {
  if (activeHeatLevel == heatLevel) {
    return;
  }
  activeHeatLevel = heatLevel;

  notifyHeat();
}

void setFanLevel(int fanLevel) {
  if (activeFanLevel == fanLevel) {
    return;
  }
  activeFanLevel = fanLevel;

  int fanPwm = 255 - min(255, round((float)activeFanLevel / FAN_LEVELS * 255));
  analogWrite(FAN_PIN, fanPwm);

  notifyFan();
}

void setState(bool drum, bool tray, bool dump, int heatLevel, int fanLevel) {
  digitalWrite(DRUM_PIN, drum ? LOW : HIGH);
  digitalWrite(TRAY_PIN, tray ? LOW : HIGH);
  digitalWrite(DUMP_PIN, dump ? LOW : HIGH);
  setHeatLevel(heatLevel);
  setFanLevel(fanLevel);
}

void setMode(Mode m) {
  if (activeMode == m) {
    return;
  }
  Serial.print("setMode ");
  Serial.println(m);

  activeMode = m;
  activeModeMs = millis();
  switch (activeMode) {
    case STANDBY:
      setState(false, false, false, 0, 0);
      break;
    case PREHEAT:
    case ROAST:
      setState(true, false, false, HEAT_LEVELS, 0);
      break;
    case EJECT:
      setState(true, true, true, 0, FAN_LEVELS);
      break;
    case COOL:
      setState(true, true, false, 0, FAN_LEVELS);
      break;
  }

  notifyMode();
}

void onConnect(uint16_t conn_handle) {
  char central_name[32] = { 0 };
  Bluefruit.Gap.getPeerName(conn_handle, central_name, sizeof(central_name));

  Serial.print("Connected to ");
  Serial.println(central_name);
}

void onDisconnect(uint16_t conn_handle, uint8_t reason) {
  Serial.println("Disconnected");
}

void onModeCccd(BLECharacteristic& chr, uint16_t cccd_value) {
  Serial.print("Mode CCCD Updated: ");
  Serial.println(cccd_value);

  notifyMode();
}

void onHeatCccd(BLECharacteristic& chr, uint16_t cccd_value) {
  Serial.print("Heat CCCD Updated: ");
  Serial.println(cccd_value);

  notifyHeat();
}

void onFanCccd(BLECharacteristic& chr, uint16_t cccd_value) {
  Serial.print("Fan CCCD Updated: ");
  Serial.println(cccd_value);

  notifyFan();
}

void onTemp1Cccd(BLECharacteristic& chr, uint16_t cccd_value) {
  Serial.print("Temp1 CCCD Updated: ");
  Serial.println(cccd_value);

  notifyTemp1();
}

void onTemp2Cccd(BLECharacteristic& chr, uint16_t cccd_value) {
  Serial.print("Temp2 CCCD Updated: ");
  Serial.println(cccd_value);

  notifyTemp2();
}

void onWriteMode(BLECharacteristic& chr, uint8_t* data, uint16_t len, uint16_t offset) {
  Serial.print("Mode value updated: ");
  Serial.println(data[0]);

  setMode((Mode)data[0]);
}
void onWriteHeat(BLECharacteristic& chr, uint8_t* data, uint16_t len, uint16_t offset) {
  Serial.print("Heat value updated: ");
  Serial.println(data[0]);

  setHeatLevel(min(HEAT_LEVELS, data[0]));
}
void onWriteFan(BLECharacteristic& chr, uint8_t* data, uint16_t len, uint16_t offset) {
  Serial.print("Fan value updated: ");
  Serial.println(data[0]);

  setFanLevel(data[0]);
}

void cycleHeatState(unsigned long curMs) {
  static bool heatState = false;
  unsigned long heatCycle = curMs % (HEAT_CYCLE_MS * HEAT_LEVELS);
  if (heatState != (heatCycle < HEAT_CYCLE_MS * activeHeatLevel)) {
    heatState = !heatState;
    digitalWrite(HEAT_PIN, heatState ? LOW : HIGH);
  }
}

int readTemp(int pin) {
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  int analog = analogRead(pin);
  float v = 3.0 * analog / ((1 << 12) - 1);
  float tempC = (v - 1.25) / 0.005;
  float tempF = tempC * 9 / 5 + 32;
  return tempF;
}

void sampleTemps() {
  temp1Samples[tempIndex] = readTemp(TEMP1_PIN);
  temp2Samples[tempIndex] = readTemp(TEMP2_PIN);
  tempIndex = (tempIndex + 1) % TEMP_SAMPLES;
}

int averageTemp(int samples[]) {
  float avg = 0;
  for (int i = 0; i < TEMP_SAMPLES; i++) {
    avg += (float)samples[i] / TEMP_SAMPLES;
  }
  return round(avg);
}

void updateTemps() {
  temp1 = averageTemp(temp1Samples);
  temp2 = averageTemp(temp2Samples);

  if (LOG_TEMPS) {
    Serial.print("Current temp: ");
    Serial.print(temp1);
    Serial.print(", ");
    Serial.println(temp2);
  }

  notifyTemp1();
  notifyTemp2();
}

void loop() {
  static int nextNotifyMs = 0;
  unsigned long curMs = millis();

  if (temp1 > SAFETY_SHUTOFF_TEMP) {
    setMode(EJECT);
  } else {
    switch (activeMode) {
      case PREHEAT:
        if (temp1 <= PREHEAT_TEMP_LOW) {
          setHeatLevel(HEAT_LEVELS);
          setFanLevel(0);
        }
        if (temp1 >= PREHEAT_TEMP_HIGH) {
          setHeatLevel(0);
          setFanLevel(FAN_LEVELS);
        }
        break;
      case EJECT:
        if (curMs - activeModeMs > EJECT_DURATION_MS) {
          setMode(COOL);
        }
        break;
      case COOL:
        if (temp1 <= COOL_TEMP) {
          setMode(STANDBY);
        }
        break;
    }
  }

  cycleHeatState(curMs);

  sampleTemps();
  if (curMs > nextNotifyMs) {
    nextNotifyMs = curMs + NOTIFY_INTERVAL_MS;
    updateTemps();
    notifyMode();
    notifyHeat();
    notifyFan();
  }

  delay(50);
}
