#include <OneWire.h>
#include <DallasTemperature.h>
#include "LIFOQueue.h"
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>

const byte oneWireBus = D5;
//this controls the signal for the electric heater
const byte relay1 = D1;
byte relay1State = 0;
//this controls the pump for the pool heating
const byte relay2 = D2;
byte relay2State = 0;
const byte relay3 = D6;
byte relay3State = 0;
const byte relay4 = D3;
byte relay4State = 0;
//error indicator led
const byte errorIndicator = D7;
bool doWeHaveAnError = true;

// lifo queue to store the temperature values
LiFoQueue queue(20);
//wifi settings
const char* ssid     = "<YourSSID>";
const char* password = "<YourPassword>";

//when true the system will not set the relays
bool manualOverride = false;
//the server address where we upload the data
const char* cloudServerAddress = "<YourServerName>";
//when true and the boiler temperature above 50 the pool heating is activated
bool summerMode = false;
//when true the unit sends data to the cloud server as well
bool sendDataToCloud = false;
// loopCounterForReportingToCloud we would like to report the status in every 10 minutes
int loopCounterForReportingToCloud = 10;
//wifi signal strength
int wifiSignalStrength = 0;
//the default value after the unit has been started
int targetTemperature = 50;
//version number
const String versionNumber = "2021-01-14";
//stores the latest incoming readings from the external unit
String sensor1Value;
String sensor2Value;
//stores the number of incorrect readings for the restart cycle
int numberOfFalseReadings = 0;
//this is the OTA enable variables
ESP8266WebServer server;
//set this value by default to false as the OTA timeout is 5 minutes
bool ota_flag = false;
//onewire bus
OneWire oneWire(oneWireBus);
//dallastemperature
DallasTemperature sensors(&oneWire);
// flashActiveLed used to flash the led on the board to show activity
bool activeLedState = false;
// network less mode
bool doWeHaveNetwork = false;
// if we have we will try to reconnect later
bool doWeHaveNetworkDuringSetup = false;
// items for ntp update
WiFiUDP ntpUDP;
const long utcOffsetInSeconds = 3600;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", utcOffsetInSeconds);
String lastRebootWas = "Unknown";
/*
   Start OTA.
*/
void startOTA() {
  ArduinoOTA.setPassword("wemosDefaultPassword_123");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.println("Error[%u]:");
    Serial.println(error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
}
/*
   We use a webserver to set runtime action if needed.
*/
void initWebServer() {
  server.on("/", []() {
    server.send(200, "text/html", getWebPage());
    delay(1000);
  });

  server.on("/increase", []() {
    Serial.println("Increasing target temperature with 5...");
    if (targetTemperature < 80) {
      targetTemperature = targetTemperature + 5;
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/decrease", []() {
    Serial.println("Decreasing target temperature with 5...");
    if (targetTemperature > 30) {
      targetTemperature = targetTemperature - 5;
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/switchOne", []() {
    Serial.println("Switching relay 1 ON or OFF...");
    if (relay1State != 0) {
      relay1State = 0;
      digitalWrite(relay1, HIGH);
    } else {
      relay1State = 1;
      digitalWrite(relay1, LOW);
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/switchTwo", []() {
    Serial.println("Switching relay 2 ON or OFF...");
    if (relay2State != 0) {
      relay2State = 0;
      digitalWrite(relay2, HIGH);
    } else {
      relay2State = 1;
      digitalWrite(relay2, LOW);
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/switchThree", []() {
    Serial.println("Switching relay 3 ON or OFF...");
    if (relay3State != 0) {
      relay3State = 0;
      digitalWrite(relay3, HIGH);
    } else {
      relay3State = 1;
      digitalWrite(relay3, LOW);
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/switchFour", []() {
    Serial.println("Switching relay 4 ON or OFF...");
    if (relay4State != 0) {
      relay4State = 0;
      digitalWrite(relay4, HIGH);
    } else {
      relay4State = 1;
      digitalWrite(relay4, LOW);
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/manualOverride", []() {
    Serial.println("Switching Manual Override ON or OFF...");
    if (manualOverride) {
      manualOverride = false;
    } else {
      manualOverride = true;
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/setflag", []() {
    Serial.println("Setting OTA flag true based on webserver request...");
    server.send(200, "text/plain", "Setting flag...");
    ota_flag = true;
  });

  server.on("/restart", []() {
    Serial.println("Restarting based on webserver request...");
    server.send(200, "text/plain", "Restarting...");
    delay(1000);
    ESP.restart();
  });

  server.on("/receiveData", []() {
    Serial.println("Received data...");
    sensor1Value = server.arg("v1");
    sensor2Value = server.arg("v2");
    server.send(200, "text/plain", "Received data...");
  });

  server.on("/summerMode", []() {
    Serial.println("Switching Summer Mode ON or OFF...");
    if (summerMode) {
      summerMode = false;
    } else {
      summerMode = true;
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/sendDataToCloud", []() {
    Serial.println("Switching Cloud data Sending ON or OFF...");
    if (sendDataToCloud) {
      sendDataToCloud = false;
    } else {
      sendDataToCloud = true;
    }
    server.send(200, "text/html", getRedirectWebPage());
  });

  server.on("/help", []() {
    Serial.println("Help page was requested...");
    server.send(200, "text/plain", "Commands are: manualOverride / setflag / restart / receiveData / summerMode / sendDataToCloud");
  });

  server.begin();
}

String getWebPage() {
  String webPage = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">";
  webPage = webPage + "<style>table, th, td {border: 1px solid black;}</style></head>";

  webPage = webPage + "<header><h1>Current Temperature: ";
  webPage = webPage + queue.getAvarage() + "</h1></header>";

  webPage = webPage + "<h2>Target Temperature: " + targetTemperature + "</h2>";
  webPage = webPage + "<body>";
  webPage = webPage + "<table><tr><th>Action</th><th>Button</th></tr>";

  webPage = webPage + "<tr><td>Increase</td><td>";
  webPage = webPage + "<form method=\"post\" action=/increase><input id=\"increaseButton\" type=\"submit\" value=\"submit\" style=\"width:100%\"></form>";
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>Decrease</td><td>";
  webPage = webPage + "<form method=\"post\" action=/decrease><input id=\"decreaseButton\" type=\"submit\" value=\"submit\" style=\"width:100%\"></form>";
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>" + getRelayStatus(1) + "</td><td>";
  webPage = webPage + "<form method=\"post\" action=/switchOne><input id=\"switchOne\" type=\"submit\" value=\"submit\" style=\"width:100%\"></form>";
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>" + getRelayStatus(2) + "</td><td>";
  webPage = webPage + "<form method=\"post\" action=/switchTwo><input id=\"switchTwo\" type=\"submit\" value=\"submit\" style=\"width:100%\"></form>";
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>" + getRelayStatus(3) + "</td><td>";
  webPage = webPage + "<form method=\"post\" action=/switchThree><input id=\"switchThree\" type=\"submit\" value=\"submit\" style=\"width:100%\"></form>";
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>" + getRelayStatus(4) + "</td><td>";
  webPage = webPage + "<form method=\"post\" action=/switchFour><input id=\"switchFour\" type=\"submit\" value=\"submit\" style=\"width:100%\"></form>";
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>Sensor 1:</td><td>" + sensor1Value +"</td></tr>";
  webPage = webPage + "<tr><td>Sensor 2:</td><td>" + sensor2Value +"</td></tr>";

  webPage = webPage + "<tr><td>Manual Override</td><td>";
  if (manualOverride != 0) {
    webPage = webPage + "ON*";
  } else {
    webPage = webPage + "OFF*";
  }
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>Summer mode</td><td>";
  if (summerMode) {
    webPage = webPage + "ON*";
  } else {
    webPage = webPage + "OFF*";
  }
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>Send data to cloud</td><td>";
  if (sendDataToCloud) {
    webPage = webPage + "ON*";
  } else {
    webPage = webPage + "OFF*";
  }
  webPage = webPage + "</td></tr>";

  webPage = webPage + "<tr><td>Wifi Signal : </td><td>" + getWifiSignalStrenght() + "</td></tr>";
  webPage = webPage + "<tr><td>Version Number:</td><td>" + versionNumber + "</td></tr>";
  webPage = webPage + "</table>";
  webPage = webPage + "* to change this value use the direct link</br>";
  webPage = webPage + "Reset reason was : " + ESP.getResetReason() + "</br>";
  webPage = webPage + "Last reboot was at: " + lastRebootWas;
  webPage = webPage + "</body></html>";
  
  return webPage;
}

String getRelayStatus(int paramRelayStatus) {
  String relay = "Relay ";
  switch (paramRelayStatus) {
    case 1:
      if (relay1State != 0) {
        relay = relay + " 1 - ON";
      } else {
        relay = relay + " 1 - OFF";
      }
      break;
    case 2:
      if (relay2State != 0) {
        relay = relay + " 2 - ON";
      } else {
        relay = relay + " 2 - OFF";
      }
      break;
    case 3:
      if (relay3State != 0) {
        relay = relay + " 3 - ON";
      } else {
        relay = relay + " 3 - OFF";
      }
      break;
    case 4:
      if (relay4State != 0) {
        relay = relay + " 4 - ON";
      } else {
        relay = relay + " 4 - OFF";
      }
      break;
    default:
      relay = "ERROR";
      break;
  }
  return relay;
}

String getWifiSignalStrenght() {
  String signal = "None";
  if (wifiSignalStrength <= 65 ) {
    signal = "| | | |";
  } else if (wifiSignalStrength > 65 and wifiSignalStrength <= 70) {
    signal = "| | |";
  } else if (wifiSignalStrength > 70 and wifiSignalStrength <= 75) {
    signal = "| |";
  } else {
    signal = "|";
  }
  signal = signal + " : " + wifiSignalStrength;
  return signal;
}

String getRedirectWebPage() {
  String webPage = "<html><head><meta http-equiv = \"refresh\" content=\"2; url=http://" + WiFi.localIP().toString() + "/\"></head>";
  webPage = webPage + "<body>Working...</body></html>";
  return webPage;
}

void connectToWifi() {
  doWeHaveNetwork = false;
  wifiSignalStrength = -99;
  WiFi.hostname("WemosPro-HeatingController");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int connectionLoop = 0;
  Serial.println("Trying to connect to wifi...");
  //if we cannot connect to wifi within watchdog period restart the board
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.println("Connecting to WIFI...");
    connectionLoop ++;
    if (connectionLoop > 30) {
      Serial.println("Unable to connect to WIFI network...");
      doWeHaveAnError = true;
      break;
      //ESP.restart("Wifi signal was too low or unavailable!");
    }
    flashActiveLed();
  }
  if (WiFi.status() == WL_CONNECTED) {
    doWeHaveNetwork = true;
    wifiSignalStrength = WiFi.RSSI();
    doWeHaveAnError = false;
  }
}

void sendDataToServer() {
  //if we do not have network we quit from this method
  if (doWeHaveNetwork == false) {
    return;
  }
  String Link = "/anticontroller/reportStatus.php";
  const int httpsPort = 443;
  //const char fingerprint[] = "60 19 05 13 28 C4 18 C0 49 E1 47 CB 7A F7 53 E8 00 67 92 D3";
  const char fingerprint[] = "3A FC 88 F1 16 7F D8 33 34 A1 70 F3 99 86 CC 97 88 3A 07 85";
  WiFiClientSecure httpsClient;    //Declare object of class WiFiClient

  Serial.print("Trying to connect to : ");
  Serial.println(cloudServerAddress);

  //Serial.printf("Using fingerprint '%s'\n", fingerprint);
  httpsClient.setFingerprint(fingerprint);
  httpsClient.setTimeout(15000); // 15 Seconds
  delay(1000);
  
  Serial.println("Establishing connection...");
  int r=0; //retry counter
  while((!httpsClient.connect(cloudServerAddress, httpsPort)) && (r < 30)){
      Serial.print(".");
      r++;
      ESP.wdtFeed();
      delay(100);
      flashActiveLed();
  }
  if(r==30) {
    Serial.println("Connection failed");
    return;
  }
  else {
    Serial.println("Connected...");
  }
  //Serial.print("requesting URL: ");
  //Serial.println(cloudServerAddress);

  //this value here is a secret key leave it as it is
  String postMessage = "sendDataToServer=true";
  postMessage = postMessage + "&targetTemperature=" + targetTemperature;
  postMessage = postMessage + "&currentTemperature=" + queue.getAvarage();
  postMessage = postMessage + "&relay1=" + relay1State;
  postMessage = postMessage + "&sensor1=" + sensor1Value;
  postMessage = postMessage + "&sensor2=" + sensor2Value;

  //Serial.println("postMessage:");
  //Serial.println(postMessage);

  String httpMessage = String("POST ") + Link + " HTTP/1.1\r\n" +
               "Host: " + cloudServerAddress + "\r\n" +
               "Content-Type: application/x-www-form-urlencoded"+ "\r\n" +
               "Connection: close\r\n" +
               "Content-Length: " + postMessage.length() + "\r\n\r\n" +
               postMessage + "\r\n\r\n";
    
  httpsClient.print(httpMessage);

  Serial.println("request sent...");
  //Serial.println(httpMessage);

  Serial.println("waiting for reply");
  while (httpsClient.connected()) {  
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r") {
      Serial.println("headers received");
      break;
    }
    Serial.print(".");
    ESP.wdtFeed();
    delay(100);
    flashActiveLed();
  }

  Serial.println("reply was:");
  Serial.println("==========");
  String line;
  while(httpsClient.available()){        
    line = httpsClient.readStringUntil('\n');  //Read Line by Line
    Serial.println(line); //Print response
  }
  Serial.println("==========");
  Serial.println("closing connection");
  httpsClient.stop();
}

void flashActiveLed() {
  if (activeLedState) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
  }
  if (doWeHaveAnError) {
    digitalWrite(errorIndicator, LOW);
  } else {
    digitalWrite(errorIndicator, HIGH);
  }
  activeLedState = !activeLedState;
  ESP.wdtFeed();
}

void setup() {
  // Open serial communications and wait for port to open:
  Serial.begin(115200);
  Serial.println("Setup started...");

  ESP.wdtDisable();

  pinMode(relay1, OUTPUT);
  pinMode(relay2, OUTPUT);
  pinMode(relay3, OUTPUT);
  pinMode(relay4, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(errorIndicator, OUTPUT);
  //Set the relays to default state
  //put these delays here to minimize the electronic noise from the relay board
  //experienced several power spikes which could cause serious issues
  ESP.wdtFeed();

  digitalWrite(relay1, HIGH);
  flashActiveLed();
  delay(1000);
  digitalWrite(relay2, HIGH);
  delay(1000);
  flashActiveLed();
  digitalWrite(relay3, HIGH);
  delay(1000);
  flashActiveLed();
  digitalWrite(relay4, HIGH);
  delay(1000);
  digitalWrite(relay4, HIGH);
  flashActiveLed();

  //start wifi connection
  Serial.println("Checking for wifi network...");
  connectToWifi();
  if (doWeHaveNetwork) {
    doWeHaveNetworkDuringSetup = true;
    Serial.println("Ready");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.println("MAC address: ");
    Serial.println(WiFi.macAddress());
    Serial.println("Starting OTA listener...");
    startOTA();
    Serial.println("Starting webserver...");
    initWebServer();
    Serial.println("Getting restart time...");
    timeClient.update();
    setTime(timeClient.getEpochTime());
    lastRebootWas = String(year()) + "-" + String(month()) + "-" + String(day()) + ":" + String(hour()) +":"+ String(minute()) +":" + String(second());
    Serial.println("lastRebootWas: " + lastRebootWas);
  } else {
    Serial.println("We are in network less mode...");
  }
  sensors.begin();
  //set the resolution to the max
  sensors.setResolution(12);
}

/*
   The main loop collects the data, send it to the server then set the relay based on the given value.
*/
void loop() {
  if (ota_flag)
  {
    ota_flag = false;
    int loopCounter = 0;
    Serial.println("Waiting for OTA connection for 120 seconds...");
    while (loopCounter < 600)
    {
      loopCounter++;
      ArduinoOTA.handle();
      delay(200);
    }
  }
  //set the led on to show we are connected and gathering data
  //create a loop which connects the data for 30 seconds
  int counter = 0;
  for (int i = 0; i < 20; i++) {
    flashActiveLed();
    sensors.requestTemperatures(); // Send the command to get temperature readings
    double value = sensors.getTempCByIndex(0);
    Serial.print("sensor reading: ");
    Serial.println(value);
    //filter out the possible junk values
    if (value < -30 || value > 100) {
      Serial.println("Rejecting data due to its possible invalidity!");
      numberOfFalseReadings++;
      if (numberOfFalseReadings > 10) {
        //sendDataToServer(true, "Too many incorrect readings!");
        //ESP.restart();
        //send email?
      }
    } else {
      numberOfFalseReadings = 0;
      queue.pushValue(value);
    }
    //1200msec * 20 loops will end in half minute with sending and all other functions
    ESP.wdtFeed();
    if (doWeHaveNetwork) {
      server.handleClient();
    } 
    delay(2000);
    ESP.wdtFeed();
    counter++;
    ESP.wdtFeed();
  }
  Serial.println("End of data collecting cycle...");
  //if the manual override is off we control the boiler signal
  if (!manualOverride) {
    Serial.println("Setting relay 1...");
    if (queue.getAvarage() < targetTemperature) {
      relay1State = 1;
      digitalWrite(relay1, LOW);
    } else {
      relay1State = 0;
      digitalWrite(relay1, HIGH);
    }
  }

  if (summerMode) {
    Serial.println("Summer mode is active checking the temperature...");
    if (queue.getAvarage() > targetTemperature) {
      Serial.println("Temperature is greater then targetTemperature switch ON relay2...");
      relay2State = 1;
      digitalWrite(relay1, LOW);
    } else {
      Serial.println("Temperature is less then targetTemperature switch OFF relay2...");
      relay2State = 0;
      digitalWrite(relay1, HIGH);
    }
  }
  //if we original did not connected to any network then we do not try it here
  if (doWeHaveNetworkDuringSetup) {
    //if we are not connected trying to reconnect.
    //do not feed watchdog in the loop as it can hang up
    if (WiFi.status() != WL_CONNECTED)
    {
      connectToWifi();
    }
    if (doWeHaveNetwork && sendDataToCloud) {
      //we only deal with the counter if this is true
      //making sure that the counter will not overflow
      if (loopCounterForReportingToCloud >=10) {
        loopCounterForReportingToCloud = 0;
        sendDataToServer();
      } else {
        Serial.print("loopCounterForReportingToCloud is : ");
        Serial.println(loopCounterForReportingToCloud);
        loopCounterForReportingToCloud++;
      }
    }
  } else {
      Serial.println("We have no network, skipping reporting steps...");
  }
}
