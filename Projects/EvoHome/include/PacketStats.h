#include <map>
#include <vector>
#include <Tracer.h>
#include <HtmlWriter.h>
#include "Constants.h"
#include "RAMSES2.h"

constexpr size_t STATS_MAX_OPCODE = 16;

struct AddressStats
{
    uint16_t packetsReceived[STATS_MAX_OPCODE];
    uint16_t totalPacketsReceived = 0;
    int16_t minRSSI = 0;
    int16_t maxRSSI = -666;
    int sumRSSI = 0;

    AddressStats()
    {
        memset(packetsReceived, 0, STATS_MAX_OPCODE * sizeof(uint16_t));
    }

    void update(int opcodeIndex, int16_t rssi)
    {
        packetsReceived[opcodeIndex]++;
        totalPacketsReceived++;
        minRSSI = std::min(minRSSI, rssi);
        maxRSSI = std::max(maxRSSI, rssi);
        sumRSSI += rssi;
    }

    int16_t getAverageRSSI() { return sumRSSI / totalPacketsReceived; }
};

class PacketStatsClass
{
    public:
        void processPacket(const RAMSES2Packet* packetPtr)
        {
            int opcodeIndex = getOpcodeIndex(packetPtr->opcode);
            if (opcodeIndex < 0) 
            {
                TRACE("Max opcodes reached: %d\n", STATS_MAX_OPCODE);
                return;
            }

            const RAMSES2Address& addr = packetPtr->addr[0].isNull() 
                ? packetPtr->addr[2]
                : packetPtr->addr[0];
            if (!addr.isNull())
            {
                AddressStats* statsPtr;
                auto loc = statsByAddress.find(addr);
                if (loc == statsByAddress.end())
                {
                    statsPtr = new AddressStats();
                    statsByAddress[addr] = statsPtr;
                }
                else
                    statsPtr = loc->second;
                statsPtr->update(opcodeIndex, packetPtr->rssi);
            }
            else
                TRACE("No address found\n");                
        }

        void writeHtmlTable(HtmlWriter html)
        {

            html.writeTableStart();
            html.writeRowStart();
            html.writeHeaderCell("Address/Opcode");
            for (uint16_t opcode : opcodes)
            {
                String opcodeStr = String(opcode, 16);
                html.writeHeaderCell(opcodeStr);
            }
            html.writeHeaderCell("Total");
            html.writeHeaderCell("RSSI<sub>avg</sub>");
            html.writeRowEnd();
            for (const auto& [addr, statsPtr] : statsByAddress)
            {
                html.writeRowStart();
                html.writeCell("%s:%06d", addr.getDeviceType().c_str(), addr.deviceId);
                for (int i = 0; i < opcodes.size(); i++)
                {
                    if (statsPtr->packetsReceived[i] == 0)
                        html.writeCell("");
                    else
                        html.writeCell(statsPtr->packetsReceived[i]);
                }
                html.writeCell(statsPtr->totalPacketsReceived);
                html.writeCell(statsPtr->getAverageRSSI());
                html.writeRowEnd();
            }
            html.writeTableEnd();
        }

    private:
        std::vector<uint16_t> opcodes;
        std::map<RAMSES2Address, AddressStats*> statsByAddress;

        int getOpcodeIndex(uint16_t opcode)
        {
            for (int i = 0; i < opcodes.size(); i++)
                if (opcode == opcodes[i]) return i;

            if (opcodes.size() == STATS_MAX_OPCODE) return -1;
            int result = opcodes.size();
            opcodes.push_back(opcode);
            return result;
        }
};


