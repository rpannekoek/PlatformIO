#include <map>
#include <Log.h>
#include <HtmlWriter.h>
#include <RAMSES2.h>

constexpr size_t EVOHOME_MAX_ZONES = 8;
constexpr size_t EVOHOME_LOG_SIZE = 100;

struct ZoneData
{
    float setpoint = -1;
    float temperature = -1;
    float heatDemand = -1;
    float batteryLevel = -1;

    bool equals(const ZoneData& other) const
    {
        return std::abs(setpoint - other.setpoint) < 0.1
            && std::abs(temperature - other.temperature) < 0.1
            && std::abs(heatDemand - other.heatDemand) < 1.0
            && std::abs(batteryLevel - other.batteryLevel) < 1.0;
    }

    void writeCells(HtmlWriter& html)
    {
        writeCell(html, setpoint, F("%0.1f °C"));
        writeCell(html, temperature, F("%0.1f °C"));
        writeCell(html, heatDemand, F("%0.1f %%"));
        writeCell(html, batteryLevel, F("%0.1f %%"));
    }

    void writeCell(HtmlWriter& html, float value, const __FlashStringHelper* format)
    {
        if (value < 0)
            html.writeCell("");
        else
            html.writeCell(value, format);
    }
};

struct ZoneDataLogEntry
{
    time_t time;
    ZoneData zones[EVOHOME_MAX_ZONES];

    bool equals(const ZoneDataLogEntry& other) const
    {
        for (int i = 0; i < EVOHOME_MAX_ZONES; i++)
            if (!zones[i].equals(other.zones[i])) return false;
        return true;
    }

    void writeRow(HtmlWriter& html, uint8_t zoneCount)
    {
        html.writeRowStart();
        html.writeCell(formatTime("%T", time));
        for (int i = 0; i < zoneCount; i++)
            zones[i].writeCells(html);
        html.writeRowEnd();
    }
};

struct ZoneInfo
{
    uint8_t domainId;
    String name;
    ZoneData current;

    ZoneInfo(uint8_t domaindId, const String& name)
    {
        this->domainId = domaindId;
        this->name = name;
    }

    void writeRow(HtmlWriter& html)
    {
        html.writeRowStart();
        html.writeCell(name);
        current.writeCells(html);
        html.writeRowEnd();
    }
};

class EvoHomeInfo
{
    public:
        uint8_t zoneCount = 0;
        StaticLog<ZoneDataLogEntry> zoneDataLog;

        EvoHomeInfo() : zoneDataLog(EVOHOME_LOG_SIZE)
        {}

        void processPacket(const RAMSES2Packet* packetPtr)
        {
            switch (packetPtr->opcode)
            {
                case 0x1060:
                    processBatteryStatus(static_cast<const BatteryStatusPayload*>(packetPtr->payloadPtr));
                    break;

                case 0x2309:
                case 0x30C9:
                    processTemperatures(
                        packetPtr->opcode,
                        static_cast<const TemperaturePayload*>(packetPtr->payloadPtr));
                    break;

                case 0x0008:
                case 0x3150:
                    processHeatDemand(
                        packetPtr->opcode,
                        static_cast<const HeatDemandPayload*>(packetPtr->payloadPtr));
                    break;

                default:
                    return;
            }

            time_t currentTime = time(nullptr);
            if ((_lastLogEntryPtr == nullptr) 
                || ((currentTime > _lastLogEntryPtr->time + 1) && !_lastLogEntryPtr->equals(_currentLogEntry)))
            {
                _currentLogEntry.time = currentTime;
                _lastLogEntryPtr = zoneDataLog.add(&_currentLogEntry);
            }
        }

        void writeCurrentValues(HtmlWriter& html)
        {
            html.writeTableStart();
            html.writeRowStart();
            html.writeHeaderCell("Zone");
            html.writeHeaderCell("Setpoint");
            html.writeHeaderCell("Temperature");
            html.writeHeaderCell("Heat demand");
            html.writeHeaderCell("Battery");
            html.writeRowEnd();
            for (auto const& [zoneId, zoneInfoPtr] : _zoneInfoById)
                zoneInfoPtr->writeRow(html);
            html.writeTableEnd();
        }

        ZoneInfo* getZoneInfo(uint8_t domainId)
        {
            ZoneInfo* result;
            auto loc = _zoneInfoById.find(domainId);
            if (loc == _zoneInfoById.end())
            {
                result = new ZoneInfo(domainId, RAMSES2Payload::getDomain(domainId));
                _zoneInfoById[domainId] = result;
            }
            else
                result = loc->second;
            return result;
        }

    private:
        std::map<uint8_t, ZoneInfo*> _zoneInfoById;
        ZoneDataLogEntry _currentLogEntry;
        ZoneDataLogEntry* _lastLogEntryPtr = nullptr;

        ZoneData* getZoneData(uint8_t zoneId)
        {
            if (zoneId >= EVOHOME_MAX_ZONES) return nullptr;
            if (zoneId >= zoneCount) zoneCount = zoneId + 1;
            return &_currentLogEntry.zones[zoneId];
        }

        void processTemperatures(uint16_t opcode, const TemperaturePayload* payloadPtr)
        {
            for (int i = 0; i < payloadPtr->getCount(); i++)
            {
                ZoneInfo* zoneInfoPtr = getZoneInfo(payloadPtr->getDomainId(i));
                ZoneData* zoneDataPtr = getZoneData(zoneInfoPtr->domainId);
                float temperature = payloadPtr->getTemperature(i);
                if (opcode == 0x2309)
                {
                    zoneInfoPtr->current.setpoint = temperature;
                    if (zoneDataPtr != nullptr) zoneDataPtr->setpoint = temperature;
                }
                else
                {
                    zoneInfoPtr->current.temperature = temperature;
                    if (zoneDataPtr != nullptr) zoneDataPtr->temperature = temperature;
                }
            }
        }

        void processHeatDemand(uint16_t opcode, const HeatDemandPayload* payloadPtr)
        {
            ZoneInfo* zoneInfoPtr = getZoneInfo(payloadPtr->getDomainId());
            ZoneData* zoneDataPtr = getZoneData(zoneInfoPtr->domainId);
            float heatDemand = payloadPtr->getHeatDemand();
            zoneInfoPtr->current.heatDemand = heatDemand;
            if (zoneDataPtr != nullptr) zoneDataPtr->heatDemand = heatDemand;
        }

        void processBatteryStatus(const BatteryStatusPayload* payloadPtr)
        {
            ZoneInfo* zoneInfoPtr = getZoneInfo(payloadPtr->getDomainId());
            ZoneData* zoneDataPtr = getZoneData(zoneInfoPtr->domainId);
            float batteryLevel = payloadPtr->getBatteryLevel();
            zoneInfoPtr->current.batteryLevel = batteryLevel;
            if (zoneDataPtr != nullptr) zoneDataPtr->batteryLevel = batteryLevel;
        }
};