#include <map>
#include <HtmlWriter.h>
#include <RAMSES2.h>

struct ZoneInfo
{
    String name;
    float setpoint;
    float temperature;
    float heatDemand;
    float batteryLevel;
};

class EvoHomeInfo
{
    public:
        std::map<uint8_t, ZoneInfo*> zoneInfoByZoneId;

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
            }
        }

        void writeHtmlTable(HtmlWriter& html)
        {
            html.writeTableStart();
            html.writeRowStart();
            html.writeHeaderCell("Zone");
            html.writeHeaderCell("Setpoint");
            html.writeHeaderCell("Temperature");
            html.writeHeaderCell("Heat demand");
            html.writeHeaderCell("Battery");
            html.writeRowEnd();
            for (auto const& [zoneId, zoneInfoPtr] : zoneInfoByZoneId)
            {
                html.writeRowStart();
                html.writeCell(zoneInfoPtr->name);
                html.writeCell(zoneInfoPtr->setpoint, F("%0.1f °C"));
                html.writeCell(zoneInfoPtr->temperature, F("%0.1f °C"));
                html.writeCell(zoneInfoPtr->heatDemand, F("%0.1f %%"));
                html.writeCell(zoneInfoPtr->batteryLevel, F("%0.1f %%"));
                html.writeRowEnd();
            }
            html.writeTableEnd();
        }

    private:
        ZoneInfo* getZoneInfo(uint8_t zoneId)
        {
            ZoneInfo* result;
            auto loc = zoneInfoByZoneId.find(zoneId);
            if (loc == zoneInfoByZoneId.end())
            {
                result = new ZoneInfo();
                result->name = RAMSES2Payload::getDomain(zoneId);
                zoneInfoByZoneId[zoneId] = result;
            }
            else
                result = loc->second;
            return result;
        }

        void processTemperatures(uint16_t opcode, const TemperaturePayload* payloadPtr)
        {
            for (int i = 0; i < payloadPtr->getCount(); i++)
            {
                uint8_t zoneId = payloadPtr->getDomainId(i);
                ZoneInfo* zoneInfoPtr = getZoneInfo(zoneId);
                if (opcode == 0x2309)
                    zoneInfoPtr->setpoint = payloadPtr->getTemperature(i);
                else
                    zoneInfoPtr->temperature = payloadPtr->getTemperature(i);
            }
        }

        void processHeatDemand(uint16_t opcode, const HeatDemandPayload* payloadPtr)
        {
            uint8_t zoneId = payloadPtr->getDomainId();
            ZoneInfo* zoneInfoPtr = getZoneInfo(zoneId);
            zoneInfoPtr->heatDemand = payloadPtr->getHeatDemand();
        }

        void processBatteryStatus(const BatteryStatusPayload* payloadPtr)
        {
            uint8_t zoneId = payloadPtr->getDomainId();
            ZoneInfo* zoneInfoPtr = getZoneInfo(zoneId);
            zoneInfoPtr->batteryLevel = payloadPtr->getBatteryLevel();
        }
};