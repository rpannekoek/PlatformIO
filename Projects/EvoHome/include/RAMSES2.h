#ifndef RAMSES2_H
#define RAMSES2_H

#include <Print.h>
#include <Logger.h>
#include <LED.h>
#include "CC1101.h"

constexpr size_t RAMSES_MAX_PAYLOAD_SIZE = 64;
constexpr size_t RAMSES_MAX_PACKET_SIZE = RAMSES_MAX_PAYLOAD_SIZE + 16;
constexpr size_t RAMSES_MAX_FRAME_SIZE = (RAMSES_MAX_PACKET_SIZE + 1) * 2 + 12;

constexpr uint16_t PARAM_NULL = 0xFFFF;

enum struct RAMSES2PackageType : uint8_t
{
    Request = 0,
    Info,
    Write,
    Response
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
    void print(Print& output) const;
    void printJson(Print& output) const;

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
    uint16_t opcode = 0;
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

class RAMSES2
{
    public:
        int errors = 0;

        RAMSES2(CC1101& cc1101, HardwareSerial& uart, LED& led, ILogger& logger); 

        void onPacketReceived(void (*handler)(const RAMSES2Packet* packetPtr))
        {
            _packetReceivedHandler = handler;
        }

        bool begin(bool startReceive);
        void byteReceived(uint8_t data);
        bool sendPacket(const RAMSES2Packet& packet);
        size_t createFrame(const RAMSES2Packet& packet, uint8_t* framePtr = nullptr);
        void resetFrame();

    private:
        static const uint8_t _frameHeader[];
        static const uint8_t _frameTrailer[];

        CC1101& _cc1101;
        HardwareSerial& _rxSerial;
        LED& _led;
        ILogger& _logger;
        TaskHandle_t _taskHandle;
        int _frameIndex;
        uint8_t _frameBuffer[RAMSES_MAX_FRAME_SIZE];
        uint8_t _packetBuffer[RAMSES_MAX_PACKET_SIZE];
        void (*_packetReceivedHandler)(const RAMSES2Packet* packetPtr);
        bool _switchToIdle = false;
        uint32_t _switchToReceiveMillis = 0;

        uint8_t manchesterEncode(uint8_t nibble);
        uint8_t manchesterDecode(uint8_t data);
        static void run(void* taskParam);
        void doWork();
        void packetReceived(size_t size);
        bool sendFrame(size_t size);
};

#endif
