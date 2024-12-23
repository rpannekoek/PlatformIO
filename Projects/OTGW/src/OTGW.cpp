#include <Tracer.h>
#include <PrintFlags.h>
#include <TimeUtils.h>
#include <Arduino.h>
#include <Wire.h>
#include "OTGW.h"


// For I2C Watchdog protocol/implementation, see:
// https://github.com/rvdbreemen/ESPEasySlaves/blob/master/TinyI2CWatchdog/TinyI2CWatchdog.ino
constexpr int WATCHDOG_I2C_ADDRESS = 0x26;
constexpr uint8_t WATCHDOG_TIMEOUT = 4 * SECONDS_PER_MINUTE;  // almost max possible (255)

static const char* _masterStatusNames[5] = {"CH", "DHW", "Cool", "OTC", "CH2"};
static const char* _slaveStatusNames[7] = {"Fault", "CH", "DHW", "Flame", "Cool", "CH2", "Diag"};
static const char* _faultFlagNames[6] = {"Svc", "Lockout", "PWater", "Flame", "PAir", "TWater"};


OpenThermGateway::OpenThermGateway(Stream& serial, uint8_t resetPin)
    :  MessageLog(OTGW_MESSAGE_LOG_LENGTH, 10), _serial(serial)
{
    _resetPin = resetPin;
}


bool OpenThermGateway::begin(uint32_t responseTimeoutMs, uint32_t setpointOverrideTimeout)
{
    Tracer tracer(F("OpenThermGateway::begin"));

    _responseTimeoutMs = responseTimeoutMs;
    _setpointOverrideTimeout = setpointOverrideTimeout;

    memset(errors, 0, sizeof(errors));

    Wire.begin();
    watchdogResets = initWatchdog(WATCHDOG_TIMEOUT) ? readWatchdogData(14) : -20;

    MessageLog.begin();

    pinMode(_resetPin, OUTPUT);
    reset();
    return true;
}


bool OpenThermGateway::run(time_t currentTime)
{
    _currentTime = currentTime;

    if ((_feedWatchdogTime != 0) && (currentTime >= _feedWatchdogTime))
    {
        _feedWatchdogTime = currentTime + OTGW_WATCHDOG_INTERVAL;
        uint8_t res = feedWatchdog();
        if (res != 0) 
        {
            _lastError = "feedWatchdog: ";
            _lastError += res;
            return false;
        }
    }

    if ((_setpointOverrideTime != 0) && (currentTime >= _setpointOverrideTime))
    {
        _setpointOverrideTime = currentTime + _setpointOverrideTimeout;
        if (!sendCommand("CS", _setpointOverride)) return false;
    }

    if (_serial.available())
    {
        if (_ledPtr != nullptr) _ledPtr->setColor(LED_GREEN);
        OpenThermGatewayMessage otgwMessage = readMessage();
        MessageLog.add(otgwMessage.message.c_str());
        if (_messageReceivedHandler != nullptr) 
            _messageReceivedHandler(otgwMessage);
        else
            TRACE(F("No message received handler registered\n"));
        if (_ledPtr != nullptr) _ledPtr->setOff();
    }

    return true;
}


void OpenThermGateway::reset()
{
    Tracer tracer(F("OpenThermGateway::reset"));

    digitalWrite(_resetPin, LOW);
    delay(100);
    digitalWrite(_resetPin, HIGH);

    _setpointOverrideTime = 0;
    resets++;
}


bool OpenThermGateway::initWatchdog(uint8_t timeoutSeconds)
{
    Tracer tracer(F("OpenThermGateway::initWatchdog"));

    Wire.beginTransmission(WATCHDOG_I2C_ADDRESS);
    Wire.write(6); // SettingsStruct.TimeOut
    Wire.write(timeoutSeconds);
    uint8_t res = Wire.endTransmission();
    if (res != 0) 
    {
        _lastError = F("endTransmission=");
        _lastError += res;
        return false;
    }

    _feedWatchdogTime = OTGW_WATCHDOG_INTERVAL;

    // Read back SettingsStruct.TimeOut to confirm it's set properly.
    int timeoutReg = readWatchdogData(6);
    if (timeoutReg == timeoutSeconds) return true;

    _lastError = F("timeoutReg=");
    _lastError = timeoutReg;
    return false;
}


