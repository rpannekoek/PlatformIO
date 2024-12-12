#include <map>
#include <Log.h>
#include <HtmlWriter.h>
#include <RAMSES2.h>

constexpr size_t EVOHOME_MAX_ZONES = 8;
constexpr size_t EVOHOME_LOG_SIZE = 200;

struct ZoneData
{
    float setpoint = -1;
    float override = -1;
    float temperature = -1;
    float heatDemand = -1;

    bool equals(const ZoneData& other) const
    {
        return std::abs(setpoint - other.setpoint) < 0.1
            && std::abs(override - other.override) < 0.1
            && std::abs(temperature - other.temperature) < 0.1
            && std::abs(heatDemand - other.heatDemand) < 1.0;
    }

    void writeCsv(Print& output)
    {
        writeCsv(output, setpoint);
        writeCsv(output, override);
        writeCsv(output, temperature);
        writeCsv(output, heatDemand);
        writeCsv(output, -1); // Battery level no longer in ZoneData
    }

    void writeCells(HtmlWriter& html)
    {
        writeCell(html, setpoint, F("%0.1f"));
        writeCell(html, override, F("%0.1f"));
        writeCell(html, temperature, F("%0.1f"));
        writeCell(html, heatDemand, F("%0.0f"));
    }

    void writeCell(HtmlWriter& html, float value, const __FlashStringHelper* format)
    {
        if (value < 0)
            html.writeCell("");
        else
            html.writeCell(value, format);
    }

    void writeCsv(Print& output, float value)
    {
        if (value < 0)
            output.print(";");
        else
            output.printf("%0.1f;", value);
    }
};

struct ZoneDataLogEntry
{
    time_t time;
    ZoneData zones[EVOHOME_MAX_ZONES];
    float boilerHeatDemand = -1;

    bool equals(const ZoneDataLogEntry& other) const
    {
        for (int i = 0; i < EVOHOME_MAX_ZONES; i++)
            if (!zones[i].equals(other.zones[i])) return false;
        return boilerHeatDemand == other.boilerHeatDemand;
    }

    void writeRow(HtmlWriter& html, uint8_t zoneCount)
    {
        html.writeRowStart();
        html.writeCell(formatTime("%T", time));
        for (int i = 0; i < zoneCount; i++)
            zones[i].writeCells(html);
        if (boilerHeatDemand < 0)
            html.writeCell("");
        else
            html.writeCell(boilerHeatDemand, F("%0.1f %%"));
        html.writeRowEnd();
    }

    void writeCsv(Print& output, uint8_t zoneCount)
    {
        output.printf("%s;", formatTime("%F %T", time));
        for (int i = 0; i < zoneCount; i++)
            zones[i].writeCsv(output);
        if (boilerHeatDemand >= 0)
            output.printf("%0.1f", boilerHeatDemand);
        output.println();
    }
};

struct DeviceInfo
{
    RAMSES2Address address;
    int domainId = -1;
    float batteryLevel = -1;

    DeviceInfo(const RAMSES2Address& addr)
    {
        address = addr;
    }
};

struct ZoneInfo
{
    uint8_t domainId;
    String name;
    std::vector<DeviceInfo*> devices;
    ZoneData current;

    ZoneInfo(uint8_t domaindId, const String& name)
    {
        this->domainId = domaindId;
        this->name = name;
    }

    void attachDevice(DeviceInfo* deviceInfoPtr)
    {
        for (const DeviceInfo* device : devices)
            if (device->address == deviceInfoPtr->address) return; // Device already attached
        devices.push_back(deviceInfoPtr);
        deviceInfoPtr->domainId = domainId;
    }

    void writeCurrentValues(HtmlWriter& html)
    {
        html.writeRowStart();
        html.writeCell(name);
        current.writeCells(html);
        html.writeRowEnd();
    }

