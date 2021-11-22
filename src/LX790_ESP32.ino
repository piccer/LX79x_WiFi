#include <Arduino.h>
#include <string.h>
#include <Wire.h>
//#include "CRC16.h"
#include "CRC.h"
#include "ahWireSlave.h"
#include "LX790_util.h"
#include <WiFi.h>
#include <WebServer.h>
#include "SPIFFS.h"
#include <Update.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

//Hardware  
#define SDA_PIN_MAINBOARD    33  /*default 21*/
#define SCL_PIN_MAINBOARD    25  /*default 22*/
#define SDA_PIN_DISPLAY      26
#define SCL_PIN_DISPLAY      27
#define I2C_SLAVE_ADDR        0x27
#define I2C_DISPLAY_ADDR      0x27
#define OUT_IO               13

//I2C commands
#define TYPE_BUTTONS          0x01
#define LEN_BUTTONS_RES       9
#define TYPE_DISPLAY          0x02
#define LEN_BUTTONS_REQ       4
#define LEN_DISPLAY_RES       9
#define TYPE_UNKNOWN          0x04
#define LEN_UNKNOWN_REQ       4
#define TYPE_UNKNOWN_INIT     0x05
#define LEN_UNKNOWN_INIT_REQ  5
#define LEN_MAINBOARD_MAX     9
#define LEN_CMDQUE           10

//Buttons
#define BTN_BYTE1_OK          0x01
#define BTN_BYTE1_START       0x02
#define BTN_BYTE1_HOME        0x04
#define BTN_BYTE2_STOP        0xFC

const char* ssid     = "ssid";
const char* password = "pass";
const char* hostname = "LX790";
//const char* TriggerURL = "http://FHEM/fhem?cmd=set%20ROBBI%20reread&XHR=1";
const char* TriggerURL = "";
const char* mqtt_server = "192.168.x.x";
const char* MQTT_ID = "LX790";
const char* DisplayTopic = "Display";
const char* rssiTopic = "RSSI";
const char* StatusTopic = "Statustext";
const char* batteryTopic = "battery";
const char* inTopic = "inTopic";


  WiFiClient espClient;
  PubSubClient client(espClient); //lib required for mqtt
unsigned long OFF_time = 0;
  
WebServer server1(80);

WebServer *pserver[] = { &server1, nullptr };

TaskHandle_t hTask0;   //Hardware: I2C, WiFi...
TaskHandle_t hTask1;   //Web...
SemaphoreHandle_t SemMutex;

const char* Buttons[] = {"io", "start", "home", "ok", "stop", nullptr};

static struct
{
  char    WebOutDisplay[100];
  char    AktDisplay[4+1];
  char    OldDisplay[4+1];
  unsigned long WebInButtonTime[5 /* io, start, home, ok, stop */];
  int     WebInButtonState[5 /* io, start, home, ok, stop */];
  int     Lst_err;
  int     Cnt_err;
  int     Cnt_timeout;
  char    point;
  int     aktualisieren;
  int     bat;
  int     batCharge;
  char    battery[8];
  char    Statustext[100];
  struct
  {
    unsigned long T_start;
    uint8_t WebInButton[2 /*byte 1 + byte 2*/] = {0};
    unsigned long T_end;
  }cmdQue[LEN_CMDQUE];
  int cmdQueIdx;
  int LstProcesedcmdQueIdx;
} thExchange;

void Task0( void * pvParameters );
void Task1( void * pvParameters );

void setup()
{
  Serial.begin(115200);
  while (!Serial);
  Serial.print  (F("Build date: "));
  Serial.print  (__DATE__);
  Serial.print  (F(" time: "));
  Serial.println(__TIME__);

  SemMutex = xSemaphoreCreateMutex();
  if (SemMutex == NULL)
    Serial.println(F("init semaphore error"));

  if(!SPIFFS.begin(true))
    Serial.println(F("init SPIFFS error"));

  memset(&thExchange, 0, sizeof thExchange);

  xTaskCreatePinnedToCore(
    Task0,   /* Function to implement the task -> I2C, WiFi*/
    "Task0", /* Name of the task */
    10000,   /* Stack size in words */
    NULL,    /* Task input parameter */
    1,       /* Priority of the task 0 -> lowest*/
    &hTask0, /* Task handle. */
    0);      /* Core where the task should run */

  delay(500);

  xTaskCreatePinnedToCore(
    Task1,   /* Function to implement the task -> Webserver*/
    "Task1", /* Name of the task */
    10000,   /* Stack size in words */
    NULL,    /* Task input parameter */
    1,       /* Priority of the task 0 -> lowest */
    &hTask1, /* Task handle. */
    1);      /* Core where the task should run */
  //mqtt:
 if (strstr (mqtt_server,".") != NULL)
 {
        client.setServer(mqtt_server, 1883);//connecting to mqtt server
        client.setCallback(callback);
      //delay(5000);
        connectmqtt();
 }
  //mqtt ende
}

