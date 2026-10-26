#include "esp_camera.h"
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <iostream>
#include <sstream>

// Servo Pin Tanımları
#define PAN_PIN 14
#define TILT_PIN 15

// Donanımsal PWM Kanalları (ESP32 LEDC donanımı - Timer çakışması yapmaz)
const int PAN_CHANNEL  = 2;
const int TILT_CHANNEL = 3;

// Flaş Işık Tanımı
#define LIGHT_PIN 4
const int PWMLightChannel = 4;

// Kamera Pin Tanımlamaları (AI Thinker ESP32-CAM)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

const char* ssid     = "TLSROBOTIK";
const char* password = "12345678";

AsyncWebServer server(80);
AsyncWebSocket wsCamera("/Camera");
AsyncWebSocket wsServoInput("/ServoInput");
uint32_t cameraClientId = 0;

// SG90 Servo Açı Fonksiyonu (50Hz - 16 Bit LEDC)
void setServoAngle(int channel, int angle) 
{
  angle = constrain(angle, 0, 180);
  // 50Hz periyotta (20ms): 0 derece ~500us (1638 duty), 180 derece ~2400us (7864 duty)
  uint32_t duty = map(angle, 0, 180, 1638, 7864);
  ledcWrite(channel, duty);
}

const char* htmlHomePage PROGMEM = R"HTMLHOMEPAGE(
<!DOCTYPE html>
<html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
    <style>
      .noselect {
        -webkit-touch-callout: none;
        -webkit-user-select: none;
        user-select: none;
      }
      .slidecontainer { width: 100%; }
      .slider {
        -webkit-appearance: none;
        width: 100%;
        height: 22px;
        border-radius: 5px;
        background: #d3d3d3;
        outline: none;
        opacity: 0.8;
      }
      .slider::-webkit-slider-thumb {
        -webkit-appearance: none;
        width: 36px;
        height: 36px;
        border-radius: 50%;
        background: #007bff;
        cursor: pointer;
      }
    </style>
  </head>
  <body class="noselect" align="center" style="background-color:#f4f4f4;font-family:sans-serif;">
    <h3 style="color:#333;margin-top:10px;">TLS ROBOTIK PAN-TILT</h3>
    <table id="mainTable" style="width:100%;max-width:400px;margin:auto;table-layout:fixed" CELLSPACING=10>
      <tr>
        <td colspan=3 align="center">
          <img id="cameraImage" src="" style="width:100%;max-width:360px;height:auto;border-radius:8px;background:#000;">
        </td>
      </tr> 
      <tr>
        <td style="text-align:left;width:60px;"><b>Pan:</b></td>
        <td colspan=2>
          <div class="slidecontainer">
            <input type="range" min="0" max="180" value="90" class="slider" id="Pan" oninput='sendButtonInput("Pan",value)'>
          </div>
        </td>
      </tr> 
      <tr>
        <td style="text-align:left;width:60px;"><b>Tilt:</b></td>
        <td colspan=2>
          <div class="slidecontainer">
            <input type="range" min="0" max="180" value="90" class="slider" id="Tilt" oninput='sendButtonInput("Tilt",value)'>
          </div>
        </td>   
      </tr>
      <tr>
        <td style="text-align:left;width:60px;"><b>Light:</b></td>
        <td colspan=2>
          <div class="slidecontainer">
            <input type="range" min="0" max="255" value="0" class="slider" id="Light" oninput='sendButtonInput("Light",value)'>
          </div>
        </td>   
      </tr>      
    </table>
  
    <script>
      var webSocketCameraUrl = "ws:\/\/" + window.location.hostname + "/Camera";
      var webSocketServoInputUrl = "ws:\/\/" + window.location.hostname + "/ServoInput";      
      var websocketCamera;
      var websocketServoInput;
      var currentBlobUrl = null;
      
      function initCameraWebSocket() 
      {
        websocketCamera = new WebSocket(webSocketCameraUrl);
        websocketCamera.binaryType = 'blob';
        websocketCamera.onclose = function(event){ setTimeout(initCameraWebSocket, 1500); };
        websocketCamera.onmessage = function(event)
        {
          var imageId = document.getElementById("cameraImage");
          if (currentBlobUrl) {
            URL.revokeObjectURL(currentBlobUrl);
          }
          currentBlobUrl = URL.createObjectURL(event.data);
          imageId.src = currentBlobUrl;
        };
      }
      
      function initServoInputWebSocket() 
      {
        websocketServoInput = new WebSocket(webSocketServoInputUrl);
        websocketServoInput.onopen = function(event)
        {
          sendButtonInput("Pan", document.getElementById("Pan").value);
          sendButtonInput("Tilt", document.getElementById("Tilt").value);
          sendButtonInput("Light", document.getElementById("Light").value);          
        };
        websocketServoInput.onclose = function(event){ setTimeout(initServoInputWebSocket, 1500); };
      }
      
      function initWebSocket() 
      {
        initCameraWebSocket();
        initServoInputWebSocket();
      }

      function sendButtonInput(key, value) 
      {
        if (websocketServoInput && websocketServoInput.readyState === WebSocket.OPEN) {
          websocketServoInput.send(key + "," + value);
        }
      }
    
      window.onload = initWebSocket;
    </script>
  </body>    
