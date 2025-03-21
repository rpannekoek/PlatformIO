#include <HomeWizardP1Client.h>
#include <HtmlWriter.h>
#include <Log.h>
#include <Logger.h>
#include <TimeUtils.h>

constexpr uint32_t P1_AGGREGATION_INTERVAL = 60; // seconds
constexpr float GAS_CALORIFIC_VALUE = 9769; // Wh/m3

struct PhaseDayStats
{
    float minVoltage = 666;
    float maxVoltage = 0;
    float minPower = 6666;
    float maxPower = 0;
    float energyIn = 0;
    float energyOut = 0;
    float solarEnergy = 0;
    float grossEnergy = 0;

    void update(float voltage, float power, float solarPower, uint32_t seconds)
    {
        minVoltage = std::min(minVoltage, voltage);
        maxVoltage = std::max(maxVoltage, voltage);
        minPower = std::min(minPower, power);
        maxPower = std::max(maxPower, power);

        float energy = power * seconds / SECONDS_PER_HOUR;
        if (energy >= 0)
            energyIn += energy;
        else
            energyOut += -energy;

        solarEnergy += solarPower * seconds / SECONDS_PER_HOUR;
        grossEnergy += std::max(power + solarPower, 0.0F) * seconds / SECONDS_PER_HOUR;
    }

};

struct P1MonitorDayStatsEntry
{
    time_t day;
    PhaseDayStats phase[3];
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
        float solarPower[3];

        bool isInitialized() { return _p1Client.isInitialized; }
        bool isRequestPending() { return _p1Client.isRequestPending(); }

        // Constructor
        P1MonitorClass(ILogger& logger, uint16_t logSize) 
            : DayStats(7), Log(logSize), _logger(logger)
        {
            memset(solarPower, 0, sizeof(solarPower));
        }

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