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
            html.writeHeaderCell("Address", 0, 2);
            html.writeHeaderCell("Total", 0, 2);
            html.writeHeaderCell("Opcode", opcodes.size());
            html.writeHeaderCell("RSSI (dBm)", 3);
            html.writeRowEnd();
            html.writeRowStart();
            for (uint16_t opcode : opcodes)
            {
                char opcodeStr[8];
                snprintf(opcodeStr, sizeof(opcodeStr), "%04X", opcode);
                html.writeHeaderCell(opcodeStr);
            }
            html.writeHeaderCell("Min");
            html.writeHeaderCell("Max");
            html.writeHeaderCell("Avg");
            html.writeRowEnd();

            for (const auto& [addr, statsPtr] : statsByAddress)
            {
                html.writeRowStart();
                html.writeCell("%s:%06d", addr.getDeviceType().c_str(), addr.deviceId);
                html.writeCell(statsPtr->totalPacketsReceived);
                for (int i = 0; i < opcodes.size(); i++)
                {
                    if (statsPtr->packetsReceived[i] == 0)
                        html.writeCell("");
                    else
                        html.writeCell(statsPtr->packetsReceived[i]);
                }
                html.writeCell(statsPtr->minRSSI);
                html.writeCell(statsPtr->maxRSSI);
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


