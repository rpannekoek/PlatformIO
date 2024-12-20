#ifndef OTGW_H
#define OTGW_H

#include <stdint.h>
#include <WString.h>
#include <Stream.h>

constexpr int OTGW_WATCHDOG_INTERVAL = 10;
constexpr int OTGW_STARTUP_TIME = 5;
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
        uint32_t resets;

        OpenThermGateway(Stream& serial, uint8_t resetPin, uint32_t responseTimeoutMs);

        void reset();
        bool initWatchdog(uint8_t timeoutSeconds);
        int readWatchdogData(uint8_t addr);
        uint8_t feedWatchdog();
        OpenThermGatewayMessage readMessage();
        bool sendCommand(String cmd, String value);
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
        uint8_t _resetPin;
        uint32_t _responseTimeoutMs;
        char _otgwMessage[64];
        bool _watchdogInitialized = false;
        String _lastError;


        bool readLine();
};


#endif