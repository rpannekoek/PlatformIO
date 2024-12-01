#include <map>
#include <Tracer.h>
#include <TimeUtils.h>
#include "RAMSES2.h"

const char* RAMSES2Packet::typeId[] = { "Request", "Info",  "Write", "Response" };

const uint8_t RAMSES2::_frameHeader[] = { 0xFF, 0x00, 0x33, 0x55, 0x53 };
const uint8_t RAMSES2::_frameTrailer[] = { 0x35, 0xAA };


RAMSES2::RAMSES2(CC1101& cc1101, HardwareSerial& uart, LED& led, ILogger& logger) 
    : _cc1101(cc1101), _rxSerial(uart), _led(led), _logger(logger)
{
}


bool RAMSES2::begin(bool startReceive)
{
    Tracer tracer("RAMSES2::begin");

    if (!_cc1101.begin())
    {
        _logger.logEvent("CC1101 initialization failed");
        return false;
    }

    if (!_cc1101.setTxPower(CC1101TxPower::High))
    {
        _logger.logEvent("Unable to set CC1101 Tx power");
        return false;
    }

    _rxSerial.begin(38400, SERIAL_8N1, _cc1101.getGDO2Pin());

    if (startReceive) _switchToReceiveMillis = millis() + 100;

    BaseType_t res = xTaskCreatePinnedToCore(
        run,
        "RAMSES2",
        8192, // Stack Size (words)
        this,
        3, // Priority
        &_taskHandle,
        PRO_CPU_NUM); // Run on Core #0

    if (res != pdPASS)
    {
        _logger.logEvent("RAMSES2: xTaskCreate returned %d\n", res);
        return false;
    }

    return true;
}


bool RAMSES2::sendPacket(const RAMSES2Packet& packet)
{
    Tracer tracer("RAMSES2::sendPacket");
    packet.print(Serial);

    if (_cc1101.getMode() != CC1101Mode::Idle)
    {
        _switchToIdle = true; // Signal worker thread
        if (!_cc1101.awaitMode(CC1101Mode::Idle, 100))
        {
            _logger.logEvent("Timeout waiting for CC1101 idle");
            return false;
        }
    }

    size_t frameSize = createFrame(packet);

    _led.setOn();
    bool sendOk = sendFrame(frameSize);
    _led.setOff();

    if (!_cc1101.setMode(CC1101Mode::Idle))
    {
        _logger.logEvent("Unable to set CC1101 to idle");
        return false;
    }

    _switchToReceiveMillis = millis() + 100;
    return sendOk;
}


bool RAMSES2::sendFrame(size_t size)
{
    const uint32_t delayMs = 5; // approx. 24 bytes @ 38.4 kpbs
    const uint32_t timeoutMs = 100;

    Tracer tracer("RAMSES2::sendFrame");

    if (!_cc1101.writeRegister(CC1101Register::PKTLEN, size))
    {
        _logger.logEvent("Error setting PKTLEN");
        return false;
    }

    _cc1101.strobe(CC1101Register::SFTX);

    int bytesWritten = _cc1101.writeFIFO(_frameBuffer, size);
    TRACE("writeFIFO:%d\n", bytesWritten);
    if (bytesWritten < 0)
    {
        _logger.logEvent("Error writing to CC1101 FIFO: %d", bytesWritten);
        return false;
    }

    if (!_cc1101.setMode(CC1101Mode::Transmit))
    {
        _logger.logEvent("Unable to set CC1101 in transmit mode");
        return false;
    }

    int i = bytesWritten;
    while (i < size)
    {
        delay(delayMs);
        bytesWritten = _cc1101.writeFIFO(_frameBuffer + i, size - i);
        TRACE("writeFIFO:%d\n", bytesWritten);
        if (bytesWritten < 0)
        {
            _logger.logEvent("Error writing to CC1101 FIFO: %d", bytesWritten);
            return false;
        }
        i += bytesWritten;
    } 

    bool timeout = true;
    for (uint32_t waitedMs = 0; waitedMs < timeoutMs; waitedMs += delayMs )
    {
        uint8_t txBytes = _cc1101.readRegister(CC1101Register::TXBYTES);
        TRACE("TXBYTES:%d\n", txBytes);
        if ((txBytes == 0) || (txBytes & 0x80)) 
        {
            timeout = false;
            break;
        }
        delay(delayMs);
    }
    if (timeout)
        _logger.logEvent("Timeout waiting for transmit");

    return !timeout;
}