void publishSerialData(char *serialData){
  if (!client.connected()) {
    reconnect();
  }
  client.publish(StatusTopic, serialData);
}

char * GetStatustext (void)
{
  static char statustxt[100] = "";
  
  memset (statustxt, 0, sizeof statustxt);
  
  if (thExchange.cmdQueIdx)
  {
    sprintf(statustxt, "bitte warten...");
  }
  else
  {
    if (thExchange.batCharge)
    {
      strcpy(statustxt, "Laden...");  
    }
    else
    {
      strcpy(statustxt, DecodeMsg (thExchange.AktDisplay));  
    }
  }
  return statustxt; 
}

// Request:   http://MOWERADRESS/status
// Response:  [rssi dbm];[Cnt_timeout];[Cnt_err];[LstError];
void Web_aktStatusWeb(WebServer *svr)
{
  char out[400] = "";
  long rssi = WiFi.RSSI();
 
 //[cnt];[Display];[point];[lock];[clock];[bat];[rssi dbm];[Cnt_timeout];[Cnt_err];[LstError];[text]
  xSemaphoreTake(SemMutex, 1);
  
  sprintf(out, "%s;%ld;%d;%d;%d;%s", 
    thExchange.WebOutDisplay, 
    rssi, 
    thExchange.Cnt_timeout, 
    thExchange.Cnt_err, 
    thExchange.Lst_err,
    GetStatustext());
    
  xSemaphoreGive(SemMutex);

  svr->send(200,"text/html", out);
}

// Request:   http://MOWERADRESS/statval
// Response:  [Display];[rssi dbm];[battery];[text]
void Web_aktStatusValues(WebServer *svr)
{
  char out[200] = "";
  long rssi = WiFi.RSSI();
  const char* BatState[] = {"off", "empty", "low", "mid", "full", "charging"};
  int IdxBatState = 0;
  char point[2] = "";
  
  xSemaphoreTake(SemMutex, 1);
  if (thExchange.batCharge)
    IdxBatState = 5; /*charging*/
  else
    IdxBatState = thExchange.bat+1;
  
  if (thExchange.point != ' ')
    sprintf(point, "%c", thExchange.point);
  sprintf(out, "%c%c%s%c%c;%ld;%s;%s",
          thExchange.AktDisplay[0],
          thExchange.AktDisplay[1],
          point,
          thExchange.AktDisplay[2],
          thExchange.AktDisplay[3],
          rssi,
          BatState[IdxBatState],
          GetStatustext()
          );
  
  xSemaphoreGive(SemMutex);

  svr->send(200,"text/plain", out);
}

//Webcommand examples: 
// Send command:     http://MOWERADRESS/cmd?parm=start&value=1
// Get Akt Status:   http://MOWERADRESS/cmd?mowerstatus=0
// Get Status Text:  http://MOWERADRESS/cmd?mowerstatus=-E6-
void Web_getCmd(WebServer *svr)
{
  if (svr->argName(0) == "parm" &&
      svr->argName(1) == "value")
  {
    int i = 0;
    int val = svr->arg(1).toInt();
    
    xSemaphoreTake(SemMutex, 1);

    if (thExchange.cmdQueIdx)
    {
      svr->send(500, "text/plain", "busy...");
      return;
    }

    if (svr->arg(0) == "workzone" && val > 0)
    {
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+3500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_OK;
      thExchange.cmdQueIdx++;
    }
    else if (svr->arg(0) == "timedate" && val > 0)
    {
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+3500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_START;
      thExchange.cmdQueIdx++;
    }
    else if (svr->arg(0) == "startmow" && val > 0)
    {
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+200;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_START;
      thExchange.cmdQueIdx++;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis()+300;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_OK;
      thExchange.cmdQueIdx++;
    }
    else if (svr->arg(0) == "homemow" && val > 0)
    {
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+200;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_HOME;
      thExchange.cmdQueIdx++;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis()+300;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_OK;
      thExchange.cmdQueIdx++;
    }
    else
    {
      for (i=0; Buttons[i]; i++)
      {
        if (svr->arg(0) == Buttons[i])
        {
          if (i==0)
          {
            digitalWrite(OUT_IO, val?LOW:HIGH);
          }
          thExchange.WebInButtonState[i] = val > 0;
          
          if (thExchange.WebInButtonState[i])
          {
            thExchange.WebInButtonTime[i] = val + millis();
          }        
          break;
        }
      }
    }
    thExchange.Cnt_timeout = 0;      
    xSemaphoreGive(SemMutex);
  }
  else
  {
    svr->send(500, "text/plain", "invalid parameter(s)");
    return;
  }

  svr->send(200,"text/plain", "ok");
}

