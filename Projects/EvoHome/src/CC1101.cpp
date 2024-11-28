#include <map>
#include <Tracer.h>
#include "CC1101.h"

const uint8_t _cc1101Config[] =
{
    0x2E,  //  IOCFG2 	 GDO2 not used [evofw3: 0x2E/0x02 (RX/TX) > not used/TX FIFO full] [evofw2: 0x0B > Serial Clock]
    0x2E,  //  IOCFG1 	 GDO1 not used
    0x2E,  //  IOCFG0	 GDO0 not used [evofw2: 0x0C > Sync Data Out]
    0x07,  //  FIFOTHR   default FIFO threshold: 32 bytes (half full)
    0xD3,  //  SYNC1     default
    0x91,  //  SYNC0     default
    0xFF,  //  PKTLEN	 default
    0x00,  //  PKTCTRL1  don't append status [evofw3: 0x04] [evofw2: 0x00]
    0x02,  //  PKTCTRL0  FIFO, infinite packet [evofw3: 0x32/0x02 (RX/TX) > Async Serial/FIFO, infinite package] [evofw2: 0x12 > Sync Serial, infinite packet]
    0x00,  //  ADDR      default
    0x00,  //  CHANNR    default
    0x0F,  //  FSCTRL1   default [evofw2: 0x06]
    0x00,  //  FSCTRL0   default
    0x21,  //  FREQ2     /
    0x65,  //  FREQ1     / 868.3 MHz
    0x6A,  //  FREQ0     / [evofw2: 0x6C]
    0x6A,  //  MDMCFG4   //
    0x83,  //  MDMCFG3   // DRATE_M=131 data rate=38,383.4838867Hz
    0x10,  //  MDMCFG2   GFSK, No Sync Word
    0x22,  //  MDMCFG1   FEC_EN=0, NUM_PREAMBLE=4, CHANSPC_E=2
    0xF8,  //  MDMCFG0   Channel spacing 199.951 KHz
    0x50,  //  DEVIATN
    0x07,  //  MCSM2     default
    0x30,  //  MCSM1     default
    0x18,  //  MCSM0     Auto-calibrate on Idle to RX+TX, Power on timeout 149-155 uS
    0x16,  //  FOCCFG    default
    0x6C,  //  BSCFG     default
    0x43,  //  AGCCTRL2
    0x40,  //  AGCCTRL1  default
    0x91,  //  AGCCTRL0  default
    0x87,  //  WOREVT1   default
    0x6B,  //  WOREVT0   default
    0xF8,  //  WORCTRL   default
    0x56,  //  FREND1    default
    0x10,  //  FREND0    default
    0xE9,  //  FSCAL3
    0x21,  //  FSCAL2 [evofw2: 0x2A]
    0x00,  //  FSCAL1
    0x1F,  //  FSCAL0
    0x41,  //  RCCTRL1   default
    0x00,  //  RCCTRL0   default
    0x59,  //  FSTEST    default
    0x7F,  //  PTEST     default
    0x3F,  //  AGCTEST   default
    0x81,  //  TEST2
    0x35,  //  TEST1
    0x09,  //  TEST0
};


CC1101::CC1101(uint8_t spiBus, int8_t sckPin, int8_t misoPin, int8_t mosiPin, int8_t csnPin)
    : _spi(spiBus)
{
    _sckPin = sckPin;
    _misoPin = misoPin;
    _mosiPin = mosiPin;
    _csnPin = csnPin;

    pinMode(_csnPin, OUTPUT);
    digitalWrite(_csnPin, HIGH);
}


bool CC1101::begin()
{
    Tracer tracer("CC1101::begin");

    _spi.begin(_sckPin, _misoPin, _mosiPin);
    _spi.setFrequency(1000000); // 1 MHz
    _spi.setBitOrder(SPI_MSBFIRST);
    _spi.setHwCs(false);

    return reset();
}


bool CC1101::reset()
{
    Tracer tracer("CC1101::reset");

    state_t state = strobe(CC1101Register::SRES, true); 
    TRACE("SRES strobe result: %d\n", state);
    if (state < 0) return false;

    _mode = CC1101Mode::Idle;

    return writeBurst(CC1101Register::IOCFG2, _cc1101Config, sizeof(_cc1101Config));
}


uint8_t CC1101::getAddress(CC1101Register reg, bool read, bool burst)
{
    uint8_t addr = static_cast<uint8_t>(reg);
    if (read) addr |= 0x80;
    if (burst) addr |= 0x40;
    return addr;
}


bool CC1101::awaitMisoLow()
{
    for (int retries = 0; retries < 100; retries++)
    {
        if (digitalRead(_misoPin) == LOW) return true;
        delay(1);
    }

    TRACE("CC1101: Timeout waiting for MISO to go low\n");
    deselect();
    return false;
}


bool CC1101::select(bool awaitMiso)
{
    digitalWrite(_csnPin, LOW);
    return awaitMiso ? awaitMisoLow() : true;
}


void CC1101::deselect()
{
    digitalWrite(_csnPin, HIGH);
}


