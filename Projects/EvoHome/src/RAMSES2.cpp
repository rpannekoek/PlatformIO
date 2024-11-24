#include <map>
#include <Tracer.h>
#include <TimeUtils.h>
#include "RAMSES2.h"

const uint8_t _ramsesFrameHeader[] = { 0xFF, 0x00, 0x33, 0x55, 0x53 };


bool RAMSES2::begin()
{
    Tracer tracer("RAMSES2::begin");

    if (!_cc1101.begin())
    {
        _logger.logEvent("CC1101 initialization failed");
        return false;
    }

    resetFrame();

    BaseType_t res = xTaskCreatePinnedToCore(
        run,
        "RAMSES2",
        8192, // Stack Size (words)
        this,
        18, // Priority
        &_taskHandle,
        PRO_CPU_NUM); // Run on Core #0

    if (res != pdPASS)
    {
        _logger.logEvent("RAMSES2: xTaskCreate returned %d\n", res);
        return false;
    }

    return true;
}


void RAMSES2::resetFrame()
{
    _frameIndex = -sizeof(_ramsesFrameHeader);
}


uint8_t RAMSES2::manchesterDecode(uint8_t data)
{
    // Map 4 bits (nibble) to 2 bits. 0xFF means invalid.
    static uint8_t const nibbleDecode[16] = 
    {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x02, 0xFF,
        0xFF, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    return nibbleDecode[data & 0xF] | (nibbleDecode[(data >> 4) & 0xF] << 2);
}


void RAMSES2::run(void* taskParam)
{
    RAMSES2* instancePtr = static_cast<RAMSES2*>(taskParam); 
    while (true)
    {
        instancePtr->doWork();
        delay(5); // approx. 24 bytes @ 38.4 kbps
    }
}


void RAMSES2::doWork()
{
    static uint8_t buffer[CC1101_FIFO_SIZE];

    switch (_cc1101.getMode())
    {
        case CC1101Mode::Idle:
            if (!_cc1101.setMode(CC1101Mode::Receive))
                _logger.logEvent("Unable to set CC1101 in receive mode");
            break;

        case CC1101Mode::Receive:
        {
            int bytesRead = _cc1101.readFIFO(buffer, sizeof(buffer));
            if (bytesRead >= 0)
                for (int i = 0; i < bytesRead; i++) byteReceived(buffer[i]);
            else
            {
                _logger.logEvent("Reading from CC1101 FIFO failed");
                resetFrame();
            }
            break;
        }

        case CC1101Mode::Transmit:
            // TODO
            break;
    }
}


void RAMSES2::byteReceived(uint8_t data)
{
    if (_frameIndex < 0)
    {
        // Scan for RAMSES frame header
        if (data == _ramsesFrameHeader[_frameIndex + sizeof(_ramsesFrameHeader)])
            _frameIndex++;
        else
            resetFrame();
    }
    else if (data == 0x35)
    {
         // RAMSES frame trailer
         packetReceived(_frameIndex / 2);
         resetFrame();
    }
    else if (_frameIndex / 2 == RAMSES_MAX_PACKET_SIZE)
    {
        _logger.logEvent("RAMSES2: Packet is more than %d bytes\n", RAMSES_MAX_PACKET_SIZE);
        errors++;
        resetFrame();
    }
    else 
    {
        // Manchester encoded frame data
        uint8_t decodedNibble = manchesterDecode(data);
        if (decodedNibble & 0xF0)
        {
            _logger.logEvent("RAMSES2: Invalid manchester code (0x%02X)\n", data);
            errors++;
            resetFrame();
        }
        else if (_frameIndex % 2 == 0)
            _packetBuffer[_frameIndex / 2] = decodedNibble << 4;
        else
            _packetBuffer[_frameIndex / 2] |= decodedNibble;
    }
}


void RAMSES2::packetReceived(size_t size)
{
    TRACE("==== RAMSES2: Received packet of %d bytes ====\n", size);
    Tracer::hexDump(_packetBuffer, size);

    uint8_t checksum = 0;
    for (int i = 0; i < size; i++) checksum += _packetBuffer[0];
    if (checksum != 0)
    {
        _logger.logEvent("RAMSES2: Checksum failure (0x%02X)", checksum);
        errors++;
        return;
    }

    RAMSES2Packet* packetPtr = new RAMSES2Packet();
    if (!packetPtr->deserialize(_packetBuffer, size))
    {
        _logger.logEvent("RAMSES2: Packet deserialization failed");
        errors++;
        return;
    }

    if (_packetReceivedHandler == nullptr) 
    {
        TRACE("RAMSES2: No packet received handler registered\n");
        delete packetPtr;
        return;
    }

    packetPtr->rssi = _cc1101.readRSSI();
    packetPtr->timestamp = time(nullptr);

    _packetReceivedHandler(packetPtr);
}


size_t RAMSES2Address::deserialize(const uint8_t* dataPtr)
{
    deviceType = dataPtr[0] >> 2;
    deviceId = static_cast<uint32_t>(dataPtr[0] & 0x3) << 16
        | static_cast<uint32_t>(dataPtr[1] & 0x3) << 8
        | static_cast<uint32_t>(dataPtr[2]);

    return 3; // bytes
}


String RAMSES2Address::getDeviceType() const
{
    static std::map<uint8_t, const char*> knownDeviceTypes = 
    {
        { 1, "CTL" },
        { 4, "TRV" },
        { 10, "OTB" },
        { 18, "HGI" },
        { 63, "NUL" }
    };

    auto loc = knownDeviceTypes.find(deviceType);
    if (loc != knownDeviceTypes.end())
        return loc->second;
    else
        return String(deviceType);
}


void RAMSES2Address::print(Print& output) const
{
    output.printf("%02d:%06d", deviceType, deviceId);
}


void RAMSES2Address::printJson(Print& output) const
{
    output.printf(
        "{ \"deviceType\": \"%s\", \"deviceId\": %d }",
        getDeviceType().c_str(),
        deviceId);
}


bool RAMSES2Packet::deserialize(const uint8_t* dataPtr, size_t size)
{
    if (size < 8) return false;
    const uint8_t* endPtr = dataPtr + size;

    uint8_t flags = *dataPtr++;
    type = static_cast<RAMSES2PackageType>((flags & 0x30) >> 4);
    fields = flags & 0x3;
    switch (flags & 0xC)
    {
        case 0x0: fields |= F_ADDR0 | F_ADDR1 | F_ADDR2; break;
        case 0x4: fields |= F_ADDR2; break;
        case 0x8: fields |= F_ADDR0 | F_ADDR2; break;
        case 0xC: fields |= F_ADDR0 | F_ADDR1; break;
    }

    if (fields & F_ADDR0) dataPtr += addr[0].deserialize(dataPtr);
    if (fields & F_ADDR1) dataPtr += addr[1].deserialize(dataPtr);
    if (fields & F_ADDR2) dataPtr += addr[2].deserialize(dataPtr);
    if (fields & F_PARAM0) param[0] = *dataPtr++;
    if (fields & F_PARAM1) param[1] = *dataPtr++;

    opcode = dataPtr[0] << 8 | dataPtr[1];
    dataPtr += 2;
    if (dataPtr >= endPtr) return false;

    payloadPtr = createPayload();
    size_t payloadSize = payloadPtr->deserialize(dataPtr);

    return ((payloadSize != 0) && (dataPtr + payloadSize == endPtr)); 
}


void RAMSES2Packet::print(Print& output, const char* timestampFormat) const
{
    static const char* typeId[] = {" I", "RQ", "RP", " W"};

    if (timestampFormat != nullptr)
        output.printf("%s ", formatTime(timestampFormat, timestamp));

    output.printf("%03d ", -rssi);

    output.printf("%s ", typeId[static_cast<int>(type)]);

    if (fields & F_PARAM0)
        output.printf("%03d ", param[0]);
    else
        output.print("--- ");

    field_t addrField = F_ADDR0;
    for (int i = 0; i < 3; i++)
    {
        if (fields & addrField)
            addr[i].print(output);
        else
            output.print("--:------");
        output.print(" ");
        addrField <<= 1;
    }

    output.printf("%04X ", opcode);

    if (payloadPtr != nullptr) payloadPtr->print(output);

    output.println();
}


void RAMSES2Packet::printJson(Print& output) const
{
    static const char* typeId[] = { "Info", "Request", "Response", "Write"};

    output.print("{ ");
    output.printf("\"timestamp\": \"%s\", ", formatTime("%FT%T", timestamp));
    output.printf("\"rssi\": %d, ", rssi);
    output.printf("\"type\": \"%s\", ", typeId[static_cast<int>(type)]);

    if (fields & F_PARAM0) output.printf("\"param0\": %d, ", param[0]);
    if (fields & F_PARAM1) output.printf("\"param1\": %d, ", param[1]);

    field_t addrField = F_ADDR0;
    for (int i = 0; i < 3; i++)
    {
        if (fields & addrField)
        {
            output.printf("\"addr%d\": ", i);
            addr[i].printJson(output);
            output.print(", ");
        }
        addrField <<= 1;
    }

    output.printf("\"opcode\": \"%04X\", ", opcode);
    
    if (payloadPtr != nullptr)
    {
        output.printf("\"payloadType\": \"%s\", ", payloadPtr->getType());
        output.printf("\"payload\": ");
        payloadPtr->printJson(output);
    }

    output.print(" }");
}


std::vector<RAMSES2Address> RAMSES2Packet::getAddresses() const
{
    std::vector<RAMSES2Address> result;
    field_t addrField = F_ADDR0;
    for (int i = 0; i < 3; i++)
    {
        if (fields & addrField)
            result.push_back(addr[i]);
        addrField <<= 1;
    }
    return result;
}


RAMSES2Payload* RAMSES2Packet::createPayload()
{
    switch (opcode)
    {
        case 0x0008:
        case 0x3150:
            return new HeatDemandPayload();

        case 0x1060:
            return new BatteryStatusPayload();

        default:
            return new RAMSES2Payload();
    }
}


const char* RAMSES2Payload::getType() const
{
    return "Unknown";
}


size_t RAMSES2Payload::deserialize(const uint8_t* dataPtr)
{
    size = dataPtr[0];
    TRACE("RAMSES2: %d bytes payload\n", size);

    if (size > RAMSES_MAX_PAYLOAD_SIZE) return 0;

    memcpy(bytes, dataPtr + 1, size);
    return size + 1;
}


void RAMSES2Payload::print(Print& output) const
{
    output.printf("%03d ", size);
    for (int i = 0; i < size; i++) output.printf("%02X", bytes[i]);
}


void RAMSES2Payload::printJson(Print& output) const
{
    output.print("[ ");
    for (int i = 0; i < size; i++)
    {
        if (i > 0) output.print(", ");
        output.printf("%d", bytes[i]);
    } 
    output.print(" ]");
}


const char* HeatDemandPayload::getType() const
{
    return "Heat Demand";
}


String HeatDemandPayload::getDomain() const
{
    uint8_t domainId = bytes[0];
    switch (domainId)
    {
        case 0xF9: return "CH";
        case 0xFA: return "DHW";
        case 0xFC: return "BC";
    }

    String result = "Zone ";
    result += domainId;
    return result;
}


float HeatDemandPayload::getHeatDemand() const
{
    return float(bytes[1]) / 2;
}


void HeatDemandPayload::printJson(Print& output) const
{
    output.printf(
        "{ \"domain\": \"%s\", \"heatDemand\": %0.1f }",
        getDomain().c_str(),
        getHeatDemand());
}


const char* BatteryStatusPayload::getType() const
{
    return "Battery Status";
}


String BatteryStatusPayload::getDomain() const
{
    String result = "Zone ";
    result += bytes[0];
    return result;
}


float BatteryStatusPayload::getBatteryLevel() const
{
    return float(bytes[1]) / 2;
}


bool BatteryStatusPayload::getBatteryLow() const
{
    return bytes[2] == 0;
}


void BatteryStatusPayload::printJson(Print& output) const
{
    output.printf(
        "{ \"domain\": \"%s\", \"batteryLevel\": %0.1f, \"batteryLow\": %s}",
        getDomain().c_str(),
        getBatteryLevel(),
        getBatteryLow() ? "true" : "false");
}
