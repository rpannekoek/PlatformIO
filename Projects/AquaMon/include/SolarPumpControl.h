#include <Log.h>
#include <HtmlWriter.h>

struct SolarLogEntry
{
    time_t time;
    int deltaT;
    float dutyCycle;
    float targetDutyCycle;
    
    bool equals(SolarLogEntry* otherPtr)
    {
        return (otherPtr->deltaT == deltaT)
            && (otherPtr->dutyCycle == dutyCycle)
            && (otherPtr->targetDutyCycle == targetDutyCycle);
    }
};

class SolarPumpControl
{
    public:
        StaticLog<SolarLogEntry> Log;

        SolarPumpControl(uint8_t pwmPin, uint16_t logSize)
            : Log(logSize), _pwmPin(pwmPin)  {}

        bool begin();
        void control(int solarDeltaT, int pumpOnDeltaT, int pumpOffDeltaT, int pwmDeltaT, int pwmChangeRatePct);
        void updateLog(time_t time, int solarDeltaT);
        void writeStateRow(HtmlWriter& html);

    private:
        uint8_t _pwmPin;
        float _dutyCycle = 0;
        float _targetDutyCycle = 0;
        SolarLogEntry* _lastSolarLogEntryPtr = nullptr;
};