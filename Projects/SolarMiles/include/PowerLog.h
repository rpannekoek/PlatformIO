#include "Constants.h"
#include <Tracer.h>

struct PowerLogEntry
{
    time_t time;
    float dcPower[MAX_REGISTERED_INVERTERS][MAX_DC_CHANNELS_PER_INVERTER];
    float acPower[MAX_REGISTERED_INVERTERS];
    float acVoltage[MAX_REGISTERED_INVERTERS];

    void reset(int numInverters)
    {
        TRACE("PowerLogEntry::reset(%d)\n", numInverters);

        time = 0;
        for (int i = 0; i < numInverters; i++)
        {
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                dcPower[i][ch] = 0;
            acPower[i] = 0;
            acVoltage[i] = 0;
        }
    }

    void aggregate(int aggregations, int numInverters)
    {
        TRACE("PowerLogEntry::aggregate(%d, %d)\n", aggregations, numInverters);

        for (int i = 0; i < numInverters; i++)
        {
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                dcPower[i][ch] /= aggregations;
            acPower[i] /= aggregations;
            acVoltage[i] /= aggregations;
        }
    }

    void updateDC(int inverter, int dcChannel, float dcPower)
    {
        TRACE("PowerLogEntry::updateDC(%d, %d, %0.1f)\n", inverter, dcChannel, dcPower);

        this->dcPower[inverter][dcChannel] += dcPower;
    }

    void updateAC(int inverter, float acPower, float acVoltage)
    {
        TRACE("PowerLogEntry::updateAC(%d, %0.1f, %0.1f)\n", inverter, acPower, acVoltage);

        this->acPower[inverter] += acPower;
        this->acVoltage[inverter] += acVoltage;
    }

    bool equals(PowerLogEntry* other, int numInverters)
    {
        for (int i = 0; i < numInverters; i++)
        {
            for (int ch = 0; ch < MAX_DC_CHANNELS_PER_INVERTER; ch++)
                if (abs(dcPower[i][ch] - other->dcPower[i][ch]) >= POWER_EQUALS_MARGIN)
                    return false;

            if (abs(acPower[i] - other->acPower[i]) >= POWER_EQUALS_MARGIN)
                return false;

            if (abs(acVoltage[i] - other->acVoltage[i]) >= VOLTAGE_EQUALS_MARGIN)
                return false;
        }
        return true;
    }
};