</html>
)HTMLHOMEPAGE";

void handleRoot(AsyncWebServerRequest *request) 
{
  request->send_P(200, "text/html", htmlHomePage);
}

void handleNotFound(AsyncWebServerRequest *request) 
{
  request->send(404, "text/plain", "Not Found");
}

void onServoInputWebSocketEvent(AsyncWebSocket *server, 
                                AsyncWebSocketClient *client, 
                                AwsEventType type,
                                void *arg, 
                                uint8_t *data, 
                                size_t len) 
{                      
  switch (type) 
  {
    case WS_EVT_DISCONNECT:
      setServoAngle(PAN_CHANNEL, 90);
      setServoAngle(TILT_CHANNEL, 90);
      ledcWrite(PWMLightChannel, 0);
      break;
    case WS_EVT_DATA:
    {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) 
      {
        std::string myData((char *)data, len);
        std::istringstream ss(myData);
        std::string key, value;
        std::getline(ss, key, ',');
        std::getline(ss, value, ',');
        if (!value.empty())
        {
          int val = atoi(value.c_str());
          if (key == "Pan") {
            setServoAngle(PAN_CHANNEL, val);
          } else if (key == "Tilt") {
            setServoAngle(TILT_CHANNEL, val);   
          } else if (key == "Light") {
            ledcWrite(PWMLightChannel, val);         
          }           
        }
      }
      break;
    }
    default:
      break;  
  }
}

void onCameraWebSocketEvent(AsyncWebSocket *server, 
                            AsyncWebSocketClient *client, 
                            AwsEventType type,
                            void *arg, 
                            uint8_t *data, 
                            size_t len) 
{                      
  switch (type) 
  {
    case WS_EVT_CONNECT:
      cameraClientId = client->id();
      break;
    case WS_EVT_DISCONNECT:
      if (cameraClientId == client->id()) {
        cameraClientId = 0;
      }
      break;
    default:
      break;  
  }
}

void setupCamera()
{
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // QVGA (320x240): WebSocket kuyruğunu şişirmez, akıcı anlık görüntü sağlar
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
  }
}

void sendCameraPicture()
{
  if (cameraClientId == 0) return;
  
  AsyncWebSocketClient * client = wsCamera.client(cameraClientId);
  if (!client || client->queueIsFull()) return;

  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) return;

  wsCamera.binary(cameraClientId, fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void setUpHardware()
{
  // Pan Servosu (IO14 -> Kanal 2, 50Hz, 16-bit)
  ledcSetup(PAN_CHANNEL, 50, 16);
  ledcAttachPin(PAN_PIN, PAN_CHANNEL);

  // Tilt Servosu (IO15 -> Kanal 3, 50Hz, 16-bit)
  ledcSetup(TILT_CHANNEL, 50, 16);
  ledcAttachPin(TILT_PIN, TILT_CHANNEL);

  // Başlangıçta servoları 90 dereceye al
  setServoAngle(PAN_CHANNEL, 90);
  setServoAngle(TILT_CHANNEL, 90);

  // Flaş LED (IO4 -> Kanal 4, 1000Hz, 8-bit)
  ledcSetup(PWMLightChannel, 1000, 8);
  pinMode(LIGHT_PIN, OUTPUT);    
  ledcAttachPin(LIGHT_PIN, PWMLightChannel);
  ledcWrite(PWMLightChannel, 0);
}

void setup(void) 
{
  Serial.begin(115200);

  setupCamera();
  setUpHardware();

  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.onNotFound(handleNotFound);
      
  wsCamera.onEvent(onCameraWebSocketEvent);
  server.addHandler(&wsCamera);

  wsServoInput.onEvent(onServoInputWebSocketEvent);
  server.addHandler(&wsServoInput);

  server.begin();
  Serial.println("Server baslatildi");
}

void loop() 
{
  wsCamera.cleanupClients(); 
  wsServoInput.cleanupClients(); 
  sendCameraPicture(); 
  delay(30); 
}