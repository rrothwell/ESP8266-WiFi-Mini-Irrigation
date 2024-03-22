//---------------------- Section: Synopsis -------------------

// Copyright 2024 by Richard Rothwell
// ----------------------------------
// ESP8266 module on WiFi Mini PCB.
// The WiFi Mini PCB uses two shields:
// 1. A DS1703 RTC shield.
// 2. A custom shield for interfacing to: 
//    a) a Relay module, with 3-wire interface (Songle, SRD-05VDC-SL-C). This drives a 24VAC solenoid valve.
//    b) a temperature/humidity sensor module (DHT11), with 3-wire interface. This monitors atmospheric conditions.
//    c) a soil humidity sensor, with 3-wire interface (). This monitors soil conditions.
//    d) a Real time clock, with I2C interface (DS1307). This is used to schedule watering times.

//---------------------- Section: Libraries -------------------

#include <ESP8266WiFi.h>
#include "RTClib.h"
#include "LittleFS.h"
#include "PSACrypto.h"


//---------------------- Section: Microprocessor Hardware -------------------

// Shield pin assignments.
// Schematic and pinouts.
// https://lastminuteengineers.com/wemos-d1-mini-pinout-reference/

// Arduino GPIO pin assignments

#define SHIELD_D0 (16)  // GPIO16, Wake from sleep.
#define SHIELD_D1 (5)   // GPIO5
#define SHIELD_D2 (4)   // GPIO4
#define SHIELD_D3 (0)   // GPIO0, Flash when low.
#define SHIELD_D4 (2)   // GPIO2
#define SHIELD_D5 (14)  // GPIO14
#define SHIELD_D6 (12)  // GPIO12
#define SHIELD_D7 (13)  // GPIO13
#define SHIELD_D8 (15)  // GPIO15
#define SHIELD_A0 (ADC0) // ADC0
#define SHIELD_TX (1)   // GPIO1
#define SHIELD_RX (3)   // GPIO3
// #define SHIELD_GND (0)
// #define SHIELD_5V (0)
// #define SHIELD_3.3V (0)
// #define SHIELD_RST (0) 

#define SHIELD_WAKE (SHIELD_D0)
#define SHIELD_FLASH (SHIELD_D3)

// Default I2C pin assignments. Bit banging is used.
#define RTC_I2C_SCL D1
#define RTC_I2C_SDA D2

// SPI. Hardware pin assignments.
#define SPI_SCLK D5
#define SPI_MISO D6
#define SPI_MOSI D7
#define SPI_CS   D8

// Analog to digital converter 10 bits.
#define ADC   A0

//---------------------- Section: Relay Output -------------------

// Relay module.
#define RELAY_PIN SHIELD_D4 // relay connected to  GPIO4

// Relay initialisation.
const bool RELAY_OFF = 0;
const bool RELAY_ON = 1;
bool relayState = RELAY_OFF;

//---------------------- Section: Humidity Sensor Input -------------------

// Temperature humidity sensor. 
// https://lastminuteengineers.com/esp8266-dht11-dht22-web-server-tutorial/


//---------------------- Section: Realtime Clock -------------------

#define INIT_DATETIME_ON_FIRST_RUN

// The DS1703 needs to be reset to the current time regularly.
// So uncomment this line temporarily prior to compilation and then download.
// #define FORCE_DATETIME

// Real time clock shield
// BCD RTC plus 65 bytes of NVRAM and programmable squarewave output (not connected).
// https://abra-electronics.com/robotics-embedded-electronics/wemos/wemos-ds1307-wemos-wifi-d1-mini-shield-rtc-ds1307-real-time-clock-with-battery.html

// Battery is required:CR1220.
// Modifications to reduce current draw:
// https://www.instructables.com/Using-the-Wifi-D1-Mini-Real-time-Clock-and-Logger/

// Realtime clock initialisation.
String weekDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
RTC_DS1307 rtc;

//---------------------- Section: Data Persistence via Flash File System -------------------

// Flash file system.
// https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html

//---------------------- Section: Local Area Network Connection -------------------

// The SSID and WiFi password are now loaded from the flash file system.
// The file is config.txt andd it's loaded into flash separately using the "ESP8266 LittleFS Data Upload" tool.
// This tool is installed into the Arduino IDE. 
// This tool, at March 2024, needs to be used umder legacy Arduino IDE version 1.8.
// The file content is a property or INI file like: "SSID: NETGEAR87_EXT\r\lpassword: ######.
// In other words name/value pairs separated by CR/LF.

// Do not commit to a public repo with valid credentials here.
String defaultSsid = "NETGEAR87_EXT"; // fill in here your router or wifi SSID
String defaultPassword = "##########"; // fill in here your router or wifi password

String ssid; // fill in here your router or wifi SSID
String wifiPassword; // fill in here your router or wifi password

IPAddress localIPAddress;
//---------------------- Section: Web Server User Interface -------------------

// Web server to control relay: 
// https://blog.lindsaystrategic.com/2018/01/04/hw-655-esp-01-relay-board/
// https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266WiFi/examples/WiFiManualWebServer/WiFiManualWebServer.ino

// Web server initialisation.
WiFiServer server(80);

String webUserName;
String webPassword;
String sessionIdRepository;

// Web user record.
typedef struct
{
  const char* userName;
  const char* passwordHash;
  const char* sessionId;
  const char* sessionExpiryDateTime;
} UserRecord; 

UserRecord userRecords[] = {
   {"admin", ""}
};

