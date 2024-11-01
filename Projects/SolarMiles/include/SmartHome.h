#include <tr064.h>
#include <vector>
#include <Log.h>
#include <Logger.h>
#include <HtmlWriter.h>

constexpr int SH_ENERGY_LOG_SIZE = 20;
constexpr uint32_t SH_RETRY_DELAY = 5;

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
        SmartThingsPlug(const String& id, const String& name, ILogger& logger)
            : SmartDevice(id, name, logger)
        {
        }

        bool update(time_t currentTime) override;

    private:
        int _lastErrorCode = 0;
};

enum struct SmartHomeState
{
    Uninitialized = 0,
    Initialized,
    ConnectingFritzbox,
    DiscoveringDevices,
    Ready
};

class SmartHomeClass
{
    public:
        std::vector<SmartDevice*> devices;
        StaticLog<SmartDeviceEnergyLogEntry> energyLog;
        int logEntriesToSync = 0;
        int errors = 0;

        SmartHomeClass(ILogger& logger)
            : energyLog(SH_ENERGY_LOG_SIZE),  _logger(logger)
        {}

        SmartHomeState getState() { return _state; }

        const char* getStateLabel();
        uint32_t getResponseTimeMs();

        bool begin(float powerThreshold, uint32_t powerOffDelay, uint32_t pollInterval);
        bool useFritzbox(const char* host, const char* user, const char* password);
        bool startDiscovery();
        void writeHtml(HtmlWriter& html);
        void writeEnergyLogCsv(Print& output, bool onlyEntriesToSync = true);

    private:
        ILogger& _logger;
        volatile SmartHomeState _state = SmartHomeState::Uninitialized;
        TaskHandle_t _taskHandle = nullptr;
        TR064* _fritzboxPtr = nullptr;
        float _powerThreshold;
        uint32_t _powerOffDelay;
        uint32_t _pollInterval;
        uint32_t _pollMillis;
        int _currentDeviceIndex;

        void setState(SmartHomeState newState);
        bool updateDevice();
        void runStateMachine();
        static void run(void* taskParam);
};
