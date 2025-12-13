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
#include "fx/1d/demoreel100.h"
#include "fx/1d/twinklefox.h"
#include "fx/1d/fire2012.h"
#include <Ticker.h>
#include "PersistentData.h"
#include "MIDI.h"

constexpr uint8_t RGB_LED_PIN = 32;

constexpr uint32_t UPDATE_SCHEDULE_INTERVAL = 51; // 2*255*0.1
constexpr int BLINK_INTERVAL = 5;
constexpr int METRONOME_LED = 7;

constexpr size_t LIGHT_FX_COUNT = 7;
const char* LightFXNames[] = { "None", "Flame", "Blink", "Rainbow", "Demo", "TwinkleFox", "Fire" };

const char* MIDI_FILE = "/Let_It_Snow.mid";

const char* ButtonClass = "button";
const char* ContentTypeHtml = "text/html";

enum FileId
{
    Logo,
    Styles,
    HomeIcon,
    LogFileIcon,
    MusicIcon,
    SettingsIcon,
    _LastFile
};

const char* Files[] =
{
    "Logo.png",
    "styles.css",
    "Home.svg",
    "LogFile.svg",
    "Music.svg",
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
MIDI::File MidiFile;
Ticker LightFXTicker;
std::unique_ptr<fl::Fx1d> GlobalLightFXPtr = nullptr;
CRGB LEDColors[RGB_LED_COUNT];
CHSV LEDColorsHSV[RGB_LED_COUNT];
int scheduleIndexes[MAX_SCHEDULES];

time_t currentTime = 0;
time_t startOfDay = 0;
time_t updateScheduleTime = 0;
int blinkCounter = 0;


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


void startGlobalLightFX(LightFX lightFX, uint8_t fxParam)
{
    Tracer tracer("startGlobalLightFX");

    if (lightFX == LightFX::Demo)
        GlobalLightFXPtr = std::make_unique<fl::DemoReel100>(RGB_LED_COUNT);
    else if (lightFX == LightFX::TwinkleFox)
        GlobalLightFXPtr = std::make_unique<fl::TwinkleFox>(RGB_LED_COUNT);
    else if (lightFX == LightFX::Fire)
        GlobalLightFXPtr = std::make_unique<fl::Fire2012>(RGB_LED_COUNT, 55, fxParam);
    else
        return;

    LightFXTicker.attach_ms(
        10,
        []() {
            if (!GlobalLightFXPtr) return;
            GlobalLightFXPtr->draw(fl::Fx::DrawContext(millis(), LEDColors));
            FastLED.show();
        }
    );
}


void runBackgroundLightFX()
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

            case LightFX::Rainbow:
            {
                LEDColorsHSV[i].hue += scheduleEntry.lightFXparam;
                ledColor = LEDColorsHSV[i];
                colorsChanged = true;
                break;
            }

            case LightFX::Demo:
            case LightFX::TwinkleFox:
            case LightFX::Fire:
                // If any schedule uses those global FX, detach runBackgroundLightFX.
                // It will be re-attached in UpdateSchedule
                startGlobalLightFX(scheduleEntry.lightFX, scheduleEntry.lightFXparam);
                return;
        }
    }

    if (colorsChanged) FastLED.show();
}


void startBackgroundLightFX()
{
    Tracer tracer("startBackgroundLightFX");

    LightFXTicker.attach_ms(100, runBackgroundLightFX);
}


void stopGlobalLightFX()
{
    Tracer tracer("stopGlobalLightFX");

    startBackgroundLightFX();
    delay(50); // Ensure GlobalLightFXPtr is no longer used
    GlobalLightFXPtr = nullptr;
}


void resetSchedules()
{
    Tracer tracer("resetSchedules");

    for (int i = 0; i < MAX_SCHEDULES; i++)
        scheduleIndexes[i] = -1;

    if (GlobalLightFXPtr) stopGlobalLightFX();
}