//---------------------- Section: Irrigation Application Types and Globals -------------------

  // Irrigation schedule initialisation defined in hours.
  typedef struct
  {
    int begin;
    int end;
  } IrrigationEvent; 

  // Misting is define in seconds. 
  typedef struct
  {
    int begin;
    int end;
  } MistEvent; 

  typedef struct
  {
    int irrigationBeginHours;
    int irrigationDurationHours;
  
    int mistPeriodMinutes;
    int mistDurationSeconds;
   } Schedule;

  // Default values to be overridden by future web interface.
  // TODO Input data validation.
  Schedule defaultSchedule =
  {
    10, // irrigationBeginHours
    4,  // irrigationDurationHours
    20, // mistPeriodMinutes
    5   // mistDurationSeconds
  };

  Schedule currentSchedule;
  const Schedule NULL_SCHEDULE = {0, 0, 0, 0};

  MistEvent mistToday;
  IrrigationEvent waterToday;

  // Keep track of misting state to detect transitions.
  bool isPreviousMisting;

  enum Automation
  {
    NULL_STATE,
    MANUAL_ACTIVE,
    MANUAL_INACTIVE,
    AUTO_CONFIG,
    AUTO_START,
    AUTO_RUN,
  };
  int automationStatus = AUTO_RUN;

  // #define REPORT_IRRIGATION

 //---------------------- Section: Application Initialisation -------------------

void setup() 
{
  Schedule schedule;
  
  // The Serial Monitor can be set to 115200 to eavesdrop.
  // This is the default for the D1 WiFi Mini.
  Serial.begin(115200);

  // Ensure relay is safe off in event of power failure. 
  Serial.println();
  pinMode(RELAY_PIN, OUTPUT);
  switchRelay(relayState);
  
  initFileSystem();
  if (!readConfigFromFS(ssid, wifiPassword, schedule))
  {
    ssid = defaultSsid;
    wifiPassword = defaultPassword;
    schedule = defaultSchedule;
  }
  // Serial.print("SSID: "); Serial.println(ssid);
  // Serial.print("Password: "); Serial.println(wifiPassword);

  // PSA Crypto is used to generate session id's and password hashes.
  // If this is not done a -137 error code is returned.
  psa_crypto_init();

  webUserName  = F("userX");
  webPassword  = F("passX");
  
  String hashedPassword;
  bool isHashed = hasher(webPassword, hashedPassword);
  if(isHashed)
  {
    const char *webUserNames[] = {webUserName.c_str()};
    String userDetails = hashedPassword;
    const char *webUserDetails[] = {userDetails.c_str()};
    writeCredentialsToFS(webUserNames, webUserDetails);
  }
  else
  {
     Serial.println(F("Failed web password hash. ")); 
  }

  // Verify by reading the hash back.
  String userDetails;
  if (readCredentialsFromFS(webUserName, userDetails))
  {
    Serial.print(F("Loaded password hash: ")); Serial.println(userDetails);
  }
  else
  {
    Serial.println(F("Failed to load password hash. ")); 
  }

  initRTC(); 
  localIPAddress = initNetworkConnection(ssid, wifiPassword);


  if(validateIrrigationSchedule(schedule))
  {
    currentSchedule = schedule;
    initIrrigationSchedule(currentSchedule);    
  }
  else
  {
    Serial.println("The irrigation schedule is outside the acceptable range. ");
    Serial.flush();
    abort();
  }
}

//---------------------- Section: Application Run Loop -------------------

void loop() 
{
  String request;

  // Wait for client connection.
  WiFiClient browser = server.accept();
  if (browser) 
  {
    Serial.println("New web client");
    browser.setTimeout(5000);  // Overide default of 1000.
    
    automationStatus = handleRequest(browser, relayState);

    Serial.println("Web client disconnected.");
    Serial.println("");

    // At the end of scope for the browser object: 
    // 1. the response is flushed to the browser.
    // 2. the connection is closed.
  }

  switch(automationStatus)
  {
    case MANUAL_ACTIVE:
        mist(relayState);
        automationStatus = MANUAL_INACTIVE;
        break;
    case MANUAL_INACTIVE:
        break;
    case AUTO_CONFIG:
        Schedule schedule;
        if(updateSchedule(request, schedule))
        {
          if(writeConfigToFS(ssid, wifiPassword, schedule))
          {
            currentSchedule = schedule;
            initIrrigationSchedule(currentSchedule);
            Serial.println("Schedule is updated. ");
          }
        }
        automationStatus = AUTO_START;
        break;
    case AUTO_START:
        mist(false);
        Serial.println("Beginning the misting schedule. ");
        automationStatus = AUTO_RUN;
        break;
    case AUTO_RUN:
    default:
        irrigate(currentSchedule);
        // Nothing
        break;
  }
}

//---------------------- Section: Relay Functions -------------------

 // Set the relay state.
 void switchRelay(bool relayState)
 {
    if (relayState == RELAY_ON)  
    {
      digitalWrite(RELAY_PIN, HIGH);;
      Serial.println("RELAY=ON");
    }
    else  
    {
      digitalWrite(RELAY_PIN, LOW);;
      Serial.println("RELAY=OFF");
    }
 }

//---------------------- Section: Realtime Clock Functions -------------------

void initRTC()
{
  Serial.println("RTC begin init.");

  int rtcRetryCount = 5;
  int rtcRetryIndex = 0;

// Clear I2C comms to RTC after Arduino reset.
  pinMode(RTC_I2C_SCL,OUTPUT);
  digitalWrite(RTC_I2C_SCL,LOW);
  delay(100);

  bool isSuccess = true;
  while (!rtc.begin())
  {
    rtcRetryIndex++;
    Serial.print("RTC retry. "); Serial.println(rtcRetryIndex);
    if (rtcRetryIndex >= rtcRetryCount)
    {
      isSuccess = false;
      break;
    }
    delay(500);
  }
  
  if (isSuccess)
  {
     Serial.println("RTC found. ");
  }
  else
  {
    Serial.println("RTC not found.");
    Serial.flush();
    abort();
  }

  if (rtc.isrunning())
  {
    Serial.println("RTC is running.");
#ifdef FORCE_DATETIME
    applyHostComputerDateTime();
#endif
  }
  else
  {
    Serial.println("RTC is not running. ");
    Serial.println("Initialise the datetime on new RTC or battery replacement.");
    applyHostComputerDateTime();  
  }
  Serial.print(F("Reading RTC: ")); 
  Serial.print(currentDate()); Serial.print(F(" ")); Serial.println(currentTime());
}