void Task1( void * pvParameters )
{
  int s = 0;

  for (s=0; pserver[s]; s++)
  {
    const char * pngs[] = { 
      "/robomower.png", 
      "/bat_empty.png" ,"/bat_low.png" ,"/bat_mid.png" ,"/bat_full.png",
      "/unlocked.png" ,"/locked.png" ,"/clock.png", 
      nullptr };
    int p = 0;
    
    for (p=0; pngs[p]; p++)
    {
      pserver[s]->on(pngs[p], [=]()
      {
        File dat = SPIFFS.open(pngs[p], "r");
        if (dat) 
        {
          pserver[s]->send(200, "image/png", dat.readString());
          dat.close();
        }
      });
    }
      
    pserver[s]->on("/", [=]()
    {
      File html = SPIFFS.open("/index.html", "r");
      if (html)
      {
        pserver[s]->send(200, "text/html", html.readString());
        html.close();
      }
    });
    pserver[s]->on("/update", HTTP_GET, [=]()
    {
      File html = SPIFFS.open("/update.html", "r");
      if (html)
      {
        pserver[s]->sendHeader("Connection", "close");
        pserver[s]->send(200, "text/html", html.readString());
        html.close();
      }
    });
    pserver[s]->on("/execupdate", HTTP_POST, [=]() 
    {
      pserver[s]->sendHeader("Connection", "close");
      pserver[s]->send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
      ESP.restart();
    }, [=]() 
    {
      HTTPUpload& upload = pserver[s]->upload();
      if (upload.status == UPLOAD_FILE_START) 
      {
        Serial.printf("Update: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) 
        { //start with max available size
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) 
      {
        /* flashing firmware to ESP*/
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) 
        {
          Update.printError(Serial);
        }
      } 
      else if (upload.status == UPLOAD_FILE_END) 
      {
        if (Update.end(true)) 
        { //true to set the size to the current progress
          Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
        } 
        else 
        {
          Update.printError(Serial);
        }
      }
    });    
    
    pserver[s]->on("/cmd",    HTTP_GET, [=]() {Web_getCmd(pserver[s]);});
    pserver[s]->on("/web",              [=]() {Web_aktStatusWeb(pserver[s]);});
    pserver[s]->on("/statval",          [=]() {Web_aktStatusValues(pserver[s]);});
  
    pserver[s]->begin();
    //pserver[s]->sendHeader("charset", "utf-8");
  }

  while(1)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      for (s=0; pserver[s]; s++)
      {
        pserver[s]->handleClient();
      }
      //hier könnte MQTT client aufgerufen werden...
      if (strstr (mqtt_server,".") != NULL)
      {
        client.loop();
        if (Serial.available() > 0) 
        {
          char mun[501];
          memset(mun,0, 501);
          Serial.readBytesUntil( '\n',mun,500);
          publishSerialData(mun);
        }
      }
      publishdata();
    }
    delay(10);
  }
}

