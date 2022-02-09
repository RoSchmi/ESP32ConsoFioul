#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <TCS34725.h>
#include "credentials.h"
#include "RemoteDebug.h" //wireless debugging
#include "ThingSpeak.h"
#include <Preferences.h>
// OTA update
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>

#define sec_to_ms(T) (T * 1000L)
#define RG_THRESHOLD 10
#define B_THRESHOLD 2
#define POSTING_INTERVAL 30 * 1000L // send data to thingspeak maximum every 30sec
// PREFERENCES
#define LAST_COUNTER_DAY_PREF "DAY"
#define DAILY_COUNTER_PREF "DAILY"
#define TOTAL_BURNER_TIME_PREF "TOTAL"

/* WiFi configuration */
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWD;
WiFiClient client;

/* ThingSpeak settings */
unsigned long lastConnectionTime = 0;

/* Color sensor */
TCS34725 tcs;

/* Remote Debug */
RemoteDebug Debug;

/* Counters */
bool burnerPreviouslyOn = false;
unsigned long startTime = 0;
unsigned long dailyBurnerTime = 0; // cumulative burner time in seconds for the day
unsigned long totalBurnerTime = 0; // cumulative burner time in seconds for the year

/* NTP & Time */
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600; // PARIS UTC+1
const int daylightOffset_sec = 3600;
int currentDay = -1;

/* Preferences */
Preferences preferences;

/* Web server */
AsyncWebServer server(80);

int wifiSetup(void);
bool isBurnerOn();
unsigned long runningTime(unsigned long startTime, unsigned long stopTime);
void sendThingSpeakhttpRequest(unsigned long rTime, unsigned long burnerTime, unsigned long totalBurnerTime);
int getCurrentDay();
void loadLastCounterFromPreferences();

void setup(void)
{
  Serial.begin(115200);
  Serial.println("Starting...");

  // Open preferences in "counter" namespace in RW mode
  preferences.begin("counter", false);

  // initialize thingspeak client
  ThingSpeak.begin(client);

  // wifi setup
  wifiSetup();

  // OTA server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", "Hi! I am ESP32 and couting fioul consumption."); });
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              dailyBurnerTime = 0;
              totalBurnerTime = 0;
              preferences.putULong(DAILY_COUNTER_PREF, dailyBurnerTime);
              preferences.putULong(TOTAL_BURNER_TIME_PREF, totalBurnerTime);
              request->send(200, "text/plain", "Fioul conter succesfully reset."); });
  server.on("/set", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              if (request->hasParam("totalCtr"))
              {
                unsigned long totalCtr = request->getParam("totalCtr")->value().toInt();
                if (totalCtr > 0)
                {
                  debugI("Total burner time initialized with value %u \n", totalCtr);
                  preferences.putULong(TOTAL_BURNER_TIME_PREF, totalCtr);
                  totalBurnerTime = totalCtr;
                }
                char s[128];
                snprintf(s, sizeof(s), "Fioul counter successfuly initialized with value %d \n", totalCtr);
                request->send(200, "text/plain", s);
              }
              request->send(400, "text/plain", "Bad request parameters"); });
  AsyncElegantOTA.begin(&server); // Start ElegantOTA
  server.begin();
  Serial.println("HTTP server started");

  // Sync NTP time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  getCurrentDay();

  // TCS setup
  Wire.begin();
  if (!tcs.attach(Wire))
    Serial.println("ERROR: TCS34725 NOT FOUND !");
  tcs.integrationTime(50); // ms
  tcs.gain(TCS34725::Gain::X01);

  Debug.begin(WiFi.getHostname());

  // load last total and daily value from preferences (in case of restart)
  loadLastCounterFromPreferences();
  // ready to goto loop
}

