#include <Ticker.h>

constexpr int MIN_CP_STANDBY_LEVEL = 2000;
constexpr float DEFAULT_SCALE = 0.001F * (82+22) / 22; // 82k/22k voltage divider

enum struct ControlPilotStatus
{
    Standby = 0,
    VehicleDetected = 1,
    Charging = 2,
    ChargingVentilated = 3,
    NoPower = 4
};


class IEC61851ControlPilot
{
    public:
        IEC61851ControlPilot(uint8_t outputPin, uint8_t inputPin, uint8_t feedbackPin, float maxCurrent = 16);

        float getDutyCycle() { return _dutyCycle; }

        bool begin(float scale = DEFAULT_SCALE);
        int calibrate();
        void setOff();
        void setReady();
        float setCurrentLimit(float ampere);
        float getVoltage();
        bool awaitStatus(ControlPilotStatus status, int timeoutMs = 500);
        ControlPilotStatus getStatus();
        const char* getStatusName();
        void setTestStatus(ControlPilotStatus status);

    private:
        uint8_t _outputPin;
        uint8_t _inputPin;
        uint8_t _feedbackPin;
        uint8_t _pwmChannel;
        float _dutyCycle;
        float _scale;
        float _maxCurrent;
        ControlPilotStatus volatile _status;
        uint32_t _nextStatusUpdateMillis = 0;

        void determineStatus(); 
};