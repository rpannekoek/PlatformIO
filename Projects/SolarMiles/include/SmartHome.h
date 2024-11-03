#include <tr064.h>
#include <vector>
#include <LED.h>
#include <Log.h>
#include <Logger.h>
#include <HtmlWriter.h>
#include "SmartThings.h"

constexpr int SH_ENERGY_LOG_SIZE = 20;
constexpr uint32_t SH_RETRY_DELAY_MS = 5000;

enum struct SmartDeviceState
{
    Unknown = 0,
    Off = 1,
    On = 2
};

class SmartDevice;

struct SmartDeviceEnergyLogEntry
{
    SmartDevice* devicePtr;
    time_t start = 0;
    time_t end = 0;
    float energyStart = 0;
    float energyDelta = 0;
    float maxPower = 0;

    uint32_t getDuration() { return end - start; }

    void reset(time_t time, float energy)
    {
        start = time;
        end = time;
        energyStart = energy;
        energyDelta = 0;
        maxPower = 0;
    }

    void update(time_t time, float energy, float power)
    {
        end = time;
        energyDelta = energy - energyStart;
        maxPower = std::max(maxPower, power);
    }

    void writeCsv(Print& output);
};

class SmartDevice
{
    public:
        String id;
        String name;
        SmartDeviceState state = SmartDeviceState::Unknown;
        SmartDeviceState switchState = SmartDeviceState::Unknown;
        float power = 0;
        float energy = 0;
        float temperature = 0;
        float powerThreshold = 0;
        uint32_t powerOffDelay = 0;
        SmartDeviceEnergyLogEntry energyLogEntry; 

        const char* getStateLabel();
        const char* getSwitchStateLabel();
        virtual bool update(time_t currentTime);

    protected:
        ILogger& _logger;

        SmartDevice(const String& id, const String& name, ILogger& logger)
            : _logger(logger)
        {
            this->id = id;
            this->name = name;
            energyLogEntry.devicePtr = this;
        }
};

class FritzSmartPlug : public SmartDevice
{
    public:
        FritzSmartPlug(const String& id, const String& name, TR064* fritzboxPtr, ILogger& logger)
            : SmartDevice(id, name, logger)
        {
            _fritzboxPtr = fritzboxPtr;
        }

        static FritzSmartPlug* discover(int index, TR064* fritzboxPtr, ILogger& logger);

        bool update(time_t currentTime) override;

    private:
        TR064* _fritzboxPtr;
        int _lastErrorCode = 0;
};

class SmartThingsPlug : public SmartDevice
{
    public:
        SmartThingsPlug(const String& id, const String& name, SmartThingsClient* smartThingsPtr, ILogger& logger)
            : SmartDevice(id, name, logger)
        {
            _smartThingsPtr = smartThingsPtr;
        }

        bool update(time_t currentTime) override;

    private:
        SmartThingsClient* _smartThingsPtr;
        int _lastErrorCode = 0;
};

enum struct SmartHomeState
{
    Uninitialized = 0,
    Initialized,
    ConnectingFritzbox,
    DiscoveringFritzDevices,
    DiscoveringSmartThings,
    Ready
};

class SmartHomeClass
{
    public:
        std::vector<SmartDevice*> devices;
        StaticLog<SmartDeviceEnergyLogEntry> energyLog;
        int logEntriesToSync = 0;
        int errors = 0;

        SmartHomeClass(LED& led, ILogger& logger)
            : energyLog(SH_ENERGY_LOG_SIZE), _led(led), _logger(logger)
        {}

        SmartHomeState getState() { return _state; }

        const char* getStateLabel();

        bool begin(float powerThreshold, uint32_t powerOffDelay, uint32_t pollInterval);
        bool useFritzbox(const char* host, const char* user, const char* password);
        bool useSmartThings(const char* pat);
        bool startDiscovery();
        void run();
        void writeHtml(HtmlWriter& html);
        void writeEnergyLogCsv(Print& output, bool onlyEntriesToSync = true);

    private:
        LED& _led;
        ILogger& _logger;
        volatile SmartHomeState _state = SmartHomeState::Uninitialized;
        TaskHandle_t _taskHandle = nullptr;
        TR064* _fritzboxPtr = nullptr;
        SmartThingsClient* _smartThingsPtr = nullptr;
        float _powerThreshold;
        uint32_t _powerOffDelay;
        uint32_t _pollInterval;
        uint32_t _nextActionMillis;
        int _currentDeviceIndex;

        void setState(SmartHomeState newState);
        bool discoverFritzSmartPlug(int index);
        bool discoverSmartThings();
        bool updateDevice();
};
