#include <PersistentDataBase.h>
#include <FastLED.h>

constexpr size_t MAX_SCHEDULES = 4;
constexpr size_t MAX_SCHEDULE_ENTRIES = 6;
constexpr size_t RGB_LED_COUNT = 8;

enum struct LightFX
{
    None = 0,
    Flame,
    Blink,
    Rainbow,
    Demo,
    TwinkleFox,
    Fire
};

struct ScheduleEntry
{
    uint16_t time;
    CRGB color;
    LightFX lightFX;
    uint8_t lightFXparam;

    int hours() { return time / 60; }
    int minutes() { return time % 60; }
    void setTime(int hours, int minutes)
    {
        time = hours * 60 + minutes;
    }
};

struct Settings : public BasicWiFiSettings
{
    ScheduleEntry scheduleEntries[MAX_SCHEDULES][MAX_SCHEDULE_ENTRIES];
    uint8_t ledSchedules[RGB_LED_COUNT];
    int selectedMidiTrack;

    Settings() 
        : BasicWiFiSettings("XMas32", sizeof(scheduleEntries) + sizeof(ledSchedules) + sizeof(selectedMidiTrack))
    {}

    void initialize() override
    {
        BasicWiFiSettings::initialize();

        memset(scheduleEntries, 0, sizeof(scheduleEntries));
        memset(ledSchedules, 0, sizeof(ledSchedules));
        selectedMidiTrack = -1;
    }
};

Settings PersistentData;