void Task0( void * pvParameters )
{
  TwoWireSlave WireSlave  = TwoWireSlave(0); //ESP32 <-> Motherboard
  TwoWire      WireMaster = TwoWire(1);      //ESP32 <-> Display/Buttons
  int WiFi_WasConnected = 0;
  unsigned long Lst_WiFi_Status = 0;
  //CRC16 crc;
  int i = 0;
  int ret = 0;
  unsigned long Lst_ButtonReqFromMainboard = 0;
  uint8_t DatReadBuff[LEN_MAINBOARD_MAX*5] = {0};
  int IdxReadBuff = 0;
  int ReadBuff_Processed = 0;
  uint8_t DatMainboard[LEN_MAINBOARD_MAX] = {0};
  uint8_t Lst_DatMainboard[LEN_MAINBOARD_MAX] = {0};
  uint8_t DisplayRes[LEN_DISPLAY_RES] = {0x01, 0x00, 0x78, 0x00, 0x00, 0x00, 0x00, 0xFB, 0xA9};

  pinMode(OUT_IO, OUTPUT);
  digitalWrite(OUT_IO, HIGH);

  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(hostname);
  WiFi.begin(ssid, password);
  Lst_WiFi_Status = millis();

  //I2C Slave - Motherboard
  ret = WireSlave.begin(SDA_PIN_MAINBOARD, SCL_PIN_MAINBOARD, I2C_SLAVE_ADDR);
  if (!ret)
  {
    Serial.println(F("I2C slave init failed"));
    while(1);
  }
  WireMaster.begin(SDA_PIN_DISPLAY, SCL_PIN_DISPLAY, 100000);

  //crc.setPolynome(0x1021);
  //crc.setStartXOR(0xFFFF);
  //crc.setEndXOR(0xFFFF);

  while(1)
  {
    int err = 0;

    //check WLAN state
    if (WiFi_WasConnected)
    {
      if (millis() - Lst_WiFi_Status > 10000)
      {
        Lst_WiFi_Status = millis();

        if (WiFi.status() != WL_CONNECTED)
        {
          Serial.println(F("WLAN reconnect.."));
          WiFi.disconnect();
          WiFi.begin(ssid, password);
        }
      }
    }
    else
    {
      if (WiFi.status() == WL_CONNECTED)
      {
        WiFi_WasConnected = 1;
        Serial.print  (F("WiFi successfully connected with IP: "));        
        Serial.println(WiFi.localIP());

      }
      else if (millis() - Lst_WiFi_Status > 1000)
      {
        Lst_WiFi_Status = millis();
        Serial.println(F("WiFi connect.."));
      }
    }
    
    //get state/response from buttons
    while (1)
    {
      if (WireMaster.requestFrom(I2C_DISPLAY_ADDR, LEN_DISPLAY_RES) != LEN_DISPLAY_RES)
        break;
      ret = WireMaster.available();
      if (!ret)
        break;

      if (ret != LEN_DISPLAY_RES)
      {
        WireMaster.flush();
        Serial.print  (F("should never see me - L: "));
        Serial.println(__LINE__);
        break;
      }

      i = 0;
      while (WireMaster.available())
        DisplayRes[i++] = WireMaster.read();

      break;
    }

    //read Mainboard data
    while (1)
    {
      memset(DatMainboard, 0, sizeof DatMainboard);

      ret = WireSlave.read_buff(&DatReadBuff[IdxReadBuff], (sizeof DatReadBuff)-IdxReadBuff);

      if (ret < 0)
      {
        err = __LINE__;
        break; //err driver
      }

      IdxReadBuff += ret;

      if ((DatReadBuff[0] == TYPE_BUTTONS) && (IdxReadBuff >= LEN_BUTTONS_REQ))
      {
        static uint8_t Req[] = {0x01, 0x01, 0xE0, 0xC1};
        if(!memcmp(DatReadBuff, Req, sizeof Req))
        {
          memcpy(DatMainboard, DatReadBuff, LEN_BUTTONS_REQ);
          ReadBuff_Processed = LEN_BUTTONS_REQ;
        }
      }
      else if ((DatReadBuff[0] == TYPE_UNKNOWN) && (IdxReadBuff >= LEN_UNKNOWN_REQ))
      {
        static uint8_t Req[] = {0x04, 0x01, 0x15, 0x3E};
        if(!memcmp(DatReadBuff, Req, sizeof Req))
        {
          memcpy(DatMainboard, DatReadBuff, LEN_UNKNOWN_REQ);
          ReadBuff_Processed = LEN_UNKNOWN_REQ;
        }
      }
      else if ((DatReadBuff[0] == TYPE_UNKNOWN_INIT) && (IdxReadBuff >= LEN_UNKNOWN_INIT_REQ))
      {
        static uint8_t Req[] = {0x05, 0x01, 0x01, 0x83, 0xfb};
        if(!memcmp(DatReadBuff, Req, sizeof Req))
        {
          memcpy(DatMainboard, DatReadBuff, LEN_UNKNOWN_INIT_REQ);
          ReadBuff_Processed = LEN_UNKNOWN_INIT_REQ;
        }
      }
      else if (DatReadBuff[0] == TYPE_DISPLAY && IdxReadBuff >= LEN_DISPLAY_RES)
      {
        uint16_t calc_crc = 0xFFFF;
        uint16_t msg_crc = 0x0000;

        //crc.restart();
        //crc.add(&DatReadBuff[ReadBuff_Processed], LEN_DISPLAY_RES-2); -> yield() -> error !! -> ESP32 bug ??
        //calc_crc = crc.getCRC();
        calc_crc = crc16(DatReadBuff, LEN_DISPLAY_RES-2, 0x1021, 0xFFFF, 0xFFFF, false, false);

        msg_crc |= (DatReadBuff[LEN_DISPLAY_RES-2]);
        msg_crc |= (DatReadBuff[LEN_DISPLAY_RES-1])<<8;
        if (calc_crc != msg_crc)
        {
          err = __LINE__;
          break; //invalid crc
        }
        else
        {
          if (WiFi.status() == WL_CONNECTED)
          {
            DatReadBuff[5] |= 0x10;  //WiFi Symbol
          
            calc_crc = crc16(DatReadBuff, LEN_DISPLAY_RES-2, 0x1021, 0xFFFF, 0xFFFF, false, false);
            DatReadBuff[LEN_DISPLAY_RES-2] = calc_crc & 0xff;
            DatReadBuff[LEN_DISPLAY_RES-1] = calc_crc>>8;
          }

          memcpy(DatMainboard, DatReadBuff, LEN_DISPLAY_RES);
          ReadBuff_Processed = LEN_DISPLAY_RES;
        }
      }

      if (ReadBuff_Processed)
      {
        if (ret == 0)
          delay(5);
      
        memcpy(DatReadBuff, &DatReadBuff[ReadBuff_Processed], (sizeof DatReadBuff)-ReadBuff_Processed);
        IdxReadBuff -= ReadBuff_Processed;
      }
      else
      {
        if (IdxReadBuff >= LEN_MAINBOARD_MAX*3)
        {
          err = __LINE__;
          break; //no match
        }
      }
      ReadBuff_Processed = 0;

      break;
    }

    //valid data from mainboard?
    if (err)
    {      
     #if 1 //Debug print
      {
        int i = 0;
        char hex[2] = {0};
        char buff[(sizeof DatReadBuff)*2 + 1] = {0};

        Serial.print  ("Err Slave MB ret: ");
        Serial.print  (ret, DEC);
        Serial.print  (" err: ");
        Serial.print  (err, DEC);
        Serial.print  (" Data Read: ");
        memset(buff, 0, sizeof buff);
        for (i=0; i<(sizeof DatReadBuff); i++)
        {
          sprintf(hex, "%02x", DatReadBuff[i]);
          strcat(buff, hex);
        }
        Serial.print(buff);

        Serial.print (" Data MB: ");
        memset(buff, 0, sizeof buff);
        for (i=0; i<(sizeof DatMainboard); i++)
        {
          sprintf(hex, "%02x", DatMainboard[i]);
          strcat(buff, hex);
        }
        Serial.print(buff);
        Serial.println(" ");
      }
     #endif

      xSemaphoreTake(SemMutex, 1);
      thExchange.Lst_err = err;
      thExchange.Cnt_err++;
      xSemaphoreGive(SemMutex);
      WireSlave.flush();
      IdxReadBuff = ReadBuff_Processed = 0;
      memset(DatReadBuff, 0, sizeof DatReadBuff);
    }

    if (DatMainboard[0])
    {
      Lst_ButtonReqFromMainboard = millis();
      //send request to read buttons
      //if (DatMainboard[0] == TYPE_BUTTONS || DatMainboard[0] == TYPE_UNKNOWN)
      {
        WireMaster.beginTransmission(I2C_DISPLAY_ADDR);
        WireMaster.write(DatMainboard, LEN_BUTTONS_REQ);
        WireMaster.endTransmission(true);
      }
    }
    //Timeout or off?
    if (millis() - Lst_ButtonReqFromMainboard > 100)
    {
      xSemaphoreTake(SemMutex, 1);
      thExchange.Cnt_timeout++;
      memset(&thExchange.WebInButtonTime, 0, sizeof thExchange.WebInButtonTime);
      memset(&thExchange.WebInButtonState, 0, sizeof thExchange.WebInButtonState);
      if (thExchange.cmdQueIdx)
      { 
        memset(thExchange.cmdQue, 0, sizeof thExchange.cmdQue);
        thExchange.cmdQueIdx = 0;
      }
      xSemaphoreGive(SemMutex);
      Lst_ButtonReqFromMainboard = millis();

      WireMaster.flush();
     #if 1 //Debug print
      {
        int i = 0;
        char hex[2] = {0};
        char buff[(sizeof DatReadBuff)*2 + 1] = {0};
        int NotEmpty = 0;

        memset(buff, 0, sizeof buff);
        for (i=0; i<(sizeof DatReadBuff); i++)
        {
          sprintf(hex, "%02x", DatReadBuff[i]);
          strcat(buff, hex);
          if (DatReadBuff[i])
            NotEmpty = 1;
        }
        if (NotEmpty)
        {
          Serial.print  (" To read: ");
          Serial.print  (IdxReadBuff);
          Serial.print  (" proc: ");
          Serial.print  (ReadBuff_Processed);
          Serial.print  (" dat: ");
          Serial.println(buff);
        }
      }
     #endif

      WireSlave.flush();
      IdxReadBuff = ReadBuff_Processed = 0;
      memset(DatReadBuff, 0, sizeof DatReadBuff);
      
      DatMainboard[0] = TYPE_DISPLAY; //force for web counter
    }

    if (DatMainboard[0] == TYPE_BUTTONS)
    {
      //Inject
      int t = 0;
      uint8_t WebInButton[2 /*byte 1 + byte 2*/] = {0};
      static int LstProcesedcmdQueIdx = 0;
      
      memset(WebInButton, 0, sizeof WebInButton);
      
      xSemaphoreTake(SemMutex, 1);
      
      //Buttons/Actions from que
      if (!thExchange.cmdQueIdx)
        LstProcesedcmdQueIdx = 0;
      while (thExchange.cmdQueIdx)
      {
        unsigned long AktTime = millis();

        if (thExchange.cmdQue[LstProcesedcmdQueIdx].T_end < AktTime)
        {
          LstProcesedcmdQueIdx++;
        }
        if ( (thExchange.cmdQueIdx >= LEN_CMDQUE) ||
            (!thExchange.cmdQue[LstProcesedcmdQueIdx].WebInButton[0] &&
             !thExchange.cmdQue[LstProcesedcmdQueIdx].WebInButton[1]))
        {
          memset(thExchange.cmdQue, 0, sizeof thExchange.cmdQue);
          thExchange.cmdQueIdx = 0;
          break;
        }
        if (thExchange.cmdQue[LstProcesedcmdQueIdx].T_start < AktTime)
        {
          WebInButton[0] = thExchange.cmdQue[LstProcesedcmdQueIdx].WebInButton[0];
          WebInButton[1] = thExchange.cmdQue[LstProcesedcmdQueIdx].WebInButton[1];
        }
        
        break;
      }
      
      //Buttons from web
      for (t=0; Buttons[t]; t++)
      {
        if ( thExchange.WebInButtonState[t] ||
             thExchange.WebInButtonTime[t] > millis() )
        {
          if (t == 0 /*"io"*/)
            WebInButton[0] |= 0;
          if (t == 1 /*"start"*/)
            WebInButton[0] |= BTN_BYTE1_START;
          if (t == 2 /*"home"*/)
            WebInButton[0] |= BTN_BYTE1_HOME;
          if (t == 3 /*"ok"*/)
            WebInButton[0] |= BTN_BYTE1_OK;
          if (t == 4 /*"stop"*/)
            WebInButton[1] |= BTN_BYTE2_STOP;
        }
        else
        {
          thExchange.WebInButtonTime[t] = 0;
        }
      }
      xSemaphoreGive(SemMutex);
            
      if (WebInButton[0] || WebInButton[1])
      {
        uint16_t calc_crc = 0xFFFF;
        
        //01 02 78 00 00 00 00 BB 22
        //|| || -- Button: stop
        //|| -- Buttons: home, start, ok
        //-- Type

        if (WebInButton[0])
          DisplayRes[1] = WebInButton[0];
        if (WebInButton[1])
          DisplayRes[2] = WebInButton[1];

        //crc.restart();
        //crc.add(DisplayRes, LEN_DISPLAY_RES-2); -> yield() -> error !! -> ESP32 bug ??
        //calc_crc = crc.getCRC();
        calc_crc = crc16(DisplayRes, LEN_DISPLAY_RES-2, 0x1021, 0xFFFF, 0xFFFF, false, false);

        DisplayRes[LEN_DISPLAY_RES-2] = calc_crc & 0xff;
        DisplayRes[LEN_DISPLAY_RES-1] = calc_crc>>8;
      }

      ret = WireSlave.write_buff(DisplayRes, LEN_DISPLAY_RES);
      if (ret < 0)
      {
        Serial.print  (F("Ret write "));
        Serial.println(ret, DEC);
      }
    }
    else if (DatMainboard[0] == TYPE_DISPLAY)
    {      
      if (DatMainboard[1]||DatMainboard[5]) //valid or just forced from timeout
      {
        WireMaster.beginTransmission(I2C_DISPLAY_ADDR);
        WireMaster.write(DatMainboard, LEN_DISPLAY_RES);
        WireMaster.endTransmission(true);
      }

      if (memcmp(Lst_DatMainboard, DatMainboard, sizeof Lst_DatMainboard))
      {
        static unsigned int CntWebOut = 0;
        static int Lst_bat = -1;
        static unsigned long Lst_low_is_off = 0;
        static unsigned long Lst_bat_charge = 0;
        uint8_t batraw = 0;
        int bat = -1;
        int batWeb = -1;

        memcpy(Lst_DatMainboard, DatMainboard, sizeof Lst_DatMainboard);
        CntWebOut++;
        
        //battery
        batraw = DatMainboard[5] & 0xE0;
        
        if (!(DatMainboard[5] & 0x40))  //"low" - without battery case
          Lst_low_is_off = millis();
        
        if (DatMainboard[6] & 0x01)
          bat = 3;    //"full"
        else if (batraw == 0xE0)
          bat = 2;    //"mid"
        else if (batraw == 0x60)
          bat = 1;    //"low"
        else if (batraw == 0x20)
          bat = 0;    //"empty"
        
        batWeb = bat;
        if ((batraw == 0x60) && (millis() - Lst_low_is_off > 1000))
          batWeb = 1;    //"low"
        else if ((batraw == 0x20) || (batraw == 0x60) /*blink*/)
          batWeb = 0;    //"empty"
        
        //battery charging ?
        if (Lst_bat != bat)
        {
          if (bat > 1 &&
              Lst_bat < bat)   
          {
            Lst_bat_charge = millis();
          }

          Lst_bat = bat;      
        }
        
        xSemaphoreTake(SemMutex, 1);
        
        thExchange.batCharge = millis() - Lst_bat_charge < 1000;

        thExchange.point = ' ';
        if (DatMainboard[5] & 0x02)
          thExchange.point = ':';
        else if (DatMainboard[5] & 0x01)
          thExchange.point = '.';

        sprintf(thExchange.AktDisplay, "%c%c%c%c",
          DecodeChar (DatMainboard[1]),
          DecodeChar (DatMainboard[2]),
          DecodeChar (DatMainboard[3]),
          DecodeChar (DatMainboard[4]));


        if (DecodeChars_IsRun(&DatMainboard[1]))
          strcpy(thExchange.AktDisplay, "|~~|");
        else if (DecodeChars_IsRunReady(&DatMainboard[1]))
          strcpy(thExchange.AktDisplay, "|ok|");
        
        if (thExchange.batCharge && thExchange.AktDisplay[0] == 0) //hier stimmt noch was nicht, Robi springt beim Ausschalten auf Chrg???
          strcpy(thExchange.AktDisplay, "Chrg");

//piccer: Display beruhigen:
        strcpy(thExchange.AktDisplay, LetterOrNumber(thExchange.AktDisplay));

        if(strcmp(thExchange.AktDisplay, thExchange.OldDisplay) != 0 && strcmp(thExchange.OldDisplay, " OFF") !=0)
        {
          thExchange.aktualisieren = 1;
        }
        if (strcmp(thExchange.OldDisplay, " OFF") == 0 && millis() > OFF_time + 2000)
        { //Robi aus, ESP nach 2 Sekunden noch an -> wir stehen in der Station...
          strcpy(thExchange.AktDisplay, "Chrg");
          strcpy(thExchange.OldDisplay, "");
          thExchange.aktualisieren = 1;
        }

        //cnt;seg1;seg2;seg3;seg4;point;lock;clock;bat
        memset(thExchange.WebOutDisplay, 0, sizeof thExchange.WebOutDisplay);
        sprintf(thExchange.WebOutDisplay, "%d;%c%c%c%c;%c;%d;%d;%d",
          CntWebOut,
          thExchange.AktDisplay[0],
          thExchange.AktDisplay[1],
          thExchange.AktDisplay[2],
          thExchange.AktDisplay[3],
          thExchange.point,
          (DatMainboard[5] & 0x08)?1:0,  //lock
          (DatMainboard[5] & 0x04)?1:0,  //clock
          bat);
          
        thExchange.bat = batWeb;

        xSemaphoreGive(SemMutex);
        //Serial.println(thExchange.WebOutDisplay);
      }
    }
    delay(1);
  }
}

