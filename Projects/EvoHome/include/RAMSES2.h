#ifndef RAMSES2_H
#define RAMSES2_H

#include <vector>
#include <Print.h>
#include <Logger.h>
#include <LED.h>
#include "CC1101.h"

constexpr size_t RAMSES_MAX_PAYLOAD_SIZE = 64;
constexpr size_t RAMSES_MAX_PACKET_SIZE = RAMSES_MAX_PAYLOAD_SIZE + 16; // flags, 3 addr, 2 param, opcode, length
constexpr size_t RAMSES_MAX_FRAME_SIZE = (RAMSES_MAX_PACKET_SIZE + 1) * 2 + 12; // checksum, preamble, header, trailer
constexpr size_t RAMSES_SEND_BUFFER_SIZE = (RAMSES_MAX_FRAME_SIZE * 10 / 8) + 6; // incl. start&stop bits
constexpr size_t RAMSES_MIN_PACKET_SIZE = 7; // flags, 1 addr, opcode, length
constexpr size_t RAMSES_MIN_FRAME_SIZE = (RAMSES_MIN_PACKET_SIZE + 1) * 2;

constexpr uint16_t PARAM_NULL = 0xFFFF;
constexpr uint8_t MAX_MANCHESTER_ERROR_BYTES = 8;

enum struct RAMSES2PackageType : uint8_t
{
    Request = 0,
    Info,
    Write,
    Response
};

enum struct RAMSES2Opcode : uint16_t
{
    Null = 0,
    ZoneName = 0x0004,
    RelayHeatDemand = 0x0008,
    BatteryStatus = 0x1060,
    DeviceInfo = 0x10E0,
    ZoneSetpoint = 0x2309,
    ZoneTemperature = 0x30C9,
    ZoneHeatDemand = 0x3150
};

struct RAMSES2Address
{
    uint8_t deviceType = 0xFF; // null
    uint32_t deviceId = 0;

    bool isNull() const { return deviceType == 0xFF; }
    void setNull() { deviceType = 0xFF; }
    String getDeviceType() const;

    size_t serialize(uint8_t* dataPtr) const;
    size_t deserialize(const uint8_t* dataPtr);
    bool parse(const String& addrString);
    void print(Print& output, bool raw) const;
    void printJson(Print& output) const;

    friend bool operator==(const RAMSES2Address& lhs, const RAMSES2Address& rhs)
    {
        return (lhs.deviceType == rhs.deviceType) && (lhs.deviceId == rhs.deviceId);
    }

    friend bool operator<(const RAMSES2Address& lhs, const RAMSES2Address& rhs)
    {
        return (lhs.deviceType < rhs.deviceType)
            || ((lhs.deviceType == rhs.deviceType) && (lhs.deviceId < rhs.deviceId));
    }
};

struct RAMSES2Payload
{
    uint8_t size;
    uint8_t bytes[RAMSES_MAX_PAYLOAD_SIZE];

    virtual ~RAMSES2Payload() {};

    size_t serialize(uint8_t* dataPtr, size_t size) const;
    size_t deserialize(const uint8_t* dataPtr);
    bool parse(const String& str);
    virtual const char* getType() const;
    virtual void printJson(Print& output) const;
    void print(Print& output) const;

    static String getDomain(uint8_t domainId);
    static float getTemperature(const uint8_t* dataPtr);
};

struct HeatDemandPayload : public RAMSES2Payload
{
    uint8_t getDomainId() const { return bytes[0]; }
    String getDomain() const { return RAMSES2Payload::getDomain(bytes[0]); }
    float getHeatDemand() const { return float(bytes[1]) / 2; }

    virtual const char* getType() const override { return "Heat Demand"; }
    virtual void printJson(Print& output) const override;
};

struct BatteryStatusPayload : public RAMSES2Payload
{
    uint8_t getDomainId() const { return bytes[0]; }
    String getDomain() const { return RAMSES2Payload::getDomain(bytes[0]); };
    float getBatteryLevel() const { return float(bytes[1]) / 2; }
    bool getBatteryLow() const { return bytes[2] == 0; }

    virtual const char* getType() const override { return "Battery Status"; }
    virtual void printJson(Print& output) const override;
};

struct TemperaturePayload: public RAMSES2Payload
{
    uint8_t getCount() const { return size / 3; }
    uint8_t getDomainId(int i) const { return bytes[i*3]; }
    String getDomain(int i) const { return RAMSES2Payload::getDomain(bytes[i*3]); }
    float getTemperature(int i) const { return RAMSES2Payload::getTemperature(bytes + i*3 + 1); };

