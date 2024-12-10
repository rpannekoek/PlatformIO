#include <map>
#include <Tracer.h>
#include <TimeUtils.h>
#include "RAMSES2.h"

const char* RAMSES2Packet::typeId[] = { "Request", "Info",  "Write", "Response" };

const uint8_t RAMSES2::_frameHeader[] = { 0xFF, 0x00, 0x33, 0x55, 0x53 };
const uint8_t RAMSES2::_frameTrailer[] = { 0x35, 0xAA };

const int RAMSES2::_afterSyncWordIndex = 2 - sizeof(_frameHeader);


RAMSES2::RAMSES2(CC1101& cc1101, HardwareSerial& uart, LED& led, ILogger& logger) 
    : _cc1101(cc1101), _serial(uart), _led(led), _logger(logger)
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

    _cc1101.attachSerial(_serial);

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


size_t RAMSES2::uartEncode(size_t size)
{
    static const uint8_t uartBreakCondition[] = { 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF };

    memset(_sendBuffer, 0xFF, sizeof(_sendBuffer));
    memcpy(_sendBuffer, uartBreakCondition, sizeof(uartBreakCondition));

    int i = 0; // input index
    int n = sizeof(uartBreakCondition); // output index
    uint8_t inputBit = 1; // lsb first
    uint8_t outputBit = 0x80; // msb first
    do
    {
        if (inputBit == 1)
        {
            // Add start bit (0)
            _sendBuffer[n] ^= outputBit;
            if (outputBit == 1) { outputBit = 0x80; n++; } else outputBit >>= 1;
        }

        if ((_frameBuffer[i] & inputBit) == 0) _sendBuffer[n] ^= outputBit;
        if (outputBit == 1) { outputBit = 0x80; n++; } else outputBit >>= 1;

        if (inputBit == 0x80)
        {
            inputBit = 1;
            i++;
            // Add stop bit (1)
            if (outputBit == 1) { outputBit = 0x80; n++; } else outputBit >>= 1;
        }
        else
            inputBit <<= 1;
    }
    while (i < size);
    
    return n + 1;
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
    size_t sendBufferSize = uartEncode(frameSize);

    TRACE("UART encoded to %d bytes:\n", sendBufferSize);
    Tracer::hexDump(_sendBuffer, sendBufferSize);

    _led.setOn();
    bool sendOk = sendFrame(sendBufferSize);
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

    int bytesWritten = _cc1101.writeFIFO(_sendBuffer, size);
    TRACE("writeFIFO:%d\n", bytesWritten);
    if (bytesWritten <= 0)
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
        bytesWritten = _cc1101.writeFIFO(_sendBuffer + i, size - i);
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


void RAMSES2::resetFrame(bool success)
{
    if (!success && (_frameIndex >= _afterSyncWordIndex))
    {
        TRACE("_frameIndex=%d\n", _frameIndex);
        if (_frameIndex > 1)
        {
            int packetSize = _frameIndex / 2;
            errors.lastErrorPacketSize = packetSize;
            errors.lastErrorPacketTimestamp = time(nullptr);
            memcpy(errors.lastErrorPacket, _packetBuffer, packetSize);  
        }
    }

    _frameIndex = -sizeof(_frameHeader);
    _headerBitErrors = 0;
    if (_manchesterBitErrors > 0)
    {
        errors.lastManchesterBitErrors = _manchesterBitErrors;
        errors.lastManchesterErrorTimestamp = time(nullptr);
        _manchesterBitErrors = 0;
        _lastManchesterError.packetIndex = -1;
        _lastManchesterError.errorBits = 0;
    }

    if (_led.isOn()) _led.setOff();

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


uint8_t RAMSES2::manchesterDecode(uint8_t data, uint8_t& errorNibble)
{
    // Map 4 bits (nibble) to 2 bits.
    // High nibble contains the bits with errors
    static uint16_t const nibbleDecode[16] = 
    {
        0x30, // 0000 - 2 errors
        0x21, // 0001 - 1 error
        0x20, // 0010 - 1 error
        0x30, // 0011 - 2 errors
        0x12, // 0100 - 1 error
        0x03, // 0101 - ok
        0x02, // 0110 - ok
        0x12, // 0111 - 1 error
        0x10, // 1000 - 1 error
        0x01, // 1001 - ok
        0x00, // 1010 - ok
        0x10, // 1011 - 1 error
        0x30, // 1100 - 2 errors
        0x21, // 1101 - 1 error
        0x20, // 1110 - 1 error
        0x30  // 1111 - 2 errors
    };

    uint8_t lowNibble = nibbleDecode[data & 0x0F];
    uint8_t highNibble = nibbleDecode[data >> 4];
    uint8_t combined =  lowNibble | (highNibble << 2);
    errorNibble = combined >> 4;
    return combined & 0x0F;
}


void RAMSES2::run(void* taskParam)
{
    RAMSES2* instancePtr = static_cast<RAMSES2*>(taskParam); 
    while (true)
    {
        instancePtr->doWork();
        delay(25); // approx. 100 bytes @ 38.4 kbps (UART Rx buffer is 256 bytes)
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
            if (_serial.available())
            {
                size_t bytesRead = _serial.read(_frameBuffer, sizeof(_frameBuffer));
                dataReceived(_frameBuffer, bytesRead);
            }
            break;

        case CC1101Mode::Transmit:
            // Nothing to do
            break;
    }
}


void RAMSES2::dataReceived(const uint8_t* dataPtr, size_t size)
{
    //TRACE("RX:%d\n", size);
    for (int i = 0; i < size; i++)
    {
        uint8_t data = dataPtr[i];

        if (_frameIndex < 0)
        {
            // Scan for RAMSES frame header
            bool proceed = true;
            uint8_t errorBits = data ^ _frameHeader[_frameIndex + sizeof(_frameHeader)];
            if (errorBits)
            {
                if (_frameIndex >= _afterSyncWordIndex)
                {
                    uint8_t bitErrors = countBits(errorBits);
                    _headerBitErrors += bitErrors;
                    TRACE("%02X=>%u/%u\n", data, bitErrors, _headerBitErrors);
                    int headerIndex = _frameIndex - _afterSyncWordIndex;
                    HeaderMismatchInfo& headerMismatchInfo = errors.headerMismatchInfo[headerIndex];
                    headerMismatchInfo.count++;
                    headerMismatchInfo.totalBitErrors += bitErrors;
                    headerMismatchInfo.lastValue = data;
                    headerMismatchInfo.lastErrorBits = errorBits;
                    proceed = _headerBitErrors <= maxHeaderBitErrors;
                }
                else
                    proceed = false;
            }
            if (proceed)
            {
                if (++_frameIndex == 0)
                    _led.setOn();
            }
            else
                resetFrame(false);
        }
        else if (data == _frameTrailer[0])
        {
            bool success = false;
            if (_frameIndex < RAMSES_MIN_FRAME_SIZE)
                errors.frameTooShort++;
            else if (_manchesterBitErrors > maxManchesterBitErrors)
                errors.invalidManchesterCode++;
            else
                success = packetReceived(_frameIndex / 2);
            resetFrame(success);
        }
        else if (_frameIndex / 2 == RAMSES_MAX_PACKET_SIZE)
        {
            errors.frameTooLong++;
            resetFrame(false);
        }
        else 
        {
            // Manchester encoded frame data
            int packetIndex = _frameIndex / 2;
            uint8_t errorNibble;
            uint8_t decodedNibble = manchesterDecode(data, errorNibble);
            if (errorNibble)
            {
                if (_manchesterBitErrors == 0)
                    errors.manchesterErrors.clear();
                _manchesterBitErrors += countBits(errorNibble);
            }

            if (_frameIndex % 2 == 0)
            {
                _packetBuffer[packetIndex] = decodedNibble << 4;
                if (errorNibble) 
                {
                    _lastManchesterError.packetIndex = packetIndex;
                    _lastManchesterError.errorBits = errorNibble << 4;
                    errors.manchesterErrors.push_back(_lastManchesterError);
                }
            }
            else
            {
                _packetBuffer[packetIndex] |= decodedNibble;
                if (errorNibble) 
                {
                    if (_lastManchesterError.packetIndex == packetIndex)
                    {
                        _lastManchesterError.errorBits |= errorNibble;
                        errors.manchesterErrors.pop_back();
                    }
                    else
                    {
                        _lastManchesterError.packetIndex = packetIndex;
                        _lastManchesterError.errorBits = errorNibble;
                    }
                    errors.manchesterErrors.push_back(_lastManchesterError);
                }
            }
            if (errors.manchesterErrors.size() <= MAX_MANCHESTER_ERROR_BYTES)
                _frameIndex++;
            else
            {
                errors.invalidManchesterCode++;
                resetFrame(false);
            }
        }
    }
}


bool RAMSES2::packetReceived(size_t size)
{
    uint8_t checksum = 0;
    for (int i = 0; i < size; i++) checksum += _packetBuffer[i];
    if (checksum != 0)
    {
        if (!_manchesterBitErrors)
        {
            errors.invalidChecksum++;
            return false;
        }

        // There were manchester bit errors
        // Try to correct the checksum error by changing some of those bits.
        uint8_t* correctBytePtr = _packetBuffer + _lastManchesterError.packetIndex;
        uint8_t corrected = *correctBytePtr ^ _lastManchesterError.errorBits;
        uint8_t delta = *correctBytePtr - corrected;
        if (checksum != delta)
        {
            errors.invalidManchesterCode++;
            return false;
        }
        *correctBytePtr = corrected;
        errors.repairedManchesterCode++;
    }
    else if (_manchesterBitErrors)
        errors.ignoredManchesterCode++;

    RAMSES2Packet* packetPtr = new RAMSES2Packet();
    if (!packetPtr->deserialize(_packetBuffer, size - 1))
    {
        errors.deserializationFailed++;
        delete packetPtr;
        return false;
    }

    if (_packetReceivedHandler == nullptr) 
    {
        TRACE("RAMSES2: No packet received handler registered\n");
        delete packetPtr;
        return true;
    }

    packetPtr->rssi = _cc1101.readRSSI();
    packetPtr->timestamp = time(nullptr);

    _packetReceivedHandler(packetPtr);
    return true;
}


size_t RAMSES2Address::serialize(uint8_t* dataPtr) const
{
    if (isNull()) return 0;

    dataPtr[0] = (static_cast<uint8_t>(deviceType) << 2) | deviceId >> 16;
    dataPtr[1] = (deviceId & 0xFF00) >> 8;
    dataPtr[2] = (deviceId & 0xFF); 
    return 3;
}


size_t RAMSES2Address::deserialize(const uint8_t* dataPtr)
{
    deviceType = static_cast<RAMSES2DeviceType>(dataPtr[0] >> 2);
    deviceId = static_cast<uint32_t>(dataPtr[0] & 0x3) << 16
        | static_cast<uint32_t>(dataPtr[1]) << 8
        | static_cast<uint32_t>(dataPtr[2]);

    return 3; // bytes
}


String RAMSES2Address::getDeviceType() const
{
    static std::map<RAMSES2DeviceType, const char*> knownDeviceTypes = 
    {
        { RAMSES2DeviceType::CTL, "CTL" },
        { RAMSES2DeviceType::TRV, "TRV" },
        { RAMSES2DeviceType::OTB, "OTB" },
        { RAMSES2DeviceType::HGI, "HGI" },
        { RAMSES2DeviceType::Null, "NUL" }
    };

    auto loc = knownDeviceTypes.find(deviceType);
    if (loc != knownDeviceTypes.end())
        return loc->second;
    else
        return String(static_cast<uint8_t>(deviceType));
}


bool RAMSES2Address::parse(const String& str)
{
    int devType = 0;
    int devId = 0;
    if (sscanf(str.c_str(), "%d:%d", &devType, &devId) != 2) return false;
    deviceType = static_cast<RAMSES2DeviceType>(devType);
    deviceId = devId;
    return true;
}


void RAMSES2Address::print(Print& output, bool raw) const
{
    if (isNull())
        output.print("--:------");
    else if (raw)
        output.printf("%02d:%06d", static_cast<uint8_t>(deviceType), deviceId);
    else
        output.printf("%s:%06d", getDeviceType().c_str(), deviceId);
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

    dataPtr[i++] = static_cast<uint16_t>(opcode) >> 8;
    dataPtr[i++] = static_cast<uint16_t>(opcode) & 0xFF;

    if (payloadPtr != nullptr) i += payloadPtr->serialize(dataPtr + i, (size - i));

    return i;
}


bool RAMSES2Packet::deserialize(const uint8_t* dataPtr, size_t size)
{
    if (size < RAMSES_MIN_PACKET_SIZE) return false;
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

    opcode = static_cast<RAMSES2Opcode>(dataPtr[0] << 8 | dataPtr[1]);
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
        addr[i].print(output, true);
        output.print(" ");
    }

    output.printf("%04X ", static_cast<uint16_t>(opcode));

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

    output.printf("\"opcode\": \"%04X\", ", static_cast<uint16_t>(opcode));
    
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
        case RAMSES2Opcode::RelayHeatDemand:
        case RAMSES2Opcode::ZoneHeatDemand:
            return new HeatDemandPayload();

        case RAMSES2Opcode::BatteryStatus:
            return new BatteryStatusPayload();

        case RAMSES2Opcode::ZoneSetpoint:
        case RAMSES2Opcode::ZoneTemperature:
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