    void writeDeviceInfo(HtmlWriter& html)
    {
        StringBuilder addrStr(16);

        bool first = true;
        for (DeviceInfo* deviceInfoPtr : devices)
        {
            addrStr.clear();
            deviceInfoPtr->address.print(addrStr, false);

            html.writeRowStart();
            if (first)
            {
                first = false;
                html.writeHeaderCell(name, 0, devices.size());
            }
            html.writeCell(addrStr.c_str());
            float batteryLevel = deviceInfoPtr->batteryLevel;
            if (batteryLevel < 0)
                html.writeCell("");
            else
                html.writeCell(batteryLevel, F("%0.0f %%"));
            html.writeRowEnd();
        }
    }
};

class EvoHomeInfo
{
    public:
        uint8_t zoneCount = 0;
        size_t zoneDataLogEntriesToSync = 0;
        StaticLog<ZoneDataLogEntry> zoneDataLog;

        EvoHomeInfo() : zoneDataLog(EVOHOME_LOG_SIZE)
        {}

        void processPacket(const RAMSES2Packet* packetPtr)
        {
            ZoneInfo* zoneInfoPtr = nullptr;
            switch (packetPtr->opcode)
            {
                case RAMSES2Opcode::BatteryStatus:
                    zoneInfoPtr = processBatteryStatus(
                        packetPtr->addr[0],
                        static_cast<const BatteryStatusPayload*>(packetPtr->payloadPtr));
                    break;

                case RAMSES2Opcode::ZoneSetpoint:
                case RAMSES2Opcode::ZoneTemperature:
                    zoneInfoPtr = processTemperatures(
                        packetPtr->opcode,
                        static_cast<const TemperaturePayload*>(packetPtr->payloadPtr));
                    break;

                //case RAMSES2Opcode::RelayHeatDemand: // It seems CTL always sends 0 to zone 0xFC (boiler)
                case RAMSES2Opcode::ZoneHeatDemand:
                    // It seems OTB always sends 0 to zone 0xFC too
                    if (packetPtr->addr[0].deviceType != RAMSES2DeviceType::OTB)
                    {
                        zoneInfoPtr = processHeatDemand(
                            packetPtr->opcode,
                            static_cast<const HeatDemandPayload*>(packetPtr->payloadPtr));
                    }
                    break;

                default:
                    return;
            }

            if (zoneInfoPtr != nullptr) 
            {
                // Zone has been resolved; attach sender device to the zone if not attached yet.
                const RAMSES2Address& senderAddr = packetPtr->addr[0];
                if (senderAddr.deviceType != RAMSES2DeviceType::CTL)
                {
                    DeviceInfo* deviceInfoPtr = getDeviceInfo(senderAddr);
                    if (deviceInfoPtr->domainId < 0)
                        zoneInfoPtr->attachDevice(deviceInfoPtr);
                }
            }

            if ((_lastLogEntryPtr == nullptr) 
                || ((packetPtr->timestamp > _lastLogEntryPtr->time + 1) && !_lastLogEntryPtr->equals(_currentLogEntry)))
            {
                _currentLogEntry.time = packetPtr->timestamp;
                _lastLogEntryPtr = zoneDataLog.add(&_currentLogEntry);
                zoneDataLogEntriesToSync = std::min(zoneDataLogEntriesToSync + 1, EVOHOME_LOG_SIZE);
            }
        }

        void writeCurrentValues(HtmlWriter& html)
        {
            html.writeTableStart();
            html.writeRowStart();
            html.writeHeaderCell("Zone");
            html.writeHeaderCell("T<sub>set</sub> (°C)");
            html.writeHeaderCell("T<sub>ovr</sub> (°C)");
            html.writeHeaderCell("T<sub>act</sub> (°C)");
            html.writeHeaderCell("Heat (%)");
            html.writeRowEnd();
            for (auto const& [zoneId, zoneInfoPtr] : _zoneInfoById)
                zoneInfoPtr->writeCurrentValues(html);
            html.writeTableEnd();
        }

        void writeDeviceInfo(HtmlWriter& html)
        {
            html.writeTableStart();
            html.writeRowStart();
            html.writeHeaderCell("Zone");
            html.writeHeaderCell("Address");
            html.writeHeaderCell("Battery");
            html.writeRowEnd();
            for (auto const& [zoneId, zoneInfoPtr] : _zoneInfoById)
                zoneInfoPtr->writeDeviceInfo(html);
            html.writeTableEnd();
        }