int OpenThermGateway::readWatchdogData(uint8_t addr)
{
    Tracer tracer(F("OpenThermGateway::readWatchdogData"));

    Wire.beginTransmission(WATCHDOG_I2C_ADDRESS);
    Wire.write(0x83); // Set pointer for byte to read
    Wire.write(addr);
    uint8_t err = Wire.endTransmission();  
    if (err != 0) return -err;
    
    // Request one byte
    if (Wire.requestFrom(WATCHDOG_I2C_ADDRESS, 1) == 0)
        return -10;
    
    return Wire.read(); // Read one byte
}


uint8_t OpenThermGateway::feedWatchdog()
{
    if (_feedWatchdogTime == 0) return 0; // Watchdog not initialized

    Tracer tracer(F("OpenThermGateway::feedWatchdog"));

    Wire.beginTransmission(WATCHDOG_I2C_ADDRESS);
    Wire.write(0xA5); // Reset watchdog timer
    return Wire.endTransmission();
}


bool OpenThermGateway::readLine()
{
    int i = 0;
    uint32_t millis = 0;
    const uint32_t waitMs = 10;
    const uint32_t timeoutMs = 1000;
    do
    {
        int c = _serial.read();
        if (c < 0)
        {
            delay(waitMs);
            millis += waitMs;
            if (millis > timeoutMs)
            {
                _otgwMessage[i] = 0;
                return false;
            }
        }
        else if (c >= ' ')
            _otgwMessage[i++] = c;
        else if (c == '\n')
            break;
    }
    while (i < sizeof(_otgwMessage) - 1);
    
    _otgwMessage[i] = 0;
    return true;
}


OpenThermGatewayMessage OpenThermGateway::readMessage()
{
    Tracer tracer(F("OpenThermGateway::readMessage"));

    OpenThermGatewayMessage result;

    if (!readLine())
    {
        TRACE(F("Read timeout\n"));
        result.message = _otgwMessage;
        result.direction = OpenThermGatewayDirection::Unexpected;
        return result;
    }

    TRACE(F("Message from OTGW: '%s'\n"), _otgwMessage);
    result.message = _otgwMessage;

    // Check for gateway errors
    unsigned int errorCode;
    if (strncmp(_otgwMessage, "Error", 5) == 0)
    {
        if ((sscanf(_otgwMessage + 6, "%x", &errorCode) == 1) && (errorCode > 0) && (errorCode < 5))
            errors[errorCode]++;
        else
            errors[0]++;
        result.direction = OpenThermGatewayDirection::Error;
        return result;
    }

    // Parse OpenTherm message from gateway
    unsigned int otMsgType;
    unsigned int otDataId;
    unsigned int otDataValue;
    int mappedItems = sscanf(_otgwMessage + 1, "%02x%02x%04x", &otMsgType, &otDataId, &otDataValue); 
    if (mappedItems != 3)
    {
        TRACE(F("Failed parsing OpenTherm message. Mapped items: %d\n"), mappedItems);
        result.direction = OpenThermGatewayDirection::Unexpected;
        return result;
    }
    result.msgType = static_cast<OpenThermMsgType>((otMsgType >> 4) & 7);
    result.dataId = static_cast<OpenThermDataId>(otDataId);
    result.dataValue = otDataValue;

    switch (_otgwMessage[0])
    {
        case 'T':
            result.direction = OpenThermGatewayDirection::FromThermostat;
            break;

        case 'B':
            result.direction = OpenThermGatewayDirection::FromBoiler;
            break;

        case 'R':
            result.direction = OpenThermGatewayDirection::ToBoiler;
            break;

        case 'A':
            result.direction = OpenThermGatewayDirection::ToThermostat;
            break;

        case 'E':
            result.direction = OpenThermGatewayDirection::Error;
            break;

        default:
            result.direction = OpenThermGatewayDirection::Unexpected;
    }

    TRACE(F("direction=%d, msgType=%d, dataId=%d, dataValue=0x%04X\n"), result.direction, result.msgType, result.dataId, result.dataValue);

    return result;
}