    virtual const char* getType() const override { return "Temperature"; }
    virtual void printJson(Print& output) const override;
};

struct RAMSES2Packet
{
    static const char* typeId[];

    uint16_t param[2];
    RAMSES2Opcode opcode = RAMSES2Opcode::Null;
    RAMSES2PackageType type = RAMSES2PackageType::Request;
    RAMSES2Address addr[3];
    RAMSES2Payload* payloadPtr = nullptr;

    int16_t rssi = 0;
    time_t timestamp = 0;
 
    RAMSES2Packet() { memset(param, 0xFF, sizeof(param)); }
    ~RAMSES2Packet() { if (payloadPtr != nullptr) delete payloadPtr; }

    size_t serialize(uint8_t* dataPtr, size_t size) const;
    bool deserialize(const uint8_t* dataPtr, size_t size);
    RAMSES2Payload* createPayload();
    void print(Print& output, const char* timestampFormat = nullptr) const;
    void printJson(Print& output) const;
};

struct ManchesterErrorInfo
{
    int packetIndex;
    uint8_t errorBits;
};

struct HeaderMismatchInfo
{
    uint32_t count = 0;
    uint32_t totalBitErrors = 0;
    uint8_t lastValue = 0;
    uint8_t lastErrorBits = 0;

    float getAvgBitErrors() const { return (count == 0) ? 0 : float(totalBitErrors) / count; } 
};

struct RAMSES2ErrorInfo
{
    uint32_t frameTooShort = 0;
    uint32_t frameTooLong = 0;
    uint32_t invalidManchesterCode = 0;
    uint32_t invalidChecksum = 0;
    uint32_t deserializationFailed = 0;

    uint32_t repairedManchesterCode = 0;
    uint32_t ignoredManchesterCode = 0;

    time_t lastManchesterErrorTimestamp = 0;
    uint8_t lastManchesterBitErrors = 0;
    std::vector<ManchesterErrorInfo> manchesterErrors;
    HeaderMismatchInfo headerMismatchInfo[3];

    time_t lastErrorPacketTimestamp = 0;
    size_t lastErrorPacketSize = 0;
    uint8_t lastErrorPacket[RAMSES_MAX_PACKET_SIZE];

    uint32_t getTotal()
    {
        // Excluding header mismatches and repaired
        return frameTooShort + frameTooLong + invalidManchesterCode + invalidChecksum + deserializationFailed;
    }
};

class RAMSES2
{
    public:
        RAMSES2ErrorInfo errors;
        uint8_t maxHeaderBitErrors = 0;
        uint8_t maxManchesterBitErrors = 1;

        RAMSES2(CC1101& cc1101, HardwareSerial& uart, LED& led, ILogger& logger); 

        void onPacketReceived(void (*handler)(const RAMSES2Packet* packetPtr))
        {
            _packetReceivedHandler = handler;
        }

        bool begin(bool startReceive);
        void dataReceived(const uint8_t* data, size_t size);
        bool sendPacket(const RAMSES2Packet& packet);
        size_t createFrame(const RAMSES2Packet& packet, uint8_t* framePtr = nullptr);
        void resetFrame(bool success);
        void switchToIdle() { _switchToIdle = true; }

    private:
        static const uint8_t _frameHeader[];
        static const uint8_t _frameTrailer[];
        static const int _afterSyncWordIndex;

        CC1101& _cc1101;
        HardwareSerial& _serial;
        LED& _led;
        ILogger& _logger;
        TaskHandle_t _taskHandle;
        int _frameIndex;
        uint8_t _frameBuffer[RAMSES_MAX_FRAME_SIZE];
        uint8_t _packetBuffer[RAMSES_MAX_PACKET_SIZE];
        uint8_t _sendBuffer[RAMSES_SEND_BUFFER_SIZE];
        void (*_packetReceivedHandler)(const RAMSES2Packet* packetPtr);
        bool _switchToIdle = false;
        uint32_t _switchToReceiveMillis = 0;
        uint8_t _headerBitErrors = 0;
        uint8_t _manchesterBitErrors = 0;
        ManchesterErrorInfo _lastManchesterError;

        static inline uint8_t countBits(uint8_t data)
        {
            uint8_t result = 0;
            for (uint8_t bit = 0x80; bit >= 1; bit >>= 1)
                if (data & bit) result++;
            return result;
        }

        inline uint8_t manchesterEncode(uint8_t nibble);
        inline uint8_t manchesterDecode(uint8_t data, uint8_t& errorNibble);
        static void run(void* taskParam);
        void doWork();
        bool packetReceived(size_t size);
        bool sendFrame(size_t size);
        size_t uartEncode(size_t);
};

#endif