void applyHostComputerDateTime()
{
#ifdef INIT_DATETIME_ON_FIRST_RUN
    // Use datetime at time of compilation.
    DateTime dateTime = DateTime(F(__DATE__), F(__TIME__));
#else
    // Use datetime as decided by programmer.
    DateTime dateTime = DateTime(2024, 3, 21, 23, 59, 59);
#endif
    rtc.adjust(dateTime);
    Serial.print(F("RTC time is reset to: ")); 
    Serial.print(currentDate()); Serial.print(F(" ")); Serial.println(currentTime());
}

String currentDate()
{
  DateTime now = rtc.now();
  String current = weekDays[now.dayOfTheWeek()];
  current += F(" - ") + String(now.year()) + F("/") + now.month()  + F("/") +  now.day();
  return current;
}

String currentTime()
{
  DateTime now = rtc.now();
  String current = String(now.hour()) + F(":") + now.minute()  + F(":") +  now.second();
  return current;
}
//---------------------- Section: File System Initialisation -------------------

// Initialise file system.
// for persistent configuration.
void initFileSystem()
{
  // FS = file system.
  // File system storage is microcomputer internal flash memory,
  // but could also use the nvram in the clock chip or an SD card.

  LittleFSConfig fsConfig;
  fsConfig.setAutoFormat(false);
  LittleFS.setConfig(fsConfig);  
}

//---------------------- Section: Configuration File Functions -------------------

// Extract configuration from file system.
boolean readConfigFromFS(String &ssid, String &wifiPassword, Schedule &schedule)
{
  boolean isConfigFileValid = false;
  bool isFSMounted = LittleFS.begin();
  if (isFSMounted)
  {
    File fileHandle = LittleFS.open(F("config.txt"), "r");
    if (fileHandle)
    {
      String fileContents;
      // Read an byte as an int at a time, then treat it as a char.
      while (fileHandle.available())
      {
        fileContents += static_cast<char>(fileHandle.read());
      }
      ssid = extractPropertyValue(fileContents, "SSID");
      wifiPassword = extractPropertyValue(fileContents, "password");
      
      String irrigationBeginHoursStr = extractPropertyValue(fileContents, "irrigationBeginHours");
      String irrigationDurationHoursStr = extractPropertyValue(fileContents, "irrigationDurationHours");
      String mistPeriodMinutesStr = extractPropertyValue(fileContents, "mistPeriodMinutes");
      String mistDurationSecondsStr = extractPropertyValue(fileContents, "mistDurationSeconds");

//      Serial.print("irrigationBeginHours: "); Serial.println(irrigationBeginHoursStr);
//      Serial.print("irrigationDurationHours: "); Serial.println(irrigationDurationHoursStr);
//      Serial.print("mistPeriodMinutes: "); Serial.println(mistPeriodMinutesStr);
//      Serial.print("mistDurationSeconds: "); Serial.println(mistDurationSecondsStr);

      schedule.irrigationBeginHours = irrigationBeginHoursStr.toInt();
      schedule.irrigationDurationHours = irrigationDurationHoursStr.toInt();
      schedule.mistPeriodMinutes = mistPeriodMinutesStr.toInt();
      schedule.mistDurationSeconds = mistDurationSecondsStr.toInt();
      isConfigFileValid = true;
    }
    else
    {
      Serial.println("File open failed. ");
    }
  }
  else
  {
    Serial.println(F("Unable to mount flash file system."));
    Serial.flush();
    abort();
  };
  return isConfigFileValid;
}

// Write configuration to flash file system.
// Could also use the nvram in the clock chip or an SD card.
boolean writeConfigToFS(const String &ssid, const String &wifiPassword, const Schedule &schedule)
{
  boolean isConfigFileValid = false;
  bool isFSMounted = LittleFS.begin();
  if (isFSMounted)
  {
    File fileHandle = LittleFS.open(F("config.txt"), "w");
    if (fileHandle)
    {
      String fileContents;

      fileContents += createPropertyValue(F("SSID"), ssid);
      fileContents += createPropertyValue(F("password"), wifiPassword);
      
      fileContents += createPropertyValue(F("irrigationBeginHours"), schedule.irrigationBeginHours);
      fileContents += createPropertyValue(F("irrigationDurationHours"), schedule.irrigationDurationHours);
      fileContents += createPropertyValue(F("mistPeriodMinutes"), schedule.mistPeriodMinutes);
      fileContents += createPropertyValue(F("mistDurationSeconds"), schedule.mistDurationSeconds);

      //Serial.print("File contents: "); Serial.println(fileContents);

      // Write an byte as an int at a time, then treat it as a char.
      if(fileHandle.print(fileContents))
      {
        isConfigFileValid = true;
      }
      else
      {
         Serial.println("Configuration write failed");
      }
    }
    else
    {
      Serial.println("Configuration file open failed. ");
    }
  }
  else
  {
    Serial.println(F("Unable to mount flash file system."));
    Serial.flush();
    abort();
  };
  return isConfigFileValid;
}

//---------------------- Section: Credential Functions -------------------

// Extract configuration from file system.
boolean readCredentialsFromFS(const String &webUserName, String &userDetails)
{
  // Serial.print("Read input user details: "); Serial.println(userDetails); 
  boolean isCredentialsFileValid = false;
  bool isFSMounted = LittleFS.begin();
  if (isFSMounted)
  {
    File fileHandle = LittleFS.open(F("users.txt"), "r");
    if (fileHandle)
    {
      String fileContents;
      // Read an byte as an int at a time, then treat it as a char.
      while (fileHandle.available())
      {
        fileContents += static_cast<char>(fileHandle.read());
      }
      userDetails = extractPropertyValue(fileContents, webUserName);
 
      //Serial.print("Stored password hash: "); Serial.println(userDetails); 
      isCredentialsFileValid = true;
    }
    else
    {
      Serial.println("Credentials file open failed. ");
    }
  }
  else
  {
    Serial.println(F("Unable to mount flash file system."));
    Serial.flush();
    abort();
  };
  return isCredentialsFileValid;
}

