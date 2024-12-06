#ifndef CC1101_H
#define CC1101_h

#include <stdint.h>
#include <SPI.h>
#include <HardwareSerial.h>

constexpr uint8_t CC1101_FIFO_SIZE = 64;

enum CC1101Error
{
    CC1101_ERR_GENERAL = -1,
    CC1101_ERR_INVALID_STATE = -2,
    CC1101_ERR_RX_FIFO_OVERFLOW = -3,
    CC1101_ERR_TX_FIFO_UNDERFLOW = -4
};

typedef int8_t state_t;

enum struct CC1101Register
{
    IOCFG2 = 0x00,
    IOCFG1 = 0x01,
    IOCFG0 = 0x02,
    FIFOTHR = 0x03,
    SYNC1 = 0x04,
    SYNC0 = 0x05,
    PKTLEN = 0x06,
    PKTCTRL1 = 0x07,
    PKTCTRL0 = 0x08,
    ADDR = 0x09,
    CHANNR = 0x0A,
    FSCTRL1 = 0x0B,
    FSCTRL0 = 0x0C,
    FREQ2 = 0x0D,
    FREQ1 = 0x0E,
    FREQ0 = 0x0F,
    MDMCFG4 = 0x10,
    MDMCFG3 = 0x11,
    MDMCFG2 = 0x12,
    MDMCFG1 = 0x13,
    MDMCFG0 = 0x14,
    DEVIATN = 0x15,
    MCSM2 = 0x16,
    MCSM1 = 0x17,
    MCSM0 = 0x18,
    FOCCFG = 0x19,
    BSCFG = 0x1A,
    AGCCTRL2 = 0x1B,
    AGCCTRL1 = 0x1C,
    AGCCTRL0 = 0x1D,
    WOREVT1 = 0x1E,
    WOREVT0 = 0x1F,
    WORCTRL = 0x20,
    FREND1 = 0x21,
    FREND0 = 0x22,
    FSCAL3 = 0x23,
    FSCAL2 = 0x24,
    FSCAL1 = 0x25,
    FSCAL0 = 0x26,
    RCCTRL1 = 0x27,
    RCCTRL0 = 0x28,
    FSTEST = 0x29,
    PTEST = 0x2A,
    AGCTEST = 0x2B,
    TEST2 = 0x2C,
    TEST1 = 0x2D,
    TEST0 = 0x2E,
    SRES = 0x30,
    SFSTXON = 0x31,
    SXOFF = 0x32,
    SCAL = 0x33,
    SRX = 0x34,
    STX = 0x35,
    SIDLE = 0x36,
    SWORTIME = 0x37,
    SWOR = 0x38,
    SPWD = 0x39,
    SFRX = 0x3A,
    SFTX = 0x3B,
    SWORRST = 0x3C,
    SNOP = 0x3D,
    PATABLE = 0x3E,
    FIFO = 0x3F,
    PARTNUM = SRES | 0x40,
    VERSION = SFSTXON | 0x40,
    FREQEST = SXOFF | 0x40,
    LQI = SCAL | 0x40,
    RSSI = SRX | 0x40,
    MARCSTATE = STX | 0x40,
    WORTIME1 = SIDLE | 0x40,
    WORTIME0 = SWORTIME | 0x40,
    PKTSTATUS = SWOR | 0x40,
    VCO_VC_DAC = SPWD | 0x40,
    TXBYTES = SFRX | 0x40,
    RXBYTES = SFTX | 0x40,
    RCCTRL1_STATUS = SWORRST | 0x40,
    RCCTRL0_STATUS = SNOP | 0x40,
};

enum struct CC1101State
{
    IDLE = 0x00,
    RX = 0x10,
    TX = 0x20,
    FSTXON = 0x30,
    CALIBRATE = 0x40,
    SETTLING = 0x50,
    RX_OVERFLOW = 0x60,
    TX_UNDERFLOW = 0x70
};

enum CC1101Mode
{
    Idle,
    Receive,
    Transmit
};

enum CC1101TxPower
{
    Low, // -10 dBm
    Medium, // 0 dBm
    High, // 10 dbm
};

class CC1101
{
    public:
        CC1101(uint8_t spiBus, int8_t sckPin, int8_t misoPin, int8_t mosiPin, int8_t csnPin, int8_t gdo2Pin, int8_t gdo0Pin);

        CC1101Mode inline getMode() { return _mode; }
        CC1101State inline getState(state_t state) { return static_cast<CC1101State>(state & 0x70); }

        bool attachSerial(HardwareSerial& serial);
        bool begin();
        bool reset();
        state_t strobe(CC1101Register reg, bool awaitMiso = false);
        bool writeRegister(CC1101Register reg, uint8_t data);
        bool writeBurst(CC1101Register reg, const uint8_t* dataPtr, uint8_t size);
        int writeFIFO(const uint8_t* dataPtr, uint8_t size);
        uint8_t readRegister(CC1101Register reg);
        bool readBurst(CC1101Register reg, uint8_t* dataPtr, uint8_t size);
        int readFIFO(uint8_t* dataPtr, uint8_t size);
        bool setMode(CC1101Mode mode);
        bool setTxPower(CC1101TxPower power);
        float readRSSI();
        bool awaitMode(CC1101Mode mode, uint32_t timeoutMs);

    private:
        SPIClass _spi;
        int8_t _sckPin;
        int8_t _misoPin;
        int8_t _mosiPin;
        int8_t _csnPin;
        int8_t _gdo2Pin;
        int8_t _gdo0Pin;
        volatile CC1101Mode _mode;
        static const uint8_t _initConfig[];

        uint8_t getAddress(CC1101Register reg, bool read, bool burst);
        bool awaitMisoLow();
        bool select(bool awaitMiso = true);
        void deselect();
};

#endif