#include <LED.h>
#include <Ticker.h>

enum EVSEState
{
    Booting = 0,
    SelfTest = 1,
    Failure = 2,
    Ready = 3,
    Authorize = 4,
    AwaitCharging = 5,
    Charging = 6,
    StopCharging = 7,
    ChargeSuspended = 8,
    ChargeCompleted = 9
};

extern const char* EVSEStateNames[];
extern const char* EVSEStateColors[];

struct LEDColor
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t breathe;
};

class StatusLED : public RGBLED
{
    public:
        StatusLED(uint8_t pin);
        bool setStatus(EVSEState status);

    private:
        static LEDColor _statusColors[];
        static float _breatheTable[];
        LEDColor _statusColor;
        Ticker _breatheTicker;
        int _breatheIndex;

        static void breathe(StatusLED* instancePtr);
        void breathe();

};