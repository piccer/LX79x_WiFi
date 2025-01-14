# LX79x Mähroboter WiFi Erweiterung mit ESP32

Nachrüstung einer WiFi Schnittstelle mit Websteuerung für einen LandXcape LX79x Mähroboter

*Der Einsatz von diesem Projekt erfolgt auf eigene Gefahr! Der Ersteller kann nicht für Schäden haftbar gemacht werden. Sie können einen Verlust der Garantie und des Supports durch den Hersteller riskieren! Dieses Projekt ist keine offizielle Erweiterung des Roboters!*

<p align="center">
  <img src=pic/Display.jpg height="450"/>
  <img src=pic/Web_with_mower.png height="450"/>
</p>


### Allgemein
- Fernauslesen des Displays
- Klartextausgabe der Fehlercodes
- Fernsteuern der Tasten und Funktionen
- Anbindung über den I2C Bus zwischen Display und Mainboard
- Wenn der Roboter in der Ladestation steht uns aus ist, kann dieser mit "I/O" eingeschaltet werden

### Voraussetzungen
#### Hardware
Zum Einsatz kommt der ESP32 auf dem Board "DOIT ESP32 DEVKIT V1". Dieser wurde gewählt, da er zwei Hardware I2C Schnittstellen besitzt. Das ist wichtig, da beim I2C Bus des Roboters das Timing nicht zu sehr beeinflusst werden darf.

##### Anbindung an den I2C Bus
Um den ESP32 in den I2C Bus zwischen Mainboard und Display zwischenschalten zu können, benötigt man ein Adapterkabel. Dabei muss am Roboter selbst nichts "umgebaut" werden. Es wird lediglich der Stecker des Displays ausgesteckt und ein Adapter wie folgt dazwischen gesteckt:
<p align="center">
  <img src=pic/Adapter_1.jpg height="150"/>
  <img src=pic/Adapter_2.jpg height="150"/>
  <img src=pic/Display_1.jpg height="150"/>  
  <img src=pic/Display_2.jpg height="150"/>  
  <img src=pic/Durchgang_1.jpg height="150"/>  
  <img src=pic/Durchgang_2.jpg height="150"/>  
  <img src=pic/Schaltplan_Adapter.png height="150"/>  
</p>

Das Adapterkabel hat eine Gesamtlänge von ca 70cm.

##### Platine
Der ESP32 wird auf eine Lochrasterplatine Adaptiert. Die fertige Platine kann dann in dem Batteriefach eingebaut werden.
<p align="center">
  <img src=pic/Eingebaut_1.jpg height="150"/>
  <img src=pic/Platine_1.jpg height="150"/>
  <img src=pic/Platine_2.jpg height="150"/>
  <img src=pic/Schaltplan_ESP32.png height="150"/>
</p>

[Gesamter Schaltplan als PDF](pic/Schaltplan.pdf)

#### Software
Um den ESP32 programmieren und flashen zu können, wurde in diesem Projekt die Arduino IDE eingesetzt.

##### Folgende Voraussetzungen müssen dafür geschaffen werden:
1. Installation der aktuellen Arduino IDE
2. ESP32 Core installieren (https://randomnerdtutorials.com/installing-the-esp32-board-in-arduino-ide-windows-instructions/)
3. Boardtyp auswählen: 'Werkzeuge->Board->DOIT ESP32 DEVKIT V1'
4. CRC Bibliotheken ergänzen: 'Werkzeuge->Bibliotheken verwalten...->CRC (Rob Tillaart)' und 'PubSubClient'
5. Filesystem Uploader: (https://randomnerdtutorials.com/install-esp32-filesystem-uploader-arduino-ide/)
  1. Download ESP32FS ZIP: https://github.com/me-no-dev/arduino-esp32fs-plugin/releases/
  2. ZIP direkt in "c:\Program Files (x86)\Arduino\tools\" entpacken
  3. Arduino IDE neu starten

##### ESP32 flashen/Programm und Daten uploaden:
1. Repository herunterladen und "ino" Projektdatei öffnen
2. ESP32 Board per USB verbinden und passende COM Schnittstelle auswählen: 'Werkzeuge->Port'
3. Im .ino Code die WiFi Zugangsdaten und MQTT-Datenanpassen:
```c
const char* ssid     = "DEINESSID";
const char* password = "DEINPASSWORT";
const char* mqtt_server = "192.168.1.8";
const char* MQTT_ID = "LX790";
const char* DisplayTopic = "Display";
const char* rssiTopic = "RSSI";
const char* StatusTopic = "Statustext";
const char* inTopic = "inTopic";
```
Wenn MQTT nicht genutzt werden soll, dann ist 
```c
const char* mqtt_server = "";
```
zu setzen. Das gleiche gilt für
```c
const char* TriggerURL = "http://FHEM/fhem?cmd=set%20ROBBI%20reread&XHR=1";
```
wenn diese URL gesetzt ist, wird sie aufgerufen, sobald sich das Display am LX790 ändert.

4. Ordner "data" mit 'Werkzeuge->ESP32 Sketch Data Upload' hochladen
5. Seriellen Monitor öffnen
6. Projekt kompilieren und hochladen
7. Über Seriellen Monitor kontrollieren ob WiFi Verbindung besteht und IP Adresse, welche vom DHCP Server vergeben wurde, ermitteln bzw. merken.

### Anwendung
#### Webbrowser
Das Webinterface kann direkt mit der IP Adresse aufgerufen werden:
<p align="center">
  <img src=pic/Web_without_mower.png width="350"/>
</p>

#### MQTT
- Subsribe zur Variable "inTopic" für Befehle
- aktuell funktionieren die Befehle "mow", "home"(entsprechen "startmow" und "homemow") und "stop"
- Publishing von "Display", "RSSI" und "Status" (Klartext)

#### HTTP Anfragemethoden
##### Befehle Senden
http://MOWERADRESS/cmd?parm=[COMMAND]&value=[VALUE]

param=[COMMAND]:

- start -> Taste Start bzw. hoch
- home -> Taste Home bzw. runter
- ok -> Taste Ok
- stop -> Taste Stop
- workzone -> Arbeitsbereich einstellen
- timedate -> Zeit und Datum einstellen
- startmow -> "Mähen" starten
- homemow -> "Zurück zur Ladestation" starten

value=[VALUE]:

- Bei "start, home, ok, stop" ist [VALUE] die Dauer in ms des Tastendrucks ansonsten muss 1 übergeben werden.

>Beispiel - die Taste Start soll 5 Sekunden gedrückt werden:
>
>http://MOWERADRESS/cmd?parm=start&value=5000

##### Zustand Abfragen
http://MOWERADRESS/statval

Antwort:
[Display];[RSSI];[Batteriestatus];[Klartext]

>Beispiel bei Fehler:
>
>-E8-;-83;off;Es dauert zu lange, bis der Robi zur Ladestation zurückkehrt.
>
>Beispiel während dem Mähen:
>
>|--|;-80;mid;Mähen...

##### Update ausführen
http://MOWERADRESS/update

Damit kann eine .bin Datei auf den ESP32 hochgeladen werden.
