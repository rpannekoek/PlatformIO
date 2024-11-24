#include <map>
#include <vector>
#include <Tracer.h>
#include <HtmlWriter.h>
#include "Constants.h"
#include "RAMSES2.h"

constexpr size_t STATS_MAX_OPCODE = 16;

struct OpcodeStats
{
    uint16_t packetsReceived[STATS_MAX_OPCODE];

    OpcodeStats()
    {
        memset(packetsReceived, 0, STATS_MAX_OPCODE * sizeof(uint16_t));
    }
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
            
            for (const RAMSES2Address& addr : packetPtr->getAddresses())
            {
                auto loc = opcodeStatsByAddress.find(addr);
                if (loc == opcodeStatsByAddress.end())
                {
                    OpcodeStats* opcodeStatsPtr = new OpcodeStats();
                    opcodeStatsPtr->packetsReceived[opcodeIndex] = 1;
                    opcodeStatsByAddress[addr] = opcodeStatsPtr;
                }
                else
                    loc->second->packetsReceived[opcodeIndex]++;
            }
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
            html.writeRowEnd();
            for (const auto& [addr, opcodeStatsPtr] : opcodeStatsByAddress)
            {
                html.writeRowStart();
                html.writeCell("%s:%06d", addr.getDeviceType().c_str(), addr.deviceId);
                for (int i = 0; i < opcodes.size(); i++)
                    html.writeCell(opcodeStatsPtr->packetsReceived[i]);
                html.writeRowEnd();
            }
            html.writeTableEnd();
        }

    private:
        std::vector<uint16_t> opcodes;
        std::map<RAMSES2Address, OpcodeStats*> opcodeStatsByAddress;

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


