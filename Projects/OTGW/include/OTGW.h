#ifndef OTGW_H
#define OTGW_H

#include <stdint.h>
#include <WString.h>
#include <Stream.h>
#include <Log.h>
#include <LED.h>

constexpr int OTGW_WATCHDOG_INTERVAL = 10;
constexpr int OTGW_STARTUP_TIME = 5;
constexpr int OTGW_MESSAGE_LOG_LENGTH = 40;
constexpr uint16_t DATA_VALUE_NONE = 0xFFFF;

enum struct OpenThermMsgType
{
    ReadData = 0,
    WriteData = 1,
    InvalidData = 2,
    ReadAck = 4,
    WriteAck = 5,
    DataInvalid = 6,
    UnknownDataId = 7
};


enum OpenThermDataId // Unscoped enum so it can be used as array index without casting
{
    Status = 0,
    TSet = 1,
    SlaveFault = 5,
    MaxRelModulation = 14,
    TRoomSet = 16,
    RelModulation = 17,
    Pressure = 18,
    DHWFlowRate = 19,
    TRoom = 24,
    TBoiler = 25,
    Tdhw = 26,
    TOutside = 27,
    TReturn = 28,
    MaxTSet = 57,
    BoilerBurnerStarts = 116,
    BoilerBurnerHours = 120,
    BoilerDHWBurnerHours = 123
};


enum OpenThermStatus // Bitflags
{
    SlaveCHMode = 0x2,
    SlaveDHWMode = 0x4,
    SlaveFlame = 0x8,
    MasterCHEnable = 0x100,
    MasterDHWEnable = 0x200,
} ;


enum struct OpenThermGatewayDirection
{
    FromThermostat,
    FromBoiler,
    ToThermostat,
    ToBoiler,
    Error,
    Unexpected
};


struct OpenThermGatewayMessage
{
    String message;
    OpenThermGatewayDirection direction;
    OpenThermMsgType msgType;
    OpenThermDataId dataId;
    uint16_t dataValue;
};


class OpenThermGateway
{
    public:
        uint32_t errors[5];
        uint32_t resets = 0;
        int watchdogResets = 0;
        StringLog MessageLog;

        OpenThermGateway(Stream& serial, uint8_t resetPin);

        void onMessageReceived(void (*handler)(const OpenThermGatewayMessage& otgwMessage))
        {
            _messageReceivedHandler = handler;
        }

        void useLED(LED& led)
        {
            _ledPtr = &led;
        }

        bool begin(uint32_t responseTimeoutMs, uint32_t setpointOverrideTimeout);
        bool run(time_t currentTime);
        void reset();
        uint8_t feedWatchdog();
        bool sendCommand(const String& cmd, const String& value);
        bool setResponse(OpenThermDataId dataId, float value);

        static const char* getMasterStatus(uint16_t dataValue);
        static const char* getSlaveStatus(uint16_t dataValue);
        static const char* getFaultFlags(uint16_t dataValue);

        static float getDecimal(uint16_t dataValue);
        static int8_t getInteger(uint16_t dataValue);

        const char* getResponse() { return _otgwMessage; }
        const char* getLastError() { return _lastError.c_str(); }

    private:
        Stream& _serial;
        LED* _ledPtr = nullptr;
        uint8_t _resetPin;
        uint32_t _responseTimeoutMs;
        uint32_t _setpointOverrideTimeout;
        time_t _currentTime = 0;
        time_t _feedWatchdogTime = 0;
        time_t _setpointOverrideTime = 0;
        String _setpointOverride;
        void (*_messageReceivedHandler)(const OpenThermGatewayMessage& otgwMessage) = nullptr;
        char _otgwMessage[64];
        String _lastError;

        bool initWatchdog(uint8_t timeoutSeconds);
        int readWatchdogData(uint8_t addr);
        bool readLine();
        OpenThermGatewayMessage readMessage();
};


#endif