state_t CC1101::strobe(CC1101Register reg, bool awaitMiso)
{
    if (!select()) return -1;

    uint8_t addr = getAddress(reg, false, false);
    uint8_t state = _spi.transfer(addr);

    if (awaitMiso && !awaitMisoLow()) return -1;
    deselect();

    return static_cast<state_t>(state);
}


bool CC1101::writeRegister(CC1101Register reg, uint8_t data)
{
    uint8_t addr = getAddress(reg, false, false);
    TRACE("CC1101 Write 0x%02X: 0x%02X\n", addr, data);

    if (!select()) return false;

    _spi.transfer(addr);
    _spi.transfer(data);

    deselect();
    return true;
}


bool CC1101::writeBurst(CC1101Register reg, const uint8_t* dataPtr, uint8_t size)
{
    uint8_t addr = getAddress(reg, false, true);
    TRACE("CC1101 Write Burst 0x%02X: %d bytes\n", addr, size);

    if (!select()) return false;

    _spi.transfer(addr);
    for (int i = 0; i < size; i++)
        _spi.transfer(dataPtr[i]);

    deselect();
    return true;
}


int CC1101::writeFIFO(const uint8_t* dataPtr, uint8_t size)
{
    if (_mode != CC1101Mode::Transmit) return -1;

    uint8_t txBytes = readRegister(CC1101Register::TXBYTES);
    if (txBytes & 0x80)
    {
        TRACE("CC1101: TX FIFO underflow\n");
        strobe(CC1101Register::SFTX);
        _mode = CC1101Mode::Idle;
        return -2;
    }

    txBytes = CC1101_FIFO_SIZE - txBytes; // Remaining space in TX FIFO
    uint8_t bytesToWrite = std::min(txBytes, size);
    if (bytesToWrite != 0)
    {
        if (!writeBurst(CC1101Register::FIFO, dataPtr, bytesToWrite))
        {
            TRACE("CC1101: writeBurst failed\n");
            return -3;
        }
    }

    return bytesToWrite;
}


uint8_t CC1101::readRegister(CC1101Register reg)
{
    uint8_t addr = getAddress(reg, true, false);

    if (!select()) return 0;

    _spi.transfer(addr);
    uint8_t result = _spi.transfer(0);
    deselect();

    //TRACE("CC1101 Read 0x%02X: 0x%02X\n", addr, result);
    return result;
}


bool CC1101::readBurst(CC1101Register reg, uint8_t* dataPtr, uint8_t size)
{
    uint8_t addr = getAddress(reg, true, true);
    TRACE("CC1101 Read Burst 0x%02X: %d bytes\n", addr, size);

    if (!select()) return false;

    _spi.transfer(addr);
    for (int i = 0; i < size; i++)
        dataPtr[i] = _spi.transfer(0);

    deselect();
    return true;
}


int CC1101::readFIFO(uint8_t* dataPtr, uint8_t size)
{
    if (_mode != CC1101Mode::Receive) return -1;

    uint8_t rxBytes = readRegister(CC1101Register::RXBYTES);
    if (rxBytes & 0x80)
    {
        TRACE("CC1101: RX FIFO overflow\n");
        strobe(CC1101Register::SFRX);
        _mode = CC1101Mode::Idle;
        return -2;
    }

    uint8_t bytesToRead = std::min(rxBytes, size);
    if (bytesToRead != 0)
    {
        if (!readBurst(CC1101Register::FIFO, dataPtr, bytesToRead))
        {
            TRACE("CC1101: readBurst failed\n");
            return -3;
        }
    }

    return bytesToRead;
}


float CC1101::readRSSI()
{
  int8_t rssi = static_cast<int8_t>(readRegister(CC1101Register::RSSI));
  return float(rssi) / 2 - 74;
}

bool CC1101::setMode(CC1101Mode mode)
{
    TRACE("CC1101::setMode(%d)\n", mode);

    if (mode == _mode) return true;

    CC1101Register strobeReg;
    CC1101State newState;
    switch (mode)
    {
        case CC1101Mode::Idle:
            strobeReg = CC1101Register::SIDLE;
            newState = CC1101State::IDLE;
            break;

        case CC1101Mode::Receive:
            strobeReg = CC1101Register::SRX;
            newState = CC1101State::RX;
            break;

        case CC1101Mode::Transmit:
            strobeReg = CC1101Register::STX;
            newState = CC1101State::TX;
            break;

        default:
            return false;
    }

    for (int retries = 0; retries < 100; retries++)
    {
        CC1101State state = getState(strobe(strobeReg));
        if (state == newState)
        {
            _mode = mode;
            return true;
        }
        delay(1);
    }

    TRACE("CC1101: Timeout waiting for state 0x%02X\n", newState);
    return false;
}


bool CC1101::setTxPower(CC1101TxPower power)
{
    uint8_t paLevel;
    switch (power)
    {
        case CC1101TxPower::Low:
            paLevel = 0x27; // -10 dBm @ 868 MHz
            break;

        case CC1101TxPower::Medium:
            paLevel = 0x50; // 0 dBm @ 868 MHz
            break;

        case CC1101TxPower::High:
            paLevel = 0xC2; // +10 dBm @ 868 MHz
            break;
        
        default:
            return false;
    }

    return writeRegister(CC1101Register::PATABLE, paLevel);
}