bool OpenThermGateway::sendCommand(const String& cmd, const String& value)
{
    Tracer tracer(F("OpenThermGateway::sendCommand"), cmd.c_str());

    for (int retries = 0; retries < 2; retries++)
    {
        // Send OTWG command
        _serial.print(cmd);
        _serial.print('=');
        _serial.println(value);

        // Read OTWG response
        // There may be a backlog of non-response messages in the queue, which we will skip here.
        uint32_t timeoutMillis = millis() + _responseTimeoutMs;
        do
        {
            _lastError.clear();
            if (readLine())
            {
                if (strlen(_otgwMessage) == 2)
                {
                    // 2 chars response; assume an error response
                    _lastError = cmd;
                    _lastError += " => ";
                    _lastError += _otgwMessage;
                    break;
                }
                else if (strncmp(_otgwMessage, cmd.c_str(), 2) == 0)
                {
                    TRACE(F("Response: '%s'\n"), _otgwMessage);
                    if (cmd == "CS")
                    {
                        _setpointOverride = value;
                        if (value == "0")
                            _setpointOverrideTime = 0;
                        else
                        {
                            _setpointOverrideTime = _currentTime + _setpointOverrideTimeout;
                            TRACE(F("Send override again at %s\n"), formatTime("%T", _setpointOverrideTime));
                        }
                    }
                    return true;
                }
                else
                    TRACE(F("Non-response: '%s'\n"), _otgwMessage);
            }
            else
                TRACE(F("."));
        }
        while (millis() < timeoutMillis);

        // No proper response message is received within the given timeout.
        // Feed the OTGW watchdog and retry sending the command once more.
        if (_lastError.length() == 0)
            TRACE(F("Response timeout\n"));
        else
            TRACE(F("Error: %s\n"), _lastError.c_str());
        feedWatchdog();  
    }

    // No proper response message is received even after a retry.
    if (_lastError.length() == 0)
    {
        _lastError = cmd;
        _lastError += " timeout";
    }
    return false;
}


bool OpenThermGateway::setResponse(OpenThermDataId dataId, float value)
{
    char valueBuffer[16];

    switch (dataId)
    {
        case OpenThermDataId::MaxTSet:
            snprintf(valueBuffer, sizeof(valueBuffer), "%0.0f", value);
            return sendCommand("SH", valueBuffer); 

        case OpenThermDataId::TOutside:
            snprintf(valueBuffer, sizeof(valueBuffer), "%0.1f", value);
            return sendCommand("OT", valueBuffer); 

        default:
            int16_t dataValue = value * 256.0f;
            snprintf(
                valueBuffer,
                sizeof(valueBuffer),
                "%d:%d,%d",
                dataId,
                dataValue >> 8,
                dataValue & 0xFF);
            return sendCommand("SR", valueBuffer); 
    }
}


const char* OpenThermGateway::getMasterStatus(uint16_t dataValue)
{
    return printFlags((dataValue >> 8), _masterStatusNames, 5, ",");
}


const char* OpenThermGateway::getSlaveStatus(uint16_t dataValue)
{
    return printFlags(dataValue & 0xFF, _slaveStatusNames, 7, ",");
}


const char* OpenThermGateway::getFaultFlags(uint16_t dataValue)
{
    return printFlags((dataValue >> 8), _faultFlagNames, 6, ",");
}


float OpenThermGateway::getDecimal(uint16_t dataValue)
{
    if (dataValue == DATA_VALUE_NONE)
        return 0;
    else
        return float(static_cast<int16_t>(dataValue)) / 256;
}


int8_t OpenThermGateway::getInteger(uint16_t dataValue)
{
    if (dataValue == DATA_VALUE_NONE)
        return 0;
    else
        return static_cast<int8_t>(round(getDecimal(dataValue)));
}
