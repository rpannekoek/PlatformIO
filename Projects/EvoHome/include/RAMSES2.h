#ifndef RAMSES2_H
#define RAMSES2_H

#include <Print.h>
#include <Logger.h>
#include "CC1101.h"

constexpr size_t RAMSES_MAX_PACKET_SIZE = 256;
constexpr size_t RAMSES_MAX_PAYLOAD_SIZE = 64;

typedef uint8_t field_t; 
constexpr field_t F_PARAM1 = 0x01;
constexpr field_t F_PARAM0 = 0x02;
constexpr field_t F_ADDR0 = 0x04;
constexpr field_t F_ADDR1 = 0x08;
constexpr field_t F_ADDR2 = 0x10;

enum struct RAMSES2PackageType : uint8_t
{
    Request = 0,
    Info,
    Write,
    Response
};

struct RAMSES2Address
{
    uint8_t deviceType = 0;
    uint32_t deviceId = 0;

    String getDeviceType() const;

    size_t deserialize(const uint8_t* dataPtr);
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

    size_t deserialize(const uint8_t* dataPtr);
    virtual const char* getType() const;
    virtual void printJson(Print& output) const;
    void print(Print& output) const;
};

struct HeatDemandPayload : public RAMSES2Payload
{
    String getDomain() const;
    float getHeatDemand() const;

    virtual const char* getType() const override;
    virtual void printJson(Print& output) const override;
};

struct BatteryStatusPayload : public RAMSES2Payload
{
    String getDomain() const;
    float getBatteryLevel() const;
    bool getBatteryLow() const;

    virtual const char* getType() const override;
    virtual void printJson(Print& output) const override;
};

struct RAMSES2Packet
{
    field_t fields;
    uint8_t param[2];
    uint16_t opcode;
    RAMSES2PackageType type;
    RAMSES2Address addr[3];
    RAMSES2Payload* payloadPtr;

    int rssi;
    time_t timestamp;
 
    ~RAMSES2Packet() { if (payloadPtr != nullptr) delete payloadPtr; }

    bool deserialize(const uint8_t* dataPtr, size_t size);
    RAMSES2Payload* createPayload();
    void print(Print& output, const char* timestampFormat = nullptr) const;
    void printJson(Print& output) const;
    std::vector<RAMSES2Address> getAddresses() const;
};

class RAMSES2
{
    public:
        int errors = 0;

        RAMSES2(CC1101& cc1101, ILogger& logger) 
            : _cc1101(cc1101), _logger(logger)
        {}

        void onPacketReceived(void (*handler)(const RAMSES2Packet* packetPtr))
        {
            _packetReceivedHandler = handler;
        }

        bool begin();

    private:
        CC1101& _cc1101;
        ILogger& _logger;
        TaskHandle_t _taskHandle;
        int _frameIndex;
        uint8_t _packetBuffer[RAMSES_MAX_PACKET_SIZE];
        void (*_packetReceivedHandler)(const RAMSES2Packet* packetPtr);

        uint8_t manchesterDecode(uint8_t data);
        void resetFrame();
        static void run(void* taskParam);
        void doWork();
        void byteReceived(uint8_t data);
        void packetReceived(size_t size);
};

#endif