void updateSchedule()
{
    Tracer tracer("updateSchedule");

    if (currentTime >= startOfDay + SECONDS_PER_DAY)
    {
        startOfDay = getStartOfDay(currentTime);
        resetSchedules();
    }

    bool globalLightFX = false;
    uint16_t currentScheduleTime = (currentTime - startOfDay) / 60;
    for (int i = 0; i < MAX_SCHEDULES; i++)
    {
        int scheduleIndex = scheduleIndexes[i];

        // Advance schedule to the next entry, if applicable.
        for (int n = scheduleIndex + 1; n < MAX_SCHEDULE_ENTRIES; n++)
        {
            ScheduleEntry& nextScheduleEntry = PersistentData.scheduleEntries[i][n];
            if (nextScheduleEntry.time == 0 || currentScheduleTime < nextScheduleEntry.time)
                break;
            scheduleIndex = n;
        }
        scheduleIndexes[i] = scheduleIndex;

        // Check if any global light FX are pending
        if (scheduleIndex >= 0)
        {
            ScheduleEntry& scheduleEntry = PersistentData.scheduleEntries[i][scheduleIndex];
            if (scheduleEntry.time != 0 && scheduleEntry.lightFX >= LightFX::Demo)
                globalLightFX = true;
        }
    }

    if (!globalLightFX)
    {
        if (GlobalLightFXPtr) stopGlobalLightFX();
        updateLEDs();
    }
}


void showNote(uint8_t note, uint8_t velocity)
{
    static uint8_t ledIndexPerKey[] =
    {
        0, // C
        0, // C#
        1, // D
        1, // D#
        2, // E
        3, // F
        3, // F#
        4, // G
        4, // G#
        5, // A
        5, // A#
        6  // B
    };

    static CRGB ledColors[] = 
    {
        CRGB::Gray,     // Octave 3
        CRGB::Gray,     // Octave 3#
        CRGB::Blue,     // Octave 4
        CRGB::Cyan,     // Octave 4#
        CRGB::Green,    // Octave 5 (middle C)
        CRGB::Yellow,   // Octave 5#
        CRGB::Red,      // Octave 6
        CRGB::Magenta,  // Octave 6#
        CRGB::Gray,     // Octave 7
        CRGB::Gray,     // Octave 7#
    };

    uint8_t octave = note / 12;
    if (octave < 3 || octave > 7) return;

    uint8_t key = note % 12;
    uint8_t ledIndex = ledIndexPerKey[key];
    uint8_t colorIndex = (octave - 3) * 2;
    if (key > 0 && ledIndexPerKey[key - 1] == ledIndex) colorIndex++; // sharp
    CRGB ledColor = ledColors[colorIndex];
    LEDColors[ledIndex] = ledColor.scale8(velocity * 2);
    FastLED.show();

    TRACE(
        "showNote(%u, %u) - octave: %u, key: %u, led: %u, color: %u\n",
        note,
        velocity,
        octave,
        key,
        ledIndex,
        colorIndex);
}

void showMetronome(uint8_t beat)
{
    TRACE("showMetronome(%u)\n", beat);

    LEDColors[METRONOME_LED] = (beat == 0) ? CRGB::White : CRGB::Gray25;
    FastLED.show();
    LightFXTicker.once_ms(
        100, 
        []() {
            LEDColors[METRONOME_LED] = CRGB::Black;
            FastLED.show();
        });
}

void onMidiEvent(const MIDI::Event& midiEvent)
{
    MIDI::EventType eventType = midiEvent.getType();
    if (eventType == MIDI::EventType::NoteOn)
        showNote(midiEvent.getNote(), midiEvent.getVelocity());
    else if (eventType == MIDI::EventType::NoteOff)
        showNote(midiEvent.getNote(), 0);
    else if (eventType == MIDI::EventType::Metronome)
        showMetronome(midiEvent.getBeat());
}


void midiPlayTask(void* parameter)
{
    Tracer tracer("midiPlayTask");

    LightFXTicker.detach();
    for (int i = 0; i < RGB_LED_COUNT; i++)
        LEDColors[i] = CRGB::Black;
    FastLED.show();

    MidiFile.play(PersistentData.selectedMidiTrack, onMidiEvent);

    startBackgroundLightFX();

    vTaskDelete(nullptr);
}