void loop(void)
{
  bool burnerCurrentlyOn = isBurnerOn();
  if (burnerCurrentlyOn)
  {
    if (!burnerPreviouslyOn)
    {
      // initialize times
      startTime = millis();
      burnerPreviouslyOn = true;
      debugI("Brûleur ON");
    }
  }
  else
  {
    unsigned long currTime = millis();
    // burner turned off
    if (burnerPreviouslyOn)
    {
      // check if it was really on or only in preheating mode (status LED was flashing)
      unsigned long rTime = runningTime(startTime, currTime);
      if (rTime > sec_to_ms(5))
      {
        // burner was on
        burnerPreviouslyOn = false;
        debugI("Brûleur OFF \n. Brûleur ON pendant %d \n", rTime);
        totalBurnerTime += rTime / 1000;
        preferences.putULong(TOTAL_BURNER_TIME_PREF, totalBurnerTime);
        // add to daily counter
        int day = getCurrentDay();
        if (day != currentDay && day != -1)
        {
          // new day : reset daily counter
          dailyBurnerTime = rTime / 1000;
          // set day
          currentDay = day;
          preferences.putInt(LAST_COUNTER_DAY_PREF, currentDay);
          preferences.putULong(DAILY_COUNTER_PREF, dailyBurnerTime);
          debugI("New day : reset daily counter \n");
        }
        else
        {
          dailyBurnerTime += rTime / 1000;
          preferences.putULong(DAILY_COUNTER_PREF, dailyBurnerTime);
          debugV("Same day \n");
        }

        // send to thingspeak
        if (currTime - lastConnectionTime > POSTING_INTERVAL)
        {
          sendThingSpeakhttpRequest(rTime / 1000, dailyBurnerTime, totalBurnerTime);
        }
      }
      else
      {
        burnerPreviouslyOn = false;
        // false positive
        debugI("Faux positif");
      }
    }
  }

  if (Debug.isActive(Debug.VERBOSE))
  { // Debug message info
    TCS34725::Color color = tcs.color();
    char s[128];
    snprintf(s, sizeof(s), "R : %d, G: %d, B: %d", (int)color.r, (int)color.g, (int)color.b);
    Debug.printf(s);
    delay(2000);
  }
  Debug.handle(); // remote debug
}

/* utility functions */

int wifiSetup(void)
{
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Hostname: ");
  Serial.println(WiFi.getHostname());
  return 1;
}

bool isBurnerOn()
{
  while (!tcs.available())
  {
    // Serial.println("waiting for sensor to become available");
  }
  TCS34725::Color color = tcs.color();

  if (color.g > RG_THRESHOLD && (color.r - color.g) < RG_THRESHOLD && color.b < B_THRESHOLD)
  {
    return true;
  }
  return false;
}

unsigned long runningTime(unsigned long startTime, unsigned long stopTime)
{
  return stopTime - startTime;
}

void sendThingSpeakhttpRequest(unsigned long rTime, unsigned long dailyBurnerTime, unsigned long totalBurnerTime)
{

  // Make sure there is an Internet connection.
  if (WiFi.status() != WL_CONNECTED)
  {
    wifiSetup();
  }
  ThingSpeak.setField(1, (long)rTime);
  ThingSpeak.setField(2, (long)dailyBurnerTime);
  ThingSpeak.setField(3, (long)totalBurnerTime);

  int respCode = ThingSpeak.writeFields(CHANNEL_ID, WRITE_API_KEY);
  if (respCode == 200)
  {
    debugI("Channel update successful.");
  }
  else
  {
    debugI("Problem updating channel. HTTP error code %d", respCode);
  }

  lastConnectionTime = millis();
}

int getCurrentDay()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    debugW("Failed to obtain time");
    return -1;
  }
  return timeinfo.tm_yday;
}

void loadLastCounterFromPreferences()
{
  // Read in field 2 and 3 of the private channel
  totalBurnerTime = preferences.getULong(TOTAL_BURNER_TIME_PREF, 0);
  int lastCounterDay = preferences.getInt(LAST_COUNTER_DAY_PREF, -1);
  if (lastCounterDay == getCurrentDay())
  {
    dailyBurnerTime = preferences.getULong(DAILY_COUNTER_PREF, 0);
    Serial.printf("Loaded counter for the day from memory. Day : %u, Total : %u \n", dailyBurnerTime, totalBurnerTime);
  }
}