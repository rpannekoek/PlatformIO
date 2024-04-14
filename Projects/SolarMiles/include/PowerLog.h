#include "Constants.h"
#include <Tracer.h>

struct PowerLogEntry
{
    time_t time;
    float power[MAX_REGISTERED_INVERTERS][MAX_DC_CHANNELS_PER_INVERTER];

    void reset(int numInverters)
    {
        TRACE("PowerLogEntry::reset(%d)\n", numInverters);

        time = 0;
        for (int i = 0; i < numInverters; i++)
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                power[i][ch] = 0;
    }

    void aggregate(int aggregations, int numInverters)
    {
        TRACE("PowerLogEntry::aggregate(%d, %d)\n", aggregations, numInverters);

        for (int i = 0; i < numInverters; i++)
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                power[i][ch] /= aggregations;
    }

    void update(int inverter, int channel, float power)
    {
        TRACE("PowerLogEntry::update(%d, %d, %0.1f)\n", inverter, channel, power);

        this->power[inverter][channel] += power;
    }

    bool equals(PowerLogEntry* other, int numInverters)
    {
        for (int i = 0; i < numInverters; i++)
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                if (abs(power[i][ch] - other->power[i][ch]) >= POWER_EQUALS_MARGIN)
                    return false;
        return true;
    }
};