void callback(char* topic, byte* payload, unsigned int length) {   //callback includes topic and payload ( from which (topic) the payload is comming)
  Serial.print("MQTT-Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  if ((char)payload[0] == 'm' && (char)payload[1] == 'o' && (char)payload[2] == 'w')  //mow
  {
    Serial.println("mow start command received");
    client.publish(StatusTopic, "mow starting");
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+200;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_START;
      thExchange.cmdQueIdx++;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis()+300;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_OK;
      thExchange.cmdQueIdx++;
  }
  else if ((char)payload[0] == 'h' && (char)payload[1] == 'o' && (char)payload[2] == 'm' && (char)payload[3] == 'e') //home
  {
    Serial.println(" return to home command received");
    client.publish(StatusTopic, "returning to home");
          thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+200;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_HOME;
      thExchange.cmdQueIdx++;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis()+300;
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+500;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE1_OK;
      thExchange.cmdQueIdx++;
  }
  else if ((char)payload[0] == 's' && (char)payload[1] == 't' && (char)payload[2] == 'o' && (char)payload[3] == 'p') //stop
  {
    Serial.println(" STOP command received");
    client.publish(StatusTopic, "stopping");
      thExchange.cmdQue[thExchange.cmdQueIdx].T_start = millis();
      thExchange.cmdQue[thExchange.cmdQueIdx].T_end = millis()+200;
      thExchange.cmdQue[thExchange.cmdQueIdx].WebInButton[0] = BTN_BYTE2_STOP;
      thExchange.cmdQueIdx++;
  }
  // mqtt: to be continued...
  Serial.println();
}

void reconnect() {
  while (!client.connected()) {
    Serial.println("Attempting MQTT connection...");
    if (client.connect(MQTT_ID)) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish(StatusTopic, "LX790 connected to MQTT");
      // ... and resubscribe
      client.subscribe(inTopic);
      thExchange.aktualisieren = 1;
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void connectmqtt()
{
  client.connect(MQTT_ID);  // ESP will connect to mqtt broker with clientID
  {
    Serial.println("connected to MQTT");
    // Once connected, publish an announcement...

    // ... and resubscribe
    client.subscribe(inTopic); //topic=Demo
    client.publish(StatusTopic,  "connected to MQTT");
    thExchange.aktualisieren = 1;
    if (!client.connected())
    {
      reconnect();
    }
  }
}

void publishdata()
{
  if (thExchange.aktualisieren == 1)
  {
    if (strstr (TriggerURL,"http://") != NULL)
    {
      HTTPClient http;
      http.begin(TriggerURL); //Specify the URL
      int httpCode = http.GET();                                        //Make the request
      if (httpCode > 0) { //Check for the returning code
  //      String payload = http.getString();
  //      Serial.println(httpCode);
  //      Serial.println(payload);
         strcpy(thExchange.OldDisplay, thExchange.AktDisplay);
      }
     else 
      {
      Serial.println("Error on HTTP request");
      }
     http.end(); //Free the resources
    }
    //mqtt:
    if (strstr (mqtt_server,".") != NULL)
    {
      const char* BatState[] = {"off", "empty", "low", "mid", "full", "charging"};
      int IdxBatState = 0;
      // xSemaphoreTake(SemMutex, 1);
      if (thExchange.batCharge)
        IdxBatState = 5; /*charging*/
      else
        IdxBatState = thExchange.bat+1;
      sprintf(thExchange.battery, "%s", BatState[IdxBatState]);
      client.publish(DisplayTopic, thExchange.AktDisplay);
      if (strcmp(thExchange.AktDisplay, " OFF") == 0)
      {
        OFF_time = millis();
        client.publish(StatusTopic, "Aus");
       // client.publish(batteryTopic, "full");
      }
      else
      {
        client.publish(StatusTopic, GetStatustext());
        client.publish(batteryTopic, thExchange.battery);
      }
      char rssi[10];
      int strength = WiFi.RSSI();
      sprintf(rssi, "%i", strength);
      client.publish(rssiTopic, rssi);
      strcpy(thExchange.OldDisplay, thExchange.AktDisplay);
    }
    thExchange.aktualisieren = 0;
  }
}


void loop()
{
  //Core 1
}