// Write credentials to flash file system.
// Could also use the nvram in the clock chip or an SD card.
boolean writeCredentialsToFS(const char *webUserNames[], const char *webUserDetails[])
{
  boolean isCredentialsFileValid = false;
  bool isFSMounted = LittleFS.begin();
  if (isFSMounted)
  {
    File fileHandle = LittleFS.open(F("users.txt"), "w");
    if (fileHandle)
    {
      String fileContents;
      int userNameCount = sizeof(webUserNames) > 0 ? sizeof(webUserNames)/sizeof(webUserNames[0]) : 0; 
      for (int userNameIndex = 0; userNameIndex < userNameCount; userNameIndex++)
      {
        const char *webUserName = webUserNames[userNameIndex];
        const char *webUserDetail = webUserDetails[userNameIndex];
        fileContents += createPropertyValue(webUserName, webUserDetail);
      }

      //Serial.print("File contents: "); Serial.println(fileContents);

      // Write whole contents.
      if(fileHandle.print(fileContents.c_str()))
      {
        isCredentialsFileValid = true;
      }
      else
      {
         Serial.println("Credentials write failed");
      }
    }
    else
    {
      Serial.println("Credentials file open failed. ");
    }
  }
  else
  {
    Serial.println(F("Unable to mount flash file system."));
    Serial.flush();
    abort();
  };
  return isCredentialsFileValid;
}

//---------------------- Section: File Utilities -------------------

// Generate a property name/value pair as a string.
// Property collection is a string with format:
// name0: value0
// name1: value1
// name2: value2
// (blank line optional)

String createPropertyValue(const String &nameStr, const String &valueStr)
{
  return nameStr + F(": ") + valueStr + F("\r\n");
}

String createPropertyValue(const String &nameStr, int valueInt)
{
  return createPropertyValue(nameStr, String(valueInt));
}

// Supply a property name without the colon delimiter.
// Property collection is a string with format:
// name0: value0
// name1: value1
// name2: value2
// (blank line optional)
String extractPropertyValue(const String &propertyCollection, const String &propertyName)
{
    String value;
    
    // Find the name, to extract the value.
    String propertyMarker = propertyName  + ":";
    
    // Serial.print("propertyMarker: "); Serial.println(propertyMarker);
    // Serial.print("propertyCollection: "); Serial.println(propertyCollection);

    int propertyIndex = propertyCollection.indexOf(propertyMarker);
    if (propertyIndex != -1)
    {
      int valueIndex = propertyIndex + propertyMarker.length();
      int lineBreakIndex = propertyCollection.indexOf('\r', valueIndex);
      if (lineBreakIndex == -1)
      {
        // The last property in a file with no final CR/LF 
        value = propertyCollection.substring(valueIndex);
      }
      else
      {
        // Most properties end in CR/LF. 
        value = propertyCollection.substring(valueIndex, lineBreakIndex);
      }
      value.trim();
    }
    else
    {
      // Need to fix the file contents if this message occurs.
      Serial.print(F("Property not found in file: ")); Serial.println(propertyName);
    }
    return value;
}

//---------------------- Section: Web Server Functions -------------------

IPAddress initNetworkConnection(const String &ssid, const String &wifiPassword)
{
  IPAddress localIPAddress;
  
  // Connect to WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Connecting to: "); Serial.println(ssid);
 
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, wifiPassword);
 
  // Poll for connection.
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected.");
 
  // Start the server
  server.begin();
  Serial.println("Server started.");
 
  // Print the URL handed out by DHCP.
  // The end user copies and pastes this string into a browser to get started.
  localIPAddress = WiFi.localIP();
  Serial.print("Browser URL: ");
  Serial.print("http://"); Serial.print(localIPAddress); Serial.println("/");
  
  return localIPAddress;
}

// Read browser request.

int handleRequest(WiFiClient &browser, bool &relayState)
{
  int automationStatus = NULL_STATE;

  String request = browser.readStringUntil('\r');
  if (isAuthenticated(browser))
  {
    Serial.println(F("Authenticated."));
    // Extract the HTTP request as the first line of the request header.
    Serial.println(request);
    if (request.indexOf("GET") == 0)
    {
      Serial.println(F("Get request"));
      // Interpret the HTTP request and set the corresponding relay state.
      String target = extractTarget(request);
      if (target == "/RELAY=ON")  
      {
        automationStatus = MANUAL_ACTIVE;
        relayState = RELAY_ON;
      }
      else if (target == "/RELAY=OFF")  
      {
        automationStatus = MANUAL_ACTIVE;
        relayState = RELAY_OFF;
      }
      else if (target == "/RELAY=AUTO_START")  
      {
        automationStatus = AUTO_START;
       }
      else if (target == "schedule=Submit")  
      {
        automationStatus = AUTO_CONFIG;
      }
      else
      {
        // Do nothing
        // Ignore any other kind of request including
        // the favicon request from client browser.
        automationStatus = AUTO_RUN;
      }
      // Regenerate and send web page regardless of 
      // the validity of the request.
      // Use the stored relay state.
      browser.println(generateResponse(relayState, automationStatus, currentSchedule));
    }
    else if (request.indexOf("POST") == 0)
    {
      Serial.println(F("Post request"));
      String target = extractTarget(request);
      if (target == F("/update"))
      {
        // Root path of site.
      }
      else
      {
         browser.println(generatePageNotFoundResponse());
      }
    }
  }
  else
  {
      Serial.println(F("Not authenticated."));
      String target = extractTarget(request);
      if (target == F("/login"))
      {
        browser.println(generateLoginResponse());
      }
      else if (target == F("/authenticate"))
      {
        String body = extractPostQuery(browser);
        String userName = extractQueryValue(body, F("userName"));
        String password = extractQueryValue(body, F("password"));

        String knownHash;
        boolean foundCredentials = readCredentialsFromFS(userName, knownHash);       
        if (foundCredentials)
        {
          String passwordHash;
          bool isHashed = hasher(password, passwordHash);
          if(isHashed)
          { 
            Serial.println(F("Match hashed passwords: ")); 
            Serial.print(knownHash);  Serial.println(F("<->")); 
            Serial.println(passwordHash);
            
             if (knownHash == passwordHash)
            {
              // Set session id cookie and send to the root page.
              String sessionId;
              if(createSessionId(sessionId))
              {
                storeSessionId(webUserName, sessionId);
                browser.println(generateRedirectResponseToRoot(sessionId));        
              }
              else
              {
                // Send to the login page.
                browser.println(generateRedirectResponseToLogin());        
              }
            }
          }
          else
          {
             Serial.print(F("Failed web password hash. ")); 
          }
        }
      }
      else
      {
        // Send to the login page.
        browser.println(generateRedirectResponseToLogin());
      }
  }


  // Spin wheels to pick up content until browser stops sending.
  while (browser.available())
  {
    browser.read();
  }
  return automationStatus;
}

