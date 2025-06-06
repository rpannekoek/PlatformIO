#include <tr064.h>
#include <vector>
#include <Log.h>
#include <Logger.h>
#include <HtmlWriter.h>
#include "SmartThings.h"

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
    float energyStart = 0; // Wh
    float energyDelta = 0; // Wh
    float maxPower = 0; // W

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
        String info;
        SmartDeviceState state = SmartDeviceState::Unknown;
        SmartDeviceState switchState = SmartDeviceState::Unknown;
        float power = 0; // W
        float energy = 0; // Wh
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

class SmartThingsDevice : public SmartDevice
{
    public:
        SmartThingsDevice(const String& id, const String& name, SmartThingsClient* smartThingsPtr, ILogger& logger)
            : SmartDevice(id, name, logger)
        {
            _smartThingsPtr = smartThingsPtr;
        }

        bool update(time_t currentTime) override;

    private:
        SmartThingsClient* _smartThingsPtr;
        String _powerConsumptionTimestamp;
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

        SmartHomeClass(ILogger& logger, uint16_t energyLogSize)
            : energyLog(energyLogSize), _logger(logger)
        {}

        SmartHomeState getState() { return _state; }
        bool isAwaiting() { return _isAwaiting; }

        const char* getStateLabel();

        std::function<bool(void)> isHostReady;

        bool begin(float powerThreshold, uint32_t powerOffDelay, uint32_t pollInterval);
        bool useFritzbox(const char* host, const char* user, const char* password);
        bool useSmartThings(const char* pat);
        bool startDiscovery();
        void writeHtml(HtmlWriter& html);
        void writeEnergyLogCsv(Print& output, bool onlyEntriesToSync = true);

    private:
        ILogger& _logger;
        TaskHandle_t _taskHandle;
        volatile SmartHomeState _state = SmartHomeState::Uninitialized;
        TR064* _fritzboxPtr = nullptr;
        SmartThingsClient* _smartThingsPtr = nullptr;
        bool _isAwaiting = false;
        float _powerThreshold;
        uint32_t _powerOffDelay;
        uint32_t _pollInterval;
        uint32_t _nextActionMillis;
        int _currentDeviceIndex;

        void setState(SmartHomeState newState);
        bool discoverFritzSmartPlug(int index);
        bool discoverSmartThings();
        bool updateDevice();
        void runStateMachine();
        static void run(void* taskParam);
};
