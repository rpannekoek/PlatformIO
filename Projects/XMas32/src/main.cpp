#include <Arduino.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiNTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <LED.h>
#include <Log.h>
#include <WiFiStateMachine.h>
#include <HtmlWriter.h>
#include <Navigation.h>
#include <FastLED.h>
#include <Ticker.h>
#include "PersistentData.h"

constexpr uint8_t RGB_LED_PIN = 32; // TODO
constexpr int BLINK_INTERVAL = 5;

constexpr size_t LIGHT_FX_COUNT = 4;
const char* LightFXNames[] = { "None", "Flame", "Blink", "Spectrum" };

const char* ButtonClass = "button";
const char* ContentTypeHtml = "text/html";

enum FileId
{
    Logo,
    Styles,
    HomeIcon,
    LogFileIcon,
    SettingsIcon,
    _LastFile
};

const char* Files[] =
{
    "Logo.png",
    "styles.css",
    "Home.svg",
    "LogFile.svg",
    "Settings.svg",
};

SimpleLED BuiltinLED(LED_BUILTIN, true);
ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
StringBuilder HttpResponse(24 * 1024);
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles]);
StringLog EventLog(50, 96);
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Navigation Nav;
Ticker LightFXTicker;
CRGB LEDColors[RGB_LED_COUNT];
CHSV LEDColorsHSV[RGB_LED_COUNT];
int scheduleIndexes[MAX_SCHEDULES];

time_t currentTime = 0;
time_t startOfDay = 0;
time_t updateScheduleTime = 0;
int blinkCounter = 0;


void updateSchedule()
{
    if (currentTime >= startOfDay + SECONDS_PER_DAY)
    {
        startOfDay = getStartOfDay(currentTime);
        for (int i = 0; i < MAX_SCHEDULES; i++)
            scheduleIndexes[i] = -1;
    }

    uint16_t currentScheduleTime = (currentTime - startOfDay) / 60;
    for (int i = 0; i < MAX_SCHEDULES; i++)
    {
        int scheduleIndex = scheduleIndexes[i];
        if (scheduleIndex == MAX_SCHEDULE_ENTRIES - 1) continue;
        ScheduleEntry& scheduleEntry = PersistentData.scheduleEntries[i][scheduleIndex + 1];
        if (scheduleEntry.time != 0 && currentScheduleTime >= scheduleEntry.time)
            scheduleIndexes[i] = ++scheduleIndex;
    }
}


void updateLEDs()
{
    Tracer tracer("updateLEDs");

    for (int i = 0; i < RGB_LED_COUNT; i++)
    {
        uint8_t scheduleId = PersistentData.ledSchedules[i];
        int scheduleIndex = scheduleIndexes[scheduleId];
        if (scheduleIndex < 0) continue;
        ScheduleEntry& scheduleEntry = PersistentData.scheduleEntries[scheduleId][scheduleIndex];
        LEDColors[i] = scheduleEntry.color;
        LEDColorsHSV[i] = rgb2hsv_approximate(scheduleEntry.color);
    }
    FastLED.show();
}


void runLightFX()
{
    bool colorsChanged = false;
    for (int i = 0; i < RGB_LED_COUNT; i++)
    {
        uint8_t scheduleId = PersistentData.ledSchedules[i];
        int scheduleIndex = scheduleIndexes[scheduleId];
        if (scheduleIndex < 0) continue;
        ScheduleEntry& scheduleEntry = PersistentData.scheduleEntries[scheduleId][scheduleIndex];
        CRGB& ledColor = LEDColors[i];
        switch (scheduleEntry.lightFX)
        {
            case LightFX::None:
                break;

            case LightFX::Flame:
            {
                uint8_t scale = random((255 - scheduleEntry.lightFXparam), 256);
                ledColor = scheduleEntry.color.scale8(scale);
                colorsChanged = true;
                break;
            }

            case LightFX::Blink:
            {
                if (++blinkCounter == BLINK_INTERVAL)
                {
                    ledColor = scheduleEntry.color.scale8(255 - scheduleEntry.lightFXparam);
                    blinkCounter = -BLINK_INTERVAL;
                    colorsChanged = true;
                }
                else if (blinkCounter == 0)
                {
                    ledColor = scheduleEntry.color;
                    colorsChanged = true;
                }
                break;
            }

            case LightFX::Spectrum:
            {
                LEDColorsHSV[i].hue += scheduleEntry.lightFXparam;
                ledColor = LEDColorsHSV[i];
                colorsChanged = true;
                break;
            }
        }
    }

    if (colorsChanged) FastLED.show();
}