bool isAuthenticated(WiFiClient &browser)
{
  String sessionId = retrieveSessionId(webUserName);
  String cookie = extractCookie(browser);
  Serial.print(F("Cookie from request: ")); Serial.println(cookie); 
  return sessionId.length() > 0 && cookie.indexOf(sessionId) != -1;
}

String extractTarget(const String &request)
{
  int indexStartTarget = request.indexOf(F(" ")) + 1;
  int indexEndTarget = request.indexOf(F(" "), indexStartTarget);
  String target = request.substring(indexStartTarget, indexEndTarget);
  Serial.print(F("Range: ")); Serial.print(indexStartTarget); Serial.print(F(" to ")); Serial.println(indexEndTarget); 
  Serial.print(F("Target: ")); Serial.println(target);
  return target;
}

String extractPostQuery(WiFiClient &browser)
{
  // At this point the header lines have all been consumed,
  // so only the message body remains.
  String body = browser.readStringUntil('\r');
  body.trim(); // Remove leading line feed.
  Serial.print(F("Body line: ")); Serial.println(body); 
  return body;
}

String extractBody(WiFiClient &browser)
{
  // At this point the header lines have all been consumed,
  // so only the message body remains.
  String body;
  String messagePart;
  messagePart = browser.readStringUntil('\r');
  messagePart.trim(); // Remove leading line feed.
  Serial.print(F("Body line: ")); Serial.println(messagePart); 
  body += messagePart + F("\r\n");
  while (messagePart.length() > 0)
  {
    messagePart = browser.readStringUntil('\r');
    messagePart.trim(); // Remove leading line feed.
    Serial.print(F("Header line: ")); Serial.println(messagePart); 
  }
  body = messagePart;
  Serial.print(F("Body line: ")); Serial.println(messagePart); 
  return body;
}

String extractCookie(WiFiClient &browser)
{
  // At this point the query line has already been read,
  // so the header lines come next.
  String cookieValue;
  String messagePart;
  do {
   // Read all header lines and extract cookie value.
    messagePart = browser.readStringUntil('\r');
    messagePart.trim(); // Remove leading line feed.
    Serial.print(F("Header line: ")); Serial.println(messagePart); 
    if (messagePart.startsWith(F("Cookie: ")))
    {
      String cookieName = F("Cookie: ");
      int indexStart = cookieName.length();
      cookieValue = messagePart.substring(indexStart);
      cookieValue.trim();
    }
   }   while (messagePart.length() > 0);

  // The message body is next.
  Serial.print(F("Cookie: ")); Serial.println(cookieValue); 
  return cookieValue;
}

bool createSessionId(String &result)
{
  uint32_t randomNumber = os_random();
  String inputString = webUserName + localIPAddress.toString() + randomNumber;
  bool isHashed = hasher(inputString, result);
  if(!isHashed)
  {
    Serial.print(F("Session id input: ")); Serial.println(inputString); 
  }
  result = result.substring(0, 32);
  return isHashed;
}

bool hasher(const String &inputString, String &result)
{
  bool isHashed = true;

  Serial.print(F("Hash input: ")); Serial.println(inputString); 

  // Define inputs.
  size_t inputLength = inputString.length();
  const uint8_t *inputChars =  reinterpret_cast<const uint8_t *>(inputString.c_str());

  // Define outputs.
  psa_status_t errorCode = PSA_SUCCESS;
  uint8_t hashResult[32] = {0};
  size_t hashCount = 0;

  // Calculate.
  errorCode = psa_hash_compute(PSA_ALG_SHA_256, inputChars, inputLength, hashResult, sizeof(hashResult), &hashCount);

  isHashed = errorCode == PSA_SUCCESS;
  if (isHashed)
  {
    for (int hashIndex = 0; hashIndex < hashCount; hashIndex++)
    {
      uint8_t hashChar = hashResult[hashIndex];
      // Convert into decimal represention and concatenate.
      result += hashChar;
    }
  }
  else
  {
    Serial.print(F("Hash error code: ")); Serial.println(errorCode); 
  }
  return isHashed;
}

void storeSessionId(const String &webUserName, const String &sessionId)
{
  sessionIdRepository = sessionId;
}

String retrieveSessionId(const String &webUserName)
{
//  String webPasswordHash;
//  readCredentialsFromFS(webUserName, webPasswordHash);
//  return webPasswordHash;
  Serial.print(F("Stored session id: ")); Serial.println(sessionIdRepository); 
  return sessionIdRepository;
}

