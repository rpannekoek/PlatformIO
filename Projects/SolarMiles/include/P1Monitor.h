#include <HomeWizardP1Client.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <Logger.h>
#include <TimeUtils.h>

constexpr uint32_t P1_AGGREGATION_INTERVAL = 60; // seconds
constexpr float GAS_CALORIFIC_VALUE = 9769; // Wh/m3

struct P1MonitorDayStatsEntry
{
    time_t day;
    float minVoltage[3];
    float maxVoltage[3];
    float minPower[3];
    float maxPower[3];
    float energyIn[3];
    float energyOut[3];

    P1MonitorDayStatsEntry()
    {
        for (int i = 0; i < 3; i++)
        {
            minVoltage[i] = 666;
            maxVoltage[i] = 0;
            minPower[i] = 6666;
            maxPower[i] = 0;
            energyIn[i] = 0;
            energyOut[i] = 0;
        }
    }

    void update(int phase, float voltage, float power, uint32_t seconds)
    {
        float energy = power * seconds / SECONDS_PER_HOUR;

        minVoltage[phase] = std::min(minVoltage[phase], voltage);
        maxVoltage[phase] = std::max(maxVoltage[phase], voltage);
        minPower[phase] = std::min(minPower[phase], power);
        maxPower[phase] = std::max(maxPower[phase], power);
        if (energy >= 0)
            energyIn[phase] += energy;
        else
            energyOut[phase] += -energy;
    }

    void writeTableRow(HtmlWriter& html);
};

struct P1MonitorLogEntry
{
    time_t time = 0;
    float voltage[3];
    float power[3];
    float gasPower;

    void reset(time_t aggregationTime)
    {
        time = aggregationTime;
        for (int i = 0; i < 3; i++)
        {
            voltage[i] = 0;
            power[i] = 0;
        }
        gasPower = 0;
    }

    void aggregate(int aggregations)
    {
        for (int i = 0; i < 3; i++)
        {
            voltage[i] /= aggregations;
            power[i] /= aggregations;
        }
    }

    bool equals(P1MonitorLogEntry* otherPtr, float powerDelta, float voltageDelta)
    {
        for (int i = 0; i < 3; i++)
        {
            if (abs(power[i] - otherPtr->power[i]) >= powerDelta) return false;
            if (abs(voltage[i] - otherPtr->voltage[i]) >= voltageDelta) return false;
        }
        return (abs(gasPower - otherPtr->gasPower) < powerDelta);
    }

    void writeTableRow(HtmlWriter& html);
    void writeCsv(Print& output);
};

class P1MonitorClass
{
    public:
        StaticLog<P1MonitorDayStatsEntry> DayStats;
        StaticLog<P1MonitorLogEntry> Log;
        int logEntriesToSync = 0;

        bool isInitialized() { return _p1Client.isInitialized; }
        bool isRequestPending() { return _p1Client.isRequestPending(); }

        // Constructor
        P1MonitorClass(ILogger& logger, uint16_t logSize) 
            : DayStats(7), Log(logSize), _logger(logger) {}

        bool begin(const char* host, uint32_t pollInterval, float powerDelta, float voltageDelta);
        bool run(time_t time);
        void writeStatus(HtmlWriter& html);
        void writeCurrentValues(HtmlWriter& html, int maxPhasePower);
        void writeDayStats(HtmlWriter& html);
        void writeLog(HtmlWriter& html, int page, int pageSize);
        void writeLogCsv(Print& output, int entries);

    private:
        P1MonitorDayStatsEntry* _currentDayStatsPtr = nullptr;
        P1MonitorLogEntry _newLogEntry;
        P1MonitorLogEntry* _lastLogEntryPtr;
        HomeWizardP1V1Client _p1Client;
        ILogger& _logger;
        uint32_t _pollInterval;
        time_t _nextPollTime = 0;
        int _aggregations = 0;
        float _powerDelta;
        float _voltageDelta;
        float _gasPower = 0;
        float _lastGasM3 = 0;
        uint64_t _lastGasTimestamp = 0;

        void updateLog(time_t time);
        void writeDayStats(HtmlWriter& html, int phase, int property);
};