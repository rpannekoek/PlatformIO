#include <Log.h>
#include <TimeUtils.h>
#include <Tracer.h>

constexpr uint32_t TODAY_LOG_INTERVAL = SECONDS_PER_HOUR / 2;

struct EnergyLogEntry
{
    time_t time;
    uint32_t onDuration = 0; // seconds
    float maxPower = 0; // Watts
    float energy = 0; // Wh

    void update(float power, uint32_t duration)
    {
        if (power > 0) onDuration += duration;
        maxPower = std::max(maxPower, power);
        energy += power * duration / SECONDS_PER_HOUR; 
    }
};

enum EnergyLogType
{
    Today,
    PerDay,
    PerWeek,
    PerMonth
};

class EnergyLog
{
    public:
        EnergyLog(time_t time = 0) :
            todayLog(16 * 2),
            perDayLog(7),
            perWeekLog(12),
            perMonthLog(12)
        {
            if (time != 0) init(time);
        }

        void init(time_t time)
        {
            TRACE(F("EnergyLog::init(%s)\n"), formatTime("%H:%M", time));

            EnergyLogEntry newLogEntry;
            newLogEntry.time = time - (time % TODAY_LOG_INTERVAL);
            todayLogEntryPtr = todayLog.add(&newLogEntry);

            newLogEntry.time = getStartOfDay(time);
            perDayLogEntryPtr = perDayLog.add(&newLogEntry);
            perWeekLogEntryPtr = perWeekLog.add(&newLogEntry);
            perMonthLogEntryPtr = perMonthLog.add(&newLogEntry);
            lastUpdateTime = time;
        }

        StaticLog<EnergyLogEntry>& getLog(EnergyLogType logType)
        {
            switch (logType)
            {
                case EnergyLogType::PerDay: return perDayLog;
                case EnergyLogType::PerWeek: return perWeekLog;
                case EnergyLogType::PerMonth: return perMonthLog;
                default: return todayLog;
            }
        }

        EnergyLogEntry* getYesterdayLogEntry()
        {
            return perDayLog.getEntryFromEnd(2);
        }

        float getMaxEnergy(EnergyLogType logType)
        {
            float result = 0;
            StaticLog<EnergyLogEntry>& log = getLog(logType);
            EnergyLogEntry* energyLogEntryPtr = log.getFirstEntry();
            while (energyLogEntryPtr != nullptr)
            {
                result = std::max(result, energyLogEntryPtr->energy);
                energyLogEntryPtr = log.getNextEntry();
            }
            return result;
        }

        float getTotalEnergy(EnergyLogType logType)
        {
            float result = 0;
            StaticLog<EnergyLogEntry>& log = getLog(logType);
            EnergyLogEntry* energyLogEntryPtr = log.getFirstEntry();
            while (energyLogEntryPtr != nullptr)
            {
                result += energyLogEntryPtr->energy;
                energyLogEntryPtr = log.getNextEntry();
            }
            return result;
        }

        bool update(time_t time, float power)
        {
            bool isNewDay = false;
            uint32_t duration = time - lastUpdateTime;
            lastUpdateTime = time;

            TRACE(
                F("EnergyLog::update(%s, %0.1f) duration: %u s\n"),
                formatTime("%H:%M", time),
                power,
                duration);

            if (time >= (perDayLogEntryPtr->time + SECONDS_PER_DAY))
            {
                startNewDay(time);
                duration = 0;
                isNewDay = true;
            }
            else if (time >= (todayLogEntryPtr->time + TODAY_LOG_INTERVAL))
            {
                EnergyLogEntry newLogEntry;
                newLogEntry.time = time - (time % TODAY_LOG_INTERVAL);
                todayLogEntryPtr = todayLog.add(&newLogEntry);
                TRACE(F("New today log entry @ %s\n"), formatTime("%H:%M", newLogEntry.time));
            }

            todayLogEntryPtr->update(power, duration);
            perDayLogEntryPtr->update(power, duration);
            perWeekLogEntryPtr->update(power, duration);
            perMonthLogEntryPtr->update(power, duration);

            return isNewDay;
        }

    private:
        StaticLog<EnergyLogEntry> todayLog;
        StaticLog<EnergyLogEntry> perDayLog;
        StaticLog<EnergyLogEntry> perWeekLog;
        StaticLog<EnergyLogEntry> perMonthLog;

        EnergyLogEntry* todayLogEntryPtr = nullptr;
        EnergyLogEntry* perDayLogEntryPtr = nullptr;
        EnergyLogEntry* perWeekLogEntryPtr = nullptr;
        EnergyLogEntry* perMonthLogEntryPtr = nullptr;

        time_t lastUpdateTime;

        void startNewDay(time_t time)
        {
            TRACE(F("EnergyLog::startNewDay()\n"));

            EnergyLogEntry newLogEntry;
            newLogEntry.time = time - (time % TODAY_LOG_INTERVAL);
            todayLog.clear();
            todayLogEntryPtr = todayLog.add(&newLogEntry);

            newLogEntry.time = getStartOfDay(time);
            perDayLogEntryPtr = perDayLog.add(&newLogEntry);

            if (time >= (perWeekLogEntryPtr->time + SECONDS_PER_WEEK))
                perWeekLogEntryPtr = perWeekLog.add(&newLogEntry);

            int currentMonth = gmtime(&time)->tm_mon;
            int lastLogMonth = gmtime(&perMonthLogEntryPtr->time)->tm_mon;
            if (currentMonth != lastLogMonth)
                perMonthLogEntryPtr = perMonthLog.add(&newLogEntry);
        }
};

struct InverterLog
{
    EnergyLog* acEnergyLogPtr;
    std::vector<EnergyLog*> dcEnergyLogPtrs;
    time_t lastUpdateTime;
    bool isReachable;

    // Constructor
    InverterLog()
    {
        acEnergyLogPtr = new EnergyLog();
        lastUpdateTime = 0;
        isReachable = false;
    }

    // Destructor
    ~InverterLog()
    {
        delete acEnergyLogPtr;
        for (EnergyLog* dcEnergyLogPtr : dcEnergyLogPtrs)
            delete(dcEnergyLogPtr);
    }
};