String hexString(const uint8_t byteValue)
{
  static char hexChars[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  int hiNybble = byteValue >> 4;
  int loNybble = byteValue & 0b00001111;
  return String(hexChars[hiNybble]) + String(hexChars[loNybble]);
}

// Supply a property name.
// Query collection is a string with format:
// name0=value0&name1=value1&name2:=value2

String extractQueryValue(const String &queryCollection, const String &propertyName)
{
    String value;
    
    // Find the name, to extract the value.
    String propertyMarker = propertyName  + "=";
    
    //Serial.print("propertyMarker: "); Serial.println(propertyMarker);
    //Serial.print("queryCollection: "); Serial.println(queryCollection);

    int propertyIndex = queryCollection.indexOf(propertyMarker);
    if (propertyIndex != -1)
    {
      int valueIndex = propertyIndex + propertyMarker.length();
      int lineBreakIndex = queryCollection.indexOf('&', valueIndex);
      if (lineBreakIndex == -1)
      {
        // The last property in a file with no final CR/LF 
        value = queryCollection.substring(valueIndex);
      }
      else
      {
        // Most properties end in CR/LF. 
        value = queryCollection.substring(valueIndex, lineBreakIndex);
      }
    }
    else
    {
      // Need to fix the file contents if this message occurs.
      Serial.print(F("Property not found in query: ")); Serial.println(propertyName);
    }
    return value;
}

String generateLoginResponse()
{
  String htmlPage;
  htmlPage.reserve(1024);

  String httpHeader = F("HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Connection: close\r\n"
                      "\r\n");

  String htmlStyle = F("<style type=\"text/css\">"
                        ".fieldset-auto-width {display: inline-block; }"
                        ".div-auto-width {display: inline-block; }"
                        ".force-right {text-align: right;}"
                        "fieldset {text-align: right;}"
                        "legend {float: left;}"
                        "input {margin: 2px;}"
                       "</style>");

  String htmlStart = F("<!DOCTYPE HTML>"
                      "<html>"
                      "<head><title>Irrigation Control Authentication</title></head>");

  String htmlForm = generateLoginForm(); 

  String htmlEnd = F("</html>"
                  "\r\n");

  htmlPage = httpHeader + htmlStyle + htmlStart + htmlForm + htmlEnd;
 
  return htmlPage;
}

// Generate a "not found" web page as a response.

String generatePageNotFoundResponse()
{
  String htmlPage;
  htmlPage.reserve(1024);

  String httpHeader = F("HTTP/1.1 404 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Connection: close\r\n"
                      "\r\n");

  String htmlStyle = F("<style type=\"text/css\">"
                        ".fieldset-auto-width {display: inline-block; }"
                        ".div-auto-width {display: inline-block; }"
                        ".force-right {text-align: right;}"
                        "fieldset {text-align: right;}"
                        "legend {float: left;}"
                        "input {margin: 2px;}"
                       "</style>");

  String htmlStart = F("<!DOCTYPE HTML>"
                      "<html>"
                      "<head><title>Page not found. </title></head>");

  String htmlEnd = F("</html>"
                  "\r\n");

  htmlPage = httpHeader + htmlStyle + htmlStart + htmlEnd;
 
  return htmlPage;
}

// Generate a "redirect" to login web page as a response.

String generateRedirectResponseToLogin()
{
  String htmlPage;
  htmlPage.reserve(1024);

  String httpHeader = F("HTTP/1.1 302 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Connection: close\r\n"
                      "Location: /login\r\n"
                      "\r\n");

  htmlPage = httpHeader;
 
  return htmlPage;
}
// Generate a "redirect" to root web page as a response.

String generateRedirectResponseToRoot(const String &sessionId)
{
  String htmlPage;
  htmlPage.reserve(1024);

  String httpHeader = F("HTTP/1.1 302 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Connection: close\r\n"
                      "Location: /\r\n");
  httpHeader += F("Set-cookie: sessionId=") + sessionId + F("\r\n");
  httpHeader += F("\r\n");

  htmlPage = httpHeader;
 
  return htmlPage;
}

String generateResponse(bool relayState, int automationStatus, const Schedule& schedule)
{
  String RELAY_ON_LABEL = F("ON");
  String RELAY_OFF_LABEL = F("OFF");

  String htmlPage;
  htmlPage.reserve(1024);

  String httpHeader = F("HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Connection: close\r\n"
                      "\r\n");

  String htmlStyle = F("<style type=\"text/css\">"
                        ".fieldset-auto-width {display: inline-block; }"
                        ".div-auto-width {display: inline-block; }"
                        ".force-right {text-align: right;}"
                        "fieldset {text-align: right;}"
                        "legend {float: left;}"
                        "input {margin: 2px;}"
                       "</style>");

  String htmlStart = F("<!DOCTYPE HTML>"
                      "<html>"
                      "<head><title>Irrigation Control</title></head>");

  String relayStateStr = relayState == RELAY_ON ? RELAY_ON_LABEL : RELAY_OFF_LABEL;
  String htmlTitle = F("Manual water delivery is now: ") + relayStateStr; 
  bool isShowingSchedule = automationStatus == AUTO_START || automationStatus == AUTO_RUN;
  String htmlSchedule = isShowingSchedule ? generateSchedule(schedule) : F("<br>Schedule inactive.");
  String htmlForm = generateForm(schedule); 
  String htmlControl = F("<br><br>"
                      "Turn <a href=\"/RELAY=OFF\">OFF</a> water supply.<br>"
                      "Turn <a href=\"/RELAY=ON\">ON</a> water supply.<br>"
                      "Activate misting schedule <a href=\"/RELAY=AUTO_START\">AUTO</a>.<br>");

  String htmlEnd = F("</html>"
                  "\r\n");

  htmlPage = httpHeader + htmlStyle + htmlStart + htmlTitle + htmlSchedule + htmlControl + htmlForm + htmlEnd;
 
  return htmlPage;
}

String generateSchedule(const Schedule& schedule)  
{
  String htmlSchedule = F("<br>Schedule: ");
  String scheduleItems[] = 
    {
      F("&nbsp;Irrigation begin (hour): ") + String(schedule.irrigationBeginHours),
      F("&nbsp;Irrigation duration (hours): ") + String(schedule.irrigationDurationHours),
      F("&nbsp;Mist period (minutes): ") + String(schedule.mistPeriodMinutes),
      F("&nbsp;Mist duration (seconds): ") + String(schedule.mistDurationSeconds)
    };

  int itemIndex;
  int itemCount = 0;
  if (sizeof(scheduleItems) > 0)
  {
    itemCount = sizeof(scheduleItems)/sizeof(scheduleItems[0]);
  }
  for (int itemIndex = 0; itemIndex < itemCount; itemIndex++)
  {
    htmlSchedule +=  F("<br>") + scheduleItems[itemIndex];
  } 
  return htmlSchedule;
}

String generateLoginForm()  
{
  String htmlForm = F("<br>Login: <br>");
    
  String scheduleLabels[] = 
    {
      F("&nbsp;User name:&nbsp;"),
      F("&nbsp;Password:&nbsp;")
    };
    
  String scheduleNames[] = 
    {
      F("userName"),
      F("password")
    };

  String htmlFields;
  int itemIndex;
  int itemCount = 0;
  if (sizeof(scheduleLabels) > 0)
  {
    itemCount = sizeof(scheduleLabels)/sizeof(scheduleLabels[0]);
  }
  for (int itemIndex = 0; itemIndex < itemCount; itemIndex++)
  {
    String htmlInput =  F("<input type=\"text\" name=\"") + scheduleNames[itemIndex] + F("\"") + F(" value=\"\">");
    htmlFields +=  F("<label for=\"") + scheduleNames[itemIndex] + F("\">") + scheduleLabels[itemIndex] + htmlInput  + F("</label>") + F("<br>");
  } 
  String htmlFieldSet = F("<fieldset class=\"fieldset-auto-width\">") + htmlFields + F("</fieldset>");
 
  String htmlSubmit =  F("<div class=\"force-right\"><input type=\"submit\" name=\"login\" value=\"Submit\"></div>");
  htmlForm += F("<form name=\"LOGIN\" action=\"/authenticate\" method=\"post\">") + htmlFieldSet + htmlSubmit + F("</form>");
  htmlForm = F("<div class=\"div-auto-width\">") + htmlForm + F("</div>");
  return htmlForm;
}

String generateForm(const Schedule& schedule)  
{
  String htmlForm = F("<br>Edit schedule: <br>");
  String scheduleValues[] = 
    {
      String(schedule.irrigationBeginHours),
      String(schedule.irrigationDurationHours),
      String(schedule.mistPeriodMinutes),
      String(schedule.mistDurationSeconds)
    };
    
  String scheduleLabels[] = 
    {
      F("&nbsp;Irrigation begin (hour):&nbsp;"),
      F("&nbsp;Irrigation duration (hours):&nbsp;"),
      F("&nbsp;Mist period (minutes):&nbsp;"),
      F("&nbsp;Mist duration (seconds):&nbsp;")
    };
    
  String scheduleNames[] = 
    {
      F("irrigationBeginHours"),
      F("irrigationDurationHours"),
      F("mistPeriodMinutes"),
      F("mistDurationSeconds")
    };

  String htmlFields;
  int itemIndex;
  int itemCount = sizeof(scheduleValues)/sizeof(scheduleValues[0]);
  for (int itemIndex = 0; itemIndex < itemCount; itemIndex++)
  {
    String htmlInput =  F("<input type=\"text\" name=\"") + scheduleNames[itemIndex] + F("\"") + F(" value=\"") + scheduleValues[itemIndex] + F("\">");
    htmlFields +=  F("<label for=\"") + scheduleNames[itemIndex] + F("\">") + scheduleLabels[itemIndex] + htmlInput  + F("</label>") + F("<br>");
  } 
  // String htmlLegend = F("<legend>Edit schedule: </legend>");
  String htmlFieldSet = F("<fieldset class=\"fieldset-auto-width\">") + htmlFields + F("</fieldset>");
 
  String htmlSubmit =  F("<div class=\"force-right\"><input type=\"submit\" name=\"schedule\" value=\"Submit\"></div>");
  htmlForm += F("<form name=\"SCHEDULE\" action=\"/\" method=\"get\">") + htmlFieldSet + htmlSubmit + F("</form>");
  htmlForm = F("<div class=\"div-auto-width\">") + htmlForm + F("</div>");
  return htmlForm;
}


//---------------------- Section: Application Functions -------------------

bool updateSchedule(const String &request, Schedule &newSchedule)
{
  bool isUpDateable = false;
  Schedule schedule;
  
  if(interpretScheduleRequest(request, schedule))
  {
    if(validateIrrigationSchedule(schedule))
    {
      newSchedule = schedule;
      isUpDateable = true;
    }
    else
    {
      Serial.println("The irrigation schedule is outside the acceptable range. ");
    }
  }
  return isUpDateable;
}

bool interpretScheduleRequest(const String &request, Schedule &schedule)
{
  bool isValidSchedule = false;
  
  int beginQueryIndex = request.indexOf("?");
  int endQueryIndex = request.indexOf("HTTP");
  String queryCollection;
  if (beginQueryIndex != -1 && endQueryIndex != -1)
  {
    queryCollection = request.substring(beginQueryIndex + 1, endQueryIndex - 1);
    //Serial.println(queryCollection); Serial.print(beginQueryIndex); Serial.print(F(" to ")); Serial.println(endQueryIndex);
    queryCollection.trim();
    //Serial.println(queryCollection);
    
    String irrigationBeginHoursStr = extractQueryValue(queryCollection, "irrigationBeginHours");
    String irrigationDurationHoursStr = extractQueryValue(queryCollection, "irrigationDurationHours");
    String mistPeriodMinutesStr = extractQueryValue(queryCollection, "mistPeriodMinutes");
    String mistDurationSecondsStr = extractQueryValue(queryCollection, "mistDurationSeconds");

    if (isValidInteger(irrigationBeginHoursStr))
    {
      schedule.irrigationBeginHours = irrigationBeginHoursStr.toInt();
      if (isValidInteger(irrigationDurationHoursStr))
      {
        schedule.irrigationDurationHours = irrigationDurationHoursStr.toInt();
        if (isValidInteger(mistPeriodMinutesStr))
        {
          schedule.mistPeriodMinutes = mistPeriodMinutesStr.toInt();
          if (isValidInteger(mistDurationSecondsStr))
          {
            schedule.mistDurationSeconds = mistDurationSecondsStr.toInt();
            isValidSchedule = true;
            //Serial.println(F("Found schedule:"));
            //Serial.println(schedule.irrigationBeginHours);
            //Serial.println(schedule.irrigationDurationHours);
            //Serial.println(schedule.mistPeriodMinutes);
            //Serial.println(schedule.mistDurationSeconds);
          }
          else
          {
            Serial.print(F("Not a valid integer: ")); Serial.println(mistDurationSecondsStr);
          }
        }        
        else
        {
          Serial.print(F("Not a valid integer: ")); Serial.println(mistPeriodMinutesStr);
        }
      }
      else
      {
        Serial.print(F("Not a valid integer: ")); Serial.println(irrigationDurationHoursStr);
      }
    }
    else
    {
      Serial.print(F("Not a valid integer: ")); Serial.println(irrigationBeginHoursStr);
    }
  }
  else 
  {
    Serial.print(F("The request is malformed: ")); Serial.println(request);
  } 
  return isValidSchedule;
}

// Checks numerical ranges.
bool validateIrrigationSchedule(const Schedule &schedule)
{
  bool isSuccess = true;
   Serial.print(F("Validating irrigationBeginHours = ")); Serial.println(schedule.irrigationBeginHours);
  if (!(0 <= schedule.irrigationBeginHours && schedule.irrigationBeginHours < 24))
  {
    Serial.println(F("irrigationBeginHours must be in the range 0 to 23."));
    isSuccess = false;
  }

  Serial.print(F("Validating irrigationDurationHours = ")); Serial.println(schedule.irrigationDurationHours);
  if (!(0 < schedule.irrigationDurationHours && (schedule.irrigationBeginHours + schedule.irrigationDurationHours < 24)))
  {
    Serial.println(F("IrrigationDurationHours must be greater than 0 and the calculated end hours must be less than 23."));
    isSuccess = false;
  }

  Serial.print(F("Validating mistPeriodMinutes = ")); Serial.println(schedule.mistPeriodMinutes);
  int irrigationDurationMinutes = schedule.irrigationDurationHours * 60;
  if (!(0 < schedule.mistPeriodMinutes && schedule.mistPeriodMinutes < irrigationDurationMinutes))
  {
    Serial.println(F("MistPeriodMinutes must be greater than 0 and less than ") + String(irrigationDurationMinutes) + F("."));
    isSuccess = false;
  }

  Serial.print(F("Validating mistDurationSeconds = ")); Serial.println(schedule.mistDurationSeconds);
  if (!(0 < schedule.mistDurationSeconds && schedule.mistDurationSeconds < 60))
  {
    Serial.println(F("MistDurationSeconds must be greater than 0 and less than 60."));
    isSuccess = false;
  }
  return isSuccess;
}

bool isValidInteger(const String &potentialInteger)
{
  bool isValid = true;
  int characterIndex;
  int characterCount = potentialInteger.length();
  for(characterIndex = 0; characterIndex < characterCount; characterIndex++)
  {
    if(!isDigit(potentialInteger.charAt(characterIndex)))
    {
      isValid = false;
      break;
    }
  }
  return isValid;
}

void initIrrigationSchedule(const Schedule &schedule)
{
  waterToday.begin = schedule.irrigationBeginHours;
  waterToday.end = schedule.irrigationBeginHours + schedule.irrigationDurationHours;
  isPreviousMisting = false;
  Serial.println(F("Initialising irrigation schedule.")) ;
}

void irrigate(const Schedule &schedule)
{ 
  DateTime now = rtc.now();
  int currentHour = now.hour();
  int currentSeconds = now.second() + now.minute() * 60;

  static int reportIndex;

  bool isIrrigating = waterToday.begin <= currentHour && currentHour < waterToday.end;

  #ifdef REPORT_IRRIGATION
  if (reportIndex >= 10000)
  {
   Serial.println(F("Irrigating? ") + String(isIrrigating) + F(" - ") + String(now.hour()) + F(":") + String(now.minute()) + F(":") + String(now.second())) ;
   reportIndex = 0;
  }
  else
  {
    reportIndex++;
  } 
  #endif

  if (isIrrigating)
  {
    bool isMisting = false;
    mistToday.begin = 0;
    mistToday.end = schedule.mistDurationSeconds;
    int irrigationDurationSeconds = schedule.irrigationDurationHours * 60 * 60;
    while (mistToday.end < irrigationDurationSeconds)
    {
       isMisting = mistToday.begin <= currentSeconds && currentSeconds < mistToday.end;
      if (!isMisting)
      {
        //Keep searching for another misting period.
        int mistPeriodSeconds = schedule.mistPeriodMinutes * 60;
        mistToday.begin += mistPeriodSeconds;
        mistToday.end += mistPeriodSeconds;
      }
      else
      {
        // Found a misting period.
        break;
      }
    }
    if (isPreviousMisting && !isMisting || !isPreviousMisting && isMisting)
    {
      // On entering or leaving the misting state adjust the irrigation valve state. 
      mist(isMisting);
      isPreviousMisting = isMisting;
    }
  }  
}

void mist(bool isMisting)
{
  switchRelay(isMisting);
  String mistMessage = isMisting ? F("on: ") : F("off: ");
  DateTime now = rtc.now();
  Serial.println(F("Misting ") + mistMessage + String(now.hour()) + F(":") + String(now.minute()) + F(":") + String(now.second())) ;
}
