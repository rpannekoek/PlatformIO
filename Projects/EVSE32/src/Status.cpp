#include <Tracer.h>
#include "Status.h"

#define BLACK { .red = 0, .green = 0, .blue = 0, .breathe = 0 }
#define BLUE { .red = 0, .green = 0, .blue = 255, .breathe = 0 }
#define GREEN { .red = 0, .green = 255, .blue = 0, .breathe = 0 }
#define GREEN_BREATHE { .red = 0, .green = 255, .blue = 0, .breathe = 1 }
#define CYAN { .red = 0, .green = 255, .blue = 255, .breathe = 0 }
#define CYAN_BREATHE { .red = 0, .green = 255, .blue = 255, .breathe = 1 }
#define RED { .red = 255, .green = 0, .blue = 0, .breathe = 0 }
#define MAGENTA { .red = 255, .green = 0, .blue = 255, .breathe = 0 }
#define YELLOW { .red = 255, .green = 255, .blue = 0, .breathe = 0 }
#define YELLOW_BREATHE { .red = 255, .green = 255, .blue = 0, .breathe = 2 }
#define ORANGE { .red = 255, .green = 165, .blue = 0, .breathe = 0 }
#define ORANGE_BREATHE { .red = 255, .green = 165, .blue = 0, .breathe = 2 }
#define WHITE { .red = 255, .green = 255, .blue = 255, .breathe = 0 }

constexpr float BREATHE_INTERVAL = 0.1F;
constexpr int BREATHE_STEPS = 48;


const char* EVSEStateNames[] = 
{
    [EVSEState::Booting] = "Booting",
    [EVSEState::SelfTest] = "Self Test",
    [EVSEState::Failure] = "Failure",
    [EVSEState::Ready] = "Ready",
    [EVSEState::Authorize] = "Authorize",
    [EVSEState::AwaitCharging] = "Await charging",
    [EVSEState::Charging] = "Charging",
    [EVSEState::StopCharging] = "Stop charging",
    [EVSEState::ChargeSuspended] = "Charge suspended",
    [EVSEState::ChargeCompleted] = "Charge completed"
};

const char* EVSEStateColors[] = 
{
    [EVSEState::Booting] = "blue",
    [EVSEState::SelfTest] = "magenta",
    [EVSEState::Failure] = "red",
    [EVSEState::Ready] = "green",
    [EVSEState::Authorize] = "white",
    [EVSEState::AwaitCharging] = "cyan",
    [EVSEState::Charging] = "orange",
    [EVSEState::StopCharging] = "yellow",
    [EVSEState::ChargeSuspended] = "cyan",
    [EVSEState::ChargeCompleted] = "black"
};

LEDColor StatusLED::_statusColors[] =
{
    [EVSEState::Booting] = BLUE,
    [EVSEState::SelfTest] = MAGENTA,
    [EVSEState::Failure] = RED,
    [EVSEState::Ready] = GREEN_BREATHE,
    [EVSEState::Authorize] = WHITE,
    [EVSEState::AwaitCharging] = CYAN,
    [EVSEState::Charging] = ORANGE_BREATHE,
    [EVSEState::StopCharging] = YELLOW,
    [EVSEState::ChargeSuspended] = CYAN_BREATHE,
    [EVSEState::ChargeCompleted] = BLACK
};

float StatusLED::_breatheTable[] =
{
    1.000,
    0.996,
    0.983,
    0.962,
    0.933,
    0.897,
    0.854,
    0.804,
    0.750,
    0.691,
    0.629,
    0.565,
    0.500,
    0.435,
    0.371,
    0.309,
    0.250,
    0.196,
    0.146,
    0.103,
    0.067,
    0.038,
    0.017,
    0.004,
    0.000
};


StatusLED::StatusLED(uint8_t pin)
    : RGBLED(pin)
{
}


bool StatusLED::setStatus(EVSEState status)
{
    Tracer tracer("StatusLED::setStatus", EVSEStateNames[status]);

    _statusColor = _statusColors[status];
    TRACE(
        "R=%d G=%d B=%d Breathe=%d\n",
        _statusColor.red, _statusColor.green, _statusColor.blue, _statusColor.breathe);

    if (_statusColor.breathe != 0)
    {
        _breatheIndex = 0;
        _breatheTicker.attach(BREATHE_INTERVAL / _statusColor.breathe, breathe, this);
    }
    else
        _breatheTicker.detach();

    return setColor(_statusColor.red, _statusColor.green, _statusColor.blue);
}


void StatusLED::breathe(StatusLED* instancePtr)
{
    instancePtr->breathe();
}


void StatusLED::breathe()
{
    int i = (_breatheIndex <= BREATHE_STEPS / 2) ? _breatheIndex : BREATHE_STEPS - _breatheIndex;
    float f = _breatheTable[i];

    setColor(f * _statusColor.red, f * _statusColor.green, f * _statusColor.blue);
    
    if (++_breatheIndex == BREATHE_STEPS)
        _breatheIndex = 0;
}