void playMidiTrack()
{
    Tracer tracer("playMidiTrack");

    if (PersistentData.selectedMidiTrack < 0 || 
        PersistentData.selectedMidiTrack >= MidiFile.getTracks().size())
    {
        WiFiSM.logEvent("Invalid MIDI track selection: %d", PersistentData.selectedMidiTrack);
        return;
    }
    
    xTaskCreate(
        midiPlayTask,                                   // Task function
        "MidiPlayTask",                                 // Task name
        4096,                                           // Stack size (bytes)
        nullptr,                                        // Parameter passed to task
        1,                                              // Priority
        nullptr                                         // Task handle (not needed)
    );
}


void onTimeServerSynced()
{
    updateSchedule();
    startBackgroundLightFX();

    if (!MidiFile.load(MIDI_FILE))
        WiFiSM.logEvent("Error loading MIDI file: %s", MidiFile.getError().c_str());    
}


void onWiFiInitialized()
{
    if (currentTime >= updateScheduleTime && !MidiFile.getCurrentlyPlaying())
    {
        updateScheduleTime = currentTime + UPDATE_SCHEDULE_INTERVAL;
        updateSchedule();
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
    Html.writeRow("LEDs", "%d FPS", FastLED.getFPS());
    Html.writeRow("Global FX", GlobalLightFXPtr ? GlobalLightFXPtr->fxName().c_str() : "(None)");
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

    resetSchedules();

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

    handleHttpRootRequest();
}

void handleHttpMidiRequest()
{
    Tracer tracer("handleHttpMidiRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader("MIDI music", Nav);
    Html.writeHeading(MIDI_FILE, 1);

    if (MidiFile.getError().isEmpty())
    {
        Html.writeTableStart();
        Html.writeRow("Duration", "%s", formatTimeSpan(MidiFile.getDurationSeconds(), false));
        Html.writeRow("Tracks", "%u", MidiFile.getTracks().size());
        Html.writeRow("Notes", "%u", MidiFile.getTotalNotes());
        Html.writeTableEnd();

        Html.writeHeading("Tracks", 2);
        Html.writeFormStart("/midi");

        int i = 0;
        for (const MIDI::Track& track : MidiFile.getTracks())
        {
            uint32_t notes = track.getTotalNotes();
            if (notes != 0)
            {
                const char* checked = (PersistentData.selectedMidiTrack == i) ? "checked" : "";
                HttpResponse.printf(
                    F("<div><input type=\"radio\" id=\"track%d\" name=\"track\" value=\"%d\" %s><label for=\"track%d\">'%s' - %u notes</label></div>"), 
                    i,
                    i,
                    checked,
                    i,
                    track.getName().c_str(),
                    notes);
            }
            i++;
        }
        
        Html.writeSubmitButton("Save");
        Html.writeFormEnd();

        MIDI::Track* playingTrackPtr = MidiFile.getCurrentlyPlaying();
        if (playingTrackPtr == nullptr)
        {
            if (WiFiSM.shouldPerformAction("play"))
            {
                Html.writeParagraph("Starting to play...");
                playMidiTrack();
            }
            else
                Html.writeActionLink("play", "Play", currentTime, ButtonClass);
        }
        else
        {
            Html.writeParagraph(
                "Currently playing: '%s' - %s",
                playingTrackPtr->getName().c_str(),
                formatTimeSpan(playingTrackPtr->getPlayingForSeconds(), false));
        }
    }
    else
        Html.writeParagraph(MidiFile.getError().c_str());

    Html.writeFooter();
}

void handleHttpMidiConfigPost()
{
    Tracer tracer("handleHttpMidiConfigPost");

    PersistentData.selectedMidiTrack = WebServer.arg("track").toInt();
    PersistentData.writeToEEPROM();

    handleHttpMidiRequest();
}


void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    Serial.println(cmd);
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
            .icon = Files[MusicIcon],
            .label = "MIDI",
            .urlPath ="midi",
            .handler = handleHttpMidiRequest,
            .postHandler = handleHttpMidiConfigPost
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