size_t RAMSES2::createFrame(const RAMSES2Packet& packet, uint8_t* framePtr)
{
    if (framePtr == nullptr) framePtr = _frameBuffer;

    int packetSize = packet.serialize(_packetBuffer, sizeof(_packetBuffer));
    TRACE("RAMSES2: Packet serialized to %d bytes:\n", packetSize);
    Tracer::hexDump(_packetBuffer, packetSize);

    uint8_t checksum = 0;
    for (int i = 0; i < packetSize; i++) checksum += _packetBuffer[i];
    checksum = -checksum;
    TRACE("RAMSES2: Checksum = 0x%02X\n", checksum);
    _packetBuffer[packetSize++] = checksum;

    // Build frame: preamble, header, manchester encoded packet + checksum, trailer
    int i = 5;
    memset(framePtr, 0xAA, i); // preamble 
    memcpy(framePtr + i, _frameHeader, sizeof(_frameHeader));
    i += sizeof(_frameHeader);
    for (int n = 0; n < packetSize; n++)
    {
        framePtr[i++] = manchesterEncode(_packetBuffer[n] >> 4);
        framePtr[i++] = manchesterEncode(_packetBuffer[n] & 0xF);
    }
    memcpy(framePtr + i, _frameTrailer, sizeof(_frameTrailer));

    size_t frameSize = i + sizeof(_frameTrailer);
    TRACE("RAMSES2: Created frame of %d bytes:\n", frameSize);
    Tracer::hexDump(framePtr, frameSize);

    return frameSize;
}


void RAMSES2::resetFrame()
{
    if (_frameIndex > -4) 
        TRACE("_frameIndex=%d\n", _frameIndex);

    if (_led.isOn()) _led.setOff();

    _frameIndex = -sizeof(_frameHeader);

    if (_switchToIdle)
    {
        _switchToIdle = false;
        if (!_cc1101.setMode(CC1101Mode::Idle))
            _logger.logEvent("Unable to set CC1101 to idle");
        return;
    }
}


uint8_t RAMSES2::manchesterEncode(uint8_t nibble)
{
    // Map 4 bits (nibble) to 8 bits.
    static uint8_t const nibbleEncode[16] =
    {
        0xAA, 0xA9, 0xA6, 0xA5, 0x9A, 0x99, 0x96, 0x95,
        0x6A, 0x69, 0x66, 0x65, 0x5A, 0x59, 0x56, 0x55
    };
    return nibbleEncode[nibble];
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
        delay(10); // approx. 48 bytes @ 38.4 kbps
    }
}


void RAMSES2::doWork()
{
    switch (_cc1101.getMode())
    {
        case CC1101Mode::Idle:
            if ((_switchToReceiveMillis) != 0 && (millis() >= _switchToReceiveMillis))
            {
                _switchToReceiveMillis = 0;
                _frameIndex = -sizeof(_frameHeader);
                if (!_cc1101.setMode(CC1101Mode::Receive))
                    _logger.logEvent("Unable to set CC1101 in receive mode");
            }
            break;

        case CC1101Mode::Receive:
            if (_rxSerial.available())
            {
                size_t bytesRead = _rxSerial.read(_frameBuffer, sizeof(_frameBuffer));
                //TRACE("RX:%d\n", bytesRead);
                for (int i = 0; i < bytesRead; i++) byteReceived(_frameBuffer[i]);
            }
            else
                resetFrame();
            break;

        case CC1101Mode::Transmit:
            // Nothing to do
            break;
    }
}


void RAMSES2::byteReceived(uint8_t data)
{
    if (_frameIndex < 0)
    {
        // Scan for RAMSES frame header
        if (data == _frameHeader[_frameIndex + sizeof(_frameHeader)])
        {
            if (++_frameIndex == 0)
                _led.setOn();
        }
        else
            resetFrame();
    }
    else if (data == _frameTrailer[0])
    {
         packetReceived(_frameIndex / 2);
         resetFrame();
    }
    else if (_frameIndex / 2 == RAMSES_MAX_PACKET_SIZE)
    {
        _logger.logEvent("RAMSES2 Frame too long");
        errors++;
        resetFrame();
    }
    else 
    {
        // Manchester encoded frame data
        uint8_t decodedNibble = manchesterDecode(data);
        if (decodedNibble & 0xF0)
        {
            _logger.logEvent("RAMSES2 Invalid code");
            errors++;
            resetFrame();
        }
        else if (_frameIndex % 2 == 0)
            _packetBuffer[_frameIndex / 2] = decodedNibble << 4;
        else
            _packetBuffer[_frameIndex / 2] |= decodedNibble;
        _frameIndex++;
    }
}