void onTimeServerSynced()
{
    updateSchedule();
    updateLEDs();
    LightFXTicker.attach_ms(100, runLightFX);
}


void onWiFiInitialized()
{
    if (currentTime >= updateScheduleTime)
    {
        updateScheduleTime = currentTime + SECONDS_PER_MINUTE;
        updateSchedule();
        updateLEDs();
    }
}


void handleHttpEventLogRequest()
{
    Tracer tracer("handleHttpEventLogRequest");

    Html.writeHeader("Event log", Nav);

    if (WiFiSM.shouldPerformAction("clear"))
    {
        EventLog.clear();
        WiFiSM.logEvent("Event log cleared.");
    }

    for (const char* event : EventLog)
        Html.writeDiv("%s", event);


    Html.writeActionLink("clear", "Clear event log", currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormRequest()
{
    Tracer tracer("handleHttpConfigFormRequest");

    Html.writeHeader("Settings", Nav);

    Html.writeFormStart("/config", "grid");
    PersistentData.writeHtmlForm(Html);
    Html.writeSubmitButton("Save");
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction("reset"))
    {
        Html.writeParagraph("Resetting...");
        WiFiSM.reset();
    }
    else
        Html.writeActionLink("reset", "Reset ESP", currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormPost()
{
    Tracer tracer("handleHttpConfigFormPost");

    PersistentData.parseHtmlFormData([](const String& id) -> String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void writeScheduleControls(const String& name, int scheduleId, std::function<void(ScheduleEntry&, String&)> writeControls)
{
    Html.writeHeaderCell(name);
    for (int i = 0; i < MAX_SCHEDULE_ENTRIES; i++)
    {
        String id = String(scheduleId) + "_";
        id += String(i);
        String cssClass = "schedule";
        if (i == scheduleIndexes[scheduleId]) cssClass += " current";
 
        Html.writeCellStart(cssClass);
        writeControls(PersistentData.scheduleEntries[scheduleId][i], id);
        Html.writeCellEnd();
    }
}

void handleHttpRootRequest()
{
    Tracer tracer("handleHttpRootRequest");

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }

    int identify = WebServer.hasArg("identify") ? WebServer.arg("identify").toInt() : -1;

    Html.writeHeader("Home", Nav);

    Html.writeDivStart("flex-container");

    Html.writeSectionStart("Status");
    Html.writeTableStart();
    Html.writeRow("WiFi RSSI", "%d dBm", int(WiFi.RSSI()));
    Html.writeRow("Free Heap", "%0.1f kB", float(ESP.getFreeHeap()) / 1024);
    Html.writeRow("Uptime", "%0.1f days", float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeFormStart("");

    Html.writeSectionStart("Schedules");
    Html.writeTableStart();
    for (int i = 0; i < MAX_SCHEDULES; i++)
    {
        Html.writeRowStart();
        Html.writeHeaderCell(String("#") + (i+1), 0, 3);
        writeScheduleControls("Time", i, [](ScheduleEntry& scheduleEntry, String& id) {
            Html.writeNumberBox(String("H") + id, "", scheduleEntry.hours(), 0, 23);
            Html.writeNumberBox(String("M") + id, "", scheduleEntry.minutes(), 0, 59);
            });
        Html.writeRowEnd();
        Html.writeRowStart();
        writeScheduleControls("RGB", i, [](ScheduleEntry& scheduleEntry, String& id) {
            Html.writeNumberBox(String("R") + id, "", scheduleEntry.color.red, 0, 255);
            Html.writeNumberBox(String("G") + id, "", scheduleEntry.color.green, 0, 255);
            Html.writeNumberBox(String("B") + id, "", scheduleEntry.color.blue, 0, 255);
            });
        Html.writeRowEnd();
        Html.writeRowStart();
        writeScheduleControls("FX", i, [](ScheduleEntry& scheduleEntry, String& id) {
            Html.writeDropdown(String("FX") + id, "", LightFXNames, LIGHT_FX_COUNT, int(scheduleEntry.lightFX));
            Html.writeNumberBox(String("FP") + id, "", scheduleEntry.lightFXparam, 0, 255);
            });
        Html.writeRowEnd();
    }
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart("LEDs");
    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("LED");
    Html.writeHeaderCell("RGB");
    Html.writeHeaderCell("Schedule");
    Html.writeRowEnd();

    char rgb[16];
    for (int i = 0; i < RGB_LED_COUNT; i++)
    {
        CRGB& color = LEDColors[i];
        String cssClass;
        if (i == identify)
        {
            cssClass = "current";
            color = CRGB::White;
            FastLED.show();
        }

        Html.writeRowStart();
        Html.writeCellStart(cssClass);
        Html.writeLink(String("?identify=") + i, String("#") + (i+1));
        Html.writeCellEnd();
        snprintf(rgb, sizeof(rgb), "%d,%d,%d", color.red, color.green, color.blue);
        Html.writeCell(rgb);
        Html.writeCellStart("schedule");
        Html.writeNumberBox(
            String("L") + i,
            "",
            PersistentData.ledSchedules[i] + 1,
            1,
            MAX_SCHEDULES);
        Html.writeCellEnd();
        Html.writeRowEnd();
    }
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSubmitButton("Update");
    Html.writeFormEnd();
    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpScheduleFormPost()
{
    Tracer tracer("handleHttpScheduleFormPost");

    for (int i = 0; i < MAX_SCHEDULES; i++)
    {
        for (int n = 0; n < MAX_SCHEDULE_ENTRIES; n++)
        {
            String id = String(i) + "_";
            id += String(n);
            ScheduleEntry& scheduleEntry = PersistentData.scheduleEntries[i][n];
            scheduleEntry.setTime(
                WebServer.arg(String("H") + id).toInt(),
                WebServer.arg(String("M") + id).toInt());
            scheduleEntry.color.red = WebServer.arg(String("R") + id).toInt();
            scheduleEntry.color.green = WebServer.arg(String("G") + id).toInt();
            scheduleEntry.color.blue = WebServer.arg(String("B") + id).toInt();
            scheduleEntry.lightFX = static_cast<LightFX>(WebServer.arg(String("FX") + id).toInt());
            scheduleEntry.lightFXparam = WebServer.arg(String("FP") + id).toInt();
        }
    }

    for (int i = 0; i < RGB_LED_COUNT; i++)
        PersistentData.ledSchedules[i] = WebServer.arg(String("L") + i).toInt() - 1;

    PersistentData.writeToEEPROM();

    updateSchedule();
    updateLEDs();

    handleHttpRootRequest();
}


void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    Serial.println(cmd);

    if (cmd.startsWith("L"))
    {
        for (int i = 0; i < 1000; i++)
        {
            FastLED.show();
            delay(1);
        }
    }
}


// Boot code
void setup() 
{
#ifdef DEBUG_ESP_PORT
    DEBUG_ESP_PORT.begin(115200);
    DEBUG_ESP_PORT.setDebugOutput(true);
    DEBUG_ESP_PORT.println();
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
#endif

    BuiltinLED.begin();

    PersistentData.begin();
    TimeServer.begin(PersistentData.ntpServer);
    Html.setTitlePrefix(PersistentData.hostName);
    
    Nav.menuItems =
    {
        MenuItem
        {
            .icon = Files[HomeIcon],
            .label = "Home",
            .handler = handleHttpRootRequest,
            .postHandler = handleHttpScheduleFormPost            
        },
        MenuItem
        {
            .icon = Files[LogFileIcon],
            .label = "Event log",
            .urlPath ="events",
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[SettingsIcon],
            .label = "Settings",
            .urlPath ="config",
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    };
    Nav.registerHttpHandlers(WebServer);

    WiFiSM.registerStaticFiles(Files, _LastFile);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    /*
    pinMode(RGB_LED_PIN, OUTPUT);
    uint8_t pinState = 0;
    for (int i = 0; i < 99; i++)
    {
        digitalWrite(RGB_LED_PIN, pinState);
        delay(10);
        pinState ^= HIGH;
    }
    */

    for (int i = 0; i < RGB_LED_COUNT; i++)
        LEDColors[i] = CRGB::Gray;

    FastLED.addLeds<WS2812, RGB_LED_PIN, GRB>(LEDColors, RGB_LED_COUNT);
    FastLED.show();

    Tracer::traceFreeHeap();

    BuiltinLED.setOff();    
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    WiFiSM.run();

    if (Serial.available())
        handleSerialRequest();
}