        bool writeZoneDataLogCsv(Print& output)
        {
            if (zoneDataLogEntriesToSync == 0) return false;

            ZoneDataLogEntry* logEntryPtr = zoneDataLog.getEntryFromEnd(zoneDataLogEntriesToSync);
            while (logEntryPtr != nullptr)
            {
                logEntryPtr->writeCsv(output, zoneCount);
                logEntryPtr = zoneDataLog.getNextEntry();
            }

            zoneDataLogEntriesToSync = 0;
            return true;
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
        std::map<RAMSES2Address, DeviceInfo*> _deviceInfoByAddress;
        ZoneDataLogEntry _currentLogEntry;
        ZoneDataLogEntry* _lastLogEntryPtr = nullptr;

        DeviceInfo* getDeviceInfo(const RAMSES2Address& addr)
        {
            DeviceInfo* result;
            auto loc = _deviceInfoByAddress.find(addr);
            if (loc == _deviceInfoByAddress.end())
            {
                result = new DeviceInfo(addr);
                _deviceInfoByAddress[addr] = result;
            }
            else
                result = loc->second;
            return result;
        }

        ZoneData* getZoneData(uint8_t zoneId)
        {
            if (zoneId >= EVOHOME_MAX_ZONES) return nullptr;
            if (zoneId >= zoneCount) zoneCount = zoneId + 1;
            return &_currentLogEntry.zones[zoneId];
        }

        ZoneInfo* processTemperatures(RAMSES2Opcode opcode, const TemperaturePayload* payloadPtr)
        {
            if (payloadPtr->getCount() == 1)
            {
                // Single zone setpoint/temperature reported by TRV
                // TRV seems to always report temperature with zone=0, so we ignore those
                ZoneInfo* zoneInfoPtr = nullptr;
                if (opcode == RAMSES2Opcode::ZoneSetpoint)
                {
                    zoneInfoPtr = getZoneInfo(payloadPtr->getDomainId(0));
                    ZoneData* zoneDataPtr = getZoneData(zoneInfoPtr->domainId);
                    float temperature = payloadPtr->getTemperature(0);
                    zoneInfoPtr->current.override = temperature;
                    if (zoneDataPtr != nullptr) zoneDataPtr->override = temperature;
                }
                return zoneInfoPtr;
            }

            // Multiple zone setpoint/temperatures reported by CTL
            for (int i = 0; i < payloadPtr->getCount(); i++)
            {
                ZoneInfo* zoneInfoPtr = getZoneInfo(payloadPtr->getDomainId(i));
                ZoneData* zoneDataPtr = getZoneData(zoneInfoPtr->domainId);
                float temperature = payloadPtr->getTemperature(i);
                if (opcode == RAMSES2Opcode::ZoneSetpoint)
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
            return nullptr;
        }

        ZoneInfo* processHeatDemand(RAMSES2Opcode opcode, const HeatDemandPayload* payloadPtr)
        {
            ZoneInfo* zoneInfoPtr = getZoneInfo(payloadPtr->getDomainId());
            ZoneData* zoneDataPtr = getZoneData(zoneInfoPtr->domainId);
            float heatDemand = payloadPtr->getHeatDemand();
            zoneInfoPtr->current.heatDemand = heatDemand;
            if (zoneDataPtr != nullptr) zoneDataPtr->heatDemand = heatDemand;
            else if (zoneInfoPtr->domainId == 0xFC) _currentLogEntry.boilerHeatDemand = heatDemand;
            return zoneInfoPtr; 
        }

        ZoneInfo* processBatteryStatus(RAMSES2Address addr, const BatteryStatusPayload* payloadPtr)
        {
            DeviceInfo* deviceInfoPtr = getDeviceInfo(addr);
            deviceInfoPtr->batteryLevel = payloadPtr->getBatteryLevel();

            // TRVs may incorrectly report domainId=0, so don't trust that.
            uint8_t domainId = payloadPtr->getDomainId();
            return (domainId == 0) ? nullptr : getZoneInfo(domainId);
        }
};