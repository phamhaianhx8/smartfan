#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ================= CẤU HÌNH PHẦN CỨNG =================
const int FAN_PWM_PIN = 2;   // Chân PWM quạt (D2)
const int TEMP_PIN    = 0;   // Chân đọc analog từ LM35 (A0)

const int PWM_FREQ       = 25000; // 25 kHz
const int PWM_RESOLUTION = 8;     // 8-bit (0 - 255)
const int FAN_MIN_DUTY   = 20;    // ~20% Duty Cycle
const int FAN_MAX_DUTY   = 255;   // 100% Duty Cycle

// ================= CẤU HÌNH UUID BLUETOOTH BLE =================
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_NOTIFY_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8" // Gửi dữ liệu lên web
#define CHAR_CONTROL_UUID   "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e" // Nhận lệnh từ web

BLEServer* pServer = NULL;
BLECharacteristic* pNotifyCharacteristic = NULL;
BLECharacteristic* pControlCharacteristic = NULL;
bool deviceConnected = false;

// ================= BIẾN ĐIỀU KHIỂN =================
bool isAutoMode = true;
int manualDutyPercent = 50;
float tempMin = 37.0;
float tempMax = 45.0;
float currentTemp = 0.0;
int currentPwm = FAN_MIN_DUTY;
int currentPercent = 0;

unsigned long lastReadTime = 0;
unsigned long lastBleSend  = 0;

// Callback theo dõi kết nối BLE
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("[BLE] Da ket noi dien thoai!");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("[BLE] Ngat ket noi, dang phat lai quang ba...");
    pServer->startAdvertising(); // Tiếp tục phát tín hiệu để web tìm lại được
  }
};

// Callback nhận lệnh từ Web Bluetooth
class ControlCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();
    if (rxValue.length() > 0) {
      Serial.print("[BLE Nhan lenh]: ");
      Serial.println(rxValue);

      if (rxValue.startsWith("MODE:")) {
        isAutoMode = (rxValue.substring(5) == "1");
      } else if (rxValue.startsWith("MAN:")) {
        manualDutyPercent = rxValue.substring(4).toInt();
      } else if (rxValue.startsWith("AUTO:")) {
        int commaIndex = rxValue.indexOf(',');
        if (commaIndex > 0) {
          tempMin = rxValue.substring(5, commaIndex).toFloat();
          tempMax = rxValue.substring(commaIndex + 1).toFloat();
        }
      }
    }
  }
};

// Đọc nhiệt độ LM35 (lọc trung bình 20 mẫu)
float readTemperature() {
  long sumMilliVolts = 0;
  for (int i = 0; i < 20; i++) {
    sumMilliVolts += analogReadMilliVolts(TEMP_PIN);
    delay(2);
  }
  return (sumMilliVolts / 20.0) / 10.0;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SMART FAN ESP32-C3 (WEB BLUETOOTH) ===");

  pinMode(TEMP_PIN, INPUT);

  // Cấu hình PWM quạt
  ledcAttach(FAN_PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(FAN_PWM_PIN, FAN_MIN_DUTY);

  // Khởi tạo BLE
  BLEDevice::init("ESP32_SmartFan");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Tạo BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Characteristic gửi trạng thái lên điện thoại (Notify)
  pNotifyCharacteristic = pService->createCharacteristic(
                            CHAR_NOTIFY_UUID,
                            BLECharacteristic::PROPERTY_NOTIFY
                          );
  pNotifyCharacteristic->addDescriptor(new BLE2902());

  // Characteristic nhận lệnh điều khiển từ điện thoại (Write)
  pControlCharacteristic = pService->createCharacteristic(
                             CHAR_CONTROL_UUID,
                             BLECharacteristic::PROPERTY_WRITE
                           );
  pControlCharacteristic->setCallbacks(new ControlCallbacks());

  pService->start();

  // Bắt đầu phát sóng BLE
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Dang phat song 'ESP32_SmartFan', san sang ket noi!");
}

void loop() {
  unsigned long now = millis();

  // Task 1: Tính toán tốc độ quạt (Mỗi 500ms)
  if (now - lastReadTime >= 500) {
    lastReadTime = now;
    currentTemp = readTemperature();

    if (isAutoMode) {
      if (currentTemp <= tempMin) {
        currentPwm = FAN_MIN_DUTY;
      } else if (currentTemp >= tempMax) {
        currentPwm = FAN_MAX_DUTY;
      } else {
        currentPwm = map(currentTemp * 10, tempMin * 10, tempMax * 10, FAN_MIN_DUTY, FAN_MAX_DUTY);
      }
    } else {
      if (manualDutyPercent <= 0) currentPwm = 0;
      else currentPwm = map(manualDutyPercent, 0, 100, FAN_MIN_DUTY, FAN_MAX_DUTY);
    }

    ledcWrite(FAN_PWM_PIN, currentPwm);
    currentPercent = map(currentPwm, 0, 255, 0, 100);
  }

  // Task 2: Truyền dữ liệu BLE lên Web (Mỗi 300ms nếu có kết nối)
  if (deviceConnected && (now - lastBleSend >= 300)) {
    lastBleSend = now;
    char buffer[64];
    // Chuỗi JSON: {"t":32.5,"p":45,"m":1}
    snprintf(buffer, sizeof(buffer), "{\"t\":%.1f,\"p\":%d,\"m\":%d}", 
             currentTemp, currentPercent, isAutoMode ? 1 : 0);
    pNotifyCharacteristic->setValue(buffer);
    pNotifyCharacteristic->notify();
  }
}