void RAMSES2::packetReceived(size_t size)
{
    uint8_t checksum = 0;
    for (int i = 0; i < size; i++) checksum += _packetBuffer[i];
    if (checksum != 0)
    {
        _logger.logEvent("RAMSES2 Invalid checksum");
        errors++;
        return;
    }

    RAMSES2Packet* packetPtr = new RAMSES2Packet();
    if (!packetPtr->deserialize(_packetBuffer, size - 1))
    {
        _logger.logEvent("RAMSES2 Invalid packet");
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


size_t RAMSES2Address::serialize(uint8_t* dataPtr) const
{
    if (isNull()) return 0;

    dataPtr[0] = (deviceType << 2) | deviceId >> 16;
    dataPtr[1] = (deviceId & 0xFF00) >> 8;
    dataPtr[2] = (deviceId & 0xFF); 
    return 3;
}


size_t RAMSES2Address::deserialize(const uint8_t* dataPtr)
{
    deviceType = dataPtr[0] >> 2;
    deviceId = static_cast<uint32_t>(dataPtr[0] & 0x3) << 16
        | static_cast<uint32_t>(dataPtr[1]) << 8
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
        { 63, "***" },
        { 0xFF, "NUL" }
    };

    auto loc = knownDeviceTypes.find(deviceType);
    if (loc != knownDeviceTypes.end())
        return loc->second;
    else
        return String(deviceType);
}


bool RAMSES2Address::parse(const String& str)
{
    int devType = 0;
    int devId = 0;
    if (sscanf(str.c_str(), "%d:%d", &devType, &devId) != 2) return false;
    deviceType = devType;
    deviceId = devId;
    return true;
}


void RAMSES2Address::print(Print& output) const
{
    if (isNull())
        output.print("--:------");
    else
        output.printf("%02d:%06d", deviceType, deviceId);
}


void RAMSES2Address::printJson(Print& output) const
{
    if (isNull())
        output.print("{}");
    else
        output.printf(
            "{ \"deviceType\": \"%s\", \"deviceId\": %d }",
            getDeviceType().c_str(),
            deviceId);
}


size_t RAMSES2Packet::serialize(uint8_t* dataPtr, size_t size) const
{
    if (size < 8) return 0;

    dataPtr[0] = static_cast<uint8_t>(type) << 4;
    if (addr[0].isNull() && addr[1].isNull()) dataPtr[0] |= 0x4;
    else if (addr[1].isNull()) dataPtr[0] |= 0x8; 
    else if (addr[2].isNull()) dataPtr[0] |= 0xC;

    int i = 1;
    for (int n = 0; n < 3; n++)
        i += addr[n].serialize(dataPtr + i);

    if (param[0] != PARAM_NULL) dataPtr[i++] = param[0];
    if (param[1] != PARAM_NULL) dataPtr[i++] = param[1];

    dataPtr[i++] = opcode >> 8;
    dataPtr[i++] = opcode & 0xFF;

    if (payloadPtr != nullptr) i += payloadPtr->serialize(dataPtr + i, (size - i));

    return i;
}


bool RAMSES2Packet::deserialize(const uint8_t* dataPtr, size_t size)
{
    if (size < 8) return false;
    const uint8_t* endPtr = dataPtr + size;

    uint8_t flags = *dataPtr++;
    type = static_cast<RAMSES2PackageType>((flags & 0x30) >> 4);
    switch (flags & 0xC)
    {
        case 0x0:
            dataPtr += addr[0].deserialize(dataPtr);
            dataPtr += addr[1].deserialize(dataPtr);
            dataPtr += addr[2].deserialize(dataPtr);
            break;

        case 0x4:
            addr[0].setNull();
            addr[1].setNull();
            dataPtr += addr[2].deserialize(dataPtr);
            break;

        case 0x8: 
            dataPtr += addr[0].deserialize(dataPtr);
            addr[1].setNull();
            dataPtr += addr[2].deserialize(dataPtr);
            break;

        case 0xC:
            dataPtr += addr[0].deserialize(dataPtr);
            dataPtr += addr[1].deserialize(dataPtr);
            addr[2].setNull();
            break;
    }

    if (flags & 0x2) param[0] = *dataPtr++;
    if (flags & 0x1) param[1] = *dataPtr++;

    opcode = dataPtr[0] << 8 | dataPtr[1];
    dataPtr += 2;
    if (dataPtr >= endPtr) return false;

    payloadPtr = createPayload();
    size_t payloadSize = payloadPtr->deserialize(dataPtr);

    return ((payloadSize != 0) && (dataPtr + payloadSize == endPtr)); 
}


void RAMSES2Packet::print(Print& output, const char* timestampFormat) const
{
    static const char* typeToken[] = { "RQ", " I", " W", "RP"};

    if (timestampFormat != nullptr)
        output.printf("%s ", formatTime(timestampFormat, timestamp));

    output.printf("%03d ", -rssi);

    output.printf("%s ", typeToken[static_cast<int>(type)]);

    if (param[0] == PARAM_NULL)
        output.print("--- ");
    else
        output.printf("%03d ", param[0]);

    for (int i = 0; i < 3; i++)
    {
        addr[i].print(output);
        output.print(" ");
    }

    output.printf("%04X ", opcode);

    if (payloadPtr != nullptr) payloadPtr->print(output);

    output.println();
}


void RAMSES2Packet::printJson(Print& output) const
{
    output.print("{ ");
    output.printf("\"timestamp\": \"%s\", ", formatTime("%FT%T", timestamp));
    output.printf("\"rssi\": %d, ", rssi);
    output.printf("\"type\": \"%s\", ", typeId[static_cast<int>(type)]);

    if (param[0] != PARAM_NULL) output.printf("\"param0\": %d, ", param[0]);
    if (param[1] != PARAM_NULL) output.printf("\"param1\": %d, ", param[1]);

    for (int i = 0; i < 3; i++)
    {
        if (!addr[i].isNull())
        {
            output.printf("\"addr%d\": ", i);
            addr[i].printJson(output);
            output.print(", ");
        }
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


RAMSES2Payload* RAMSES2Packet::createPayload()
{
    switch (opcode)
    {
        case 0x0008:
        case 0x3150:
            return new HeatDemandPayload();

        case 0x1060:
            return new BatteryStatusPayload();

        case 0x2309:
        case 0x30C9:
            return new TemperaturePayload();

        default:
            return new RAMSES2Payload();
    }
}


const char* RAMSES2Payload::getType() const
{
    return "Unknown";
}


size_t RAMSES2Payload::serialize(uint8_t* dataPtr, size_t size) const
{
    dataPtr[0] = this->size;
    memcpy(dataPtr + 1, bytes, this->size);
    return this->size + 1;
}


size_t RAMSES2Payload::deserialize(const uint8_t* dataPtr)
{
    size = dataPtr[0];
    if (size > RAMSES_MAX_PAYLOAD_SIZE) return 0;
    memcpy(bytes, dataPtr + 1, size);
    return size + 1;
}


bool RAMSES2Payload::parse(const String& str)
{
    int i = 0;
    int c = 0;
    const char* strBufPtr = str.c_str();
    while (c < str.length())
    {
        int byte;
        if (sscanf(strBufPtr + c, "%02X", &byte) != 1) return false;
        bytes[i++] = byte;
        c += 2;
        if (str[c] == ' ') c++;
    }
    size = i;
    return true;
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


String RAMSES2Payload::getDomain(uint8_t domainId)
{
    switch (domainId)
    {
        case 0xF9: return "CH";
        case 0xFA: return "DHW";
        case 0xFC: return "Boiler";
    }

    String result = "Zone #";
    result += (domainId + 1);
    return result;
}


float RAMSES2Payload::getTemperature(const uint8_t* dataPtr)
{
    int16_t data = dataPtr[0];
    data <<= 8;
    data |= dataPtr[1];
    return float(data) / 100;
}


void HeatDemandPayload::printJson(Print& output) const
{
    output.printf(
        "{ \"domain\": \"%s\", \"heatDemand\": %0.1f }",
        getDomain().c_str(),
        getHeatDemand());
}


void BatteryStatusPayload::printJson(Print& output) const
{
    output.printf(
        "{ \"domain\": \"%s\", \"batteryLevel\": %0.1f, \"batteryLow\": %s}",
        getDomain().c_str(),
        getBatteryLevel(),
        getBatteryLow() ? "true" : "false");
}


void TemperaturePayload::printJson(Print& output) const
{
    output.print("[ ");
    for (int i = 0; i < getCount(); i++)
    {
        if (i != 0) output.print(", ");
        output.printf(
            "{ \"domain\": \"%s\", \"temperature\": %0.1f }",
            getDomain(i).c_str(),
            getTemperature(i));
    }
    output.print(" ]");
}
