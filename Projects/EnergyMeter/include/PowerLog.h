#include "Constants.h"
#include <Tracer.h>

struct PowerLogEntry
{
    time_t time;
    float power;

    void reset()
    {
        TRACE("PowerLogEntry::reset()\n");

        time = 0;
        power = 0;
    }

    void aggregate(int aggregations)
    {
        TRACE("PowerLogEntry::aggregate(%d)\n", aggregations);

        power /= aggregations;
    }

    void update(float power)
    {
        TRACE("PowerLogEntry::update(%0.1f)\n", power);

        this->power += power;
    }

    bool equals(PowerLogEntry* other)
    {
        return abs(power - other->power) >= POWER_EQUALS_MARGIN;
    }
};
