#include <tr064.h>
#include <vector>

constexpr int MAX_SMART_DEVICES = 10;

enum struct SmartDeviceState
{
    Disabled = 0,
    Off = 1,
    On = 2
};

class SmartDevice;

class SmartHomeClass
{
    friend class SmartDevice;

    public:
        std::vector<SmartDevice*> devices;

        bool begin(float powerThreshold, uint32_t powerOffDelay, uint32_t pollInterval);
        bool useFritzbox(const char* host, const char* user, const char* password);
        bool discoverDevices();
        bool update(time_t currentTime);

    private:
        TR064 _fritzbox;
        float _powerThreshold;
        uint32_t _powerOffDelay;
        uint32_t _pollInterval;
        time_t _lastPollTime;
        int _pollDeviceIndex;
};

class SmartDevice
{
    public:
        String id;
        String name;
        SmartDeviceState state = SmartDeviceState::Disabled;
        SmartDeviceState switchState = SmartDeviceState::Disabled;
        float power = 0;
        float energy = 0;
        time_t lastOn = 0;
        float powerThreshold = 0;
        uint32_t powerOffDelay = 0;

        const char* getStateLabel();
        const char* getSwitchStateLabel();
        virtual bool update(time_t currentTime);

    protected:
        SmartDevice(const String& id, const String& name)
        {
            this->id = id;
            this->name = name;
        }
};

class FritzSmartPlug : public SmartDevice
{
    public:
        float temperature;

        FritzSmartPlug(const String& id, const String& name, TR064& fritzbox)
            : SmartDevice(id, name), _fritzbox(fritzbox)
        {
        }

        bool update(time_t currentTime) override;

    private:
        TR064& _fritzbox;
};

