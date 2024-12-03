#include <map>
#include <Tracer.h>
#include "CC1101.h"

const uint8_t CC1101::_initConfig[] =
{
    0x0D,  //  IOCFG2 	 GDO2 Async Data Out [evofw3: 0x0D > Async Data Out] [evofw2: 0x0B > Serial Clock]
    0x2E,  //  IOCFG1 	 GDO1 not used
    0x2E,  //  IOCFG0	 GDO0 not used [evofw3: 0x2E/0x02 (RX/TX) > not used/TX FIFO full][evofw2: 0x0C > Sync Data Out]
    0x07,  //  FIFOTHR   default FIFO threshold: 32 bytes (half full)
    0xFF,  //  SYNC1     |
    0x00,  //  SYNC0     | Sync Word: 0xFF00 (not used in async mode)
    0xFF,  //  PKTLEN	 default
    0x00,  //  PKTCTRL1  PQT=0, don't append status [evofw3: 0x04] [evofw2: 0x00]
    0x32,  //  PKTCTRL0  Async Serial, infinite packet, no CRC (see setMode) [evofw3: 0x32/0x02 (RX/TX) > Async Serial/FIFO, infinite package] [evofw2: 0x12 > Sync Serial, infinite packet]
    0x00,  //  ADDR      default
    0x00,  //  CHANNR    default
    0x0F,  //  FSCTRL1   default [evofw2: 0x06]
    0x00,  //  FSCTRL0   default
    0x21,  //  FREQ2     |
    0x65,  //  FREQ1     | 868.3 MHz
    0x6A,  //  FREQ0     | [evofw2: 0x6C]
    0x6A,  //  MDMCFG4   |
    0x83,  //  MDMCFG3   | DRATE_M=131 data rate=38,383.4838867Hz
    0x10,  //  MDMCFG2   GFSK, no Sync Word, no carrier sense [evofw3: 0x10]
    0x22,  //  MDMCFG1   4 preamble bytes, No FEC (not used in async mode)
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
    0x1F   //  FSCAL0
};


CC1101::CC1101(uint8_t spiBus, int8_t sckPin, int8_t misoPin, int8_t mosiPin, int8_t csnPin, int8_t gdo2Pin)
    : _spi(spiBus)
{
    _sckPin = sckPin;
    _misoPin = misoPin;
    _mosiPin = mosiPin;
    _csnPin = csnPin;
    _gdo2Pin = gdo2Pin;

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

    if (!reset())
    {
        TRACE("CC1101 reset failed\n");
        return false;
    }

    // Sanity-check: read back config registers;
    uint8_t configRegisters[sizeof(_initConfig)];
    if (!readBurst(CC1101Register::IOCFG2, configRegisters, sizeof(configRegisters)))
    {
        TRACE("CC1101 readBurst failed\n");
        return false;
    }

    TRACE("CC1101 config registers:\n");
    Tracer::hexDump(configRegisters, sizeof(configRegisters));

    uint8_t rxBytes = readRegister(CC1101Register::RXBYTES);
    TRACE("CC1101 RXBYTES: 0x%02X\n", rxBytes);

    return true;
}


bool CC1101::reset()
{
    Tracer tracer("CC1101::reset");

    state_t state = strobe(CC1101Register::SRES, true); 
    if (state < 0) return false;

    _mode = CC1101Mode::Idle;

    if (!writeBurst(CC1101Register::IOCFG2, _initConfig, sizeof(_initConfig)))
    {
        TRACE("CC1101 writeBurst failed\n");
        return false;
    }

    uint8_t marcState = readRegister(CC1101Register::MARCSTATE);
    TRACE("CC1101 MARCSTATE: 0x%02X\n", marcState);

    return ((marcState & 0x1F) == 1); // Should be in Idle state after reset
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
        delayMicroseconds(1);
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
    uint8_t status = _spi.transfer(addr);

    if (awaitMiso && !awaitMisoLow()) return -1;
    deselect();

    //TRACE("CC1101 strobe 0x%02X. Status: 0x%02X\n", addr, status);

    return static_cast<state_t>(status);
}


bool CC1101::writeRegister(CC1101Register reg, uint8_t data)
{
    uint8_t addr = getAddress(reg, false, false);
    //TRACE("CC1101 Write 0x%02X: 0x%02X\n", addr, data);

    if (!select()) return false;

    _spi.transfer(addr);
    _spi.transfer(data);

    deselect();
    return true;
}


bool CC1101::writeBurst(CC1101Register reg, const uint8_t* dataPtr, uint8_t size)
{
    uint8_t addr = getAddress(reg, false, true);
    //TRACE("CC1101 Write Burst 0x%02X: %d bytes\n", addr, size);

    if (!select()) return false;

    _spi.transfer(addr);
    for (int i = 0; i < size; i++)
        _spi.transfer(dataPtr[i]);

    deselect();
    return true;
}


int CC1101::writeFIFO(const uint8_t* dataPtr, uint8_t size)
{
    if (_mode == CC1101Mode::Receive) return CC1101_ERR_INVALID_STATE;

    uint8_t txBytes = readRegister(CC1101Register::TXBYTES);
    if (txBytes & 0x80)
    {
        //TRACE("CC1101: TX FIFO underflow; flushing.\n");
        strobe(CC1101Register::SFTX);
        _mode = CC1101Mode::Idle;
        return CC1101_ERR_TX_FIFO_UNDERFLOW;
    }

    txBytes = CC1101_FIFO_SIZE - txBytes; // Remaining space in TX FIFO
    uint8_t bytesToWrite = std::min(txBytes, size);
    if (bytesToWrite != 0)
    {
        if (!writeBurst(CC1101Register::FIFO, dataPtr, bytesToWrite))
        {
            TRACE("CC1101: writeBurst failed\n");
            return CC1101_ERR_GENERAL;
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
    //TRACE("CC1101 Read Burst 0x%02X: %d bytes\n", addr, size);

    if (!select()) return false;

    _spi.transfer(addr);
    for (int i = 0; i < size; i++)
        dataPtr[i] = _spi.transfer(0);

    deselect();
    return true;
}


int CC1101::readFIFO(uint8_t* dataPtr, uint8_t size)
{
    if (_mode != CC1101Mode::Receive) return CC1101_ERR_INVALID_STATE;

    uint8_t rxBytes = readRegister(CC1101Register::RXBYTES);
    if (rxBytes & 0x80)
    {
        //TRACE("CC1101: RX FIFO overflow; flushing.\n");
        strobe(CC1101Register::SFRX);
        _mode = CC1101Mode::Idle;
        return CC1101_ERR_RX_FIFO_OVERFLOW;
    }

    if (rxBytes > 1) rxBytes--; // See CC1101 errata

    uint8_t bytesToRead = std::min(rxBytes, size);
    if (bytesToRead != 0)
    {
        if (!readBurst(CC1101Register::FIFO, dataPtr, bytesToRead))
        {
            TRACE("CC1101: readBurst failed\n");
            return CC1101_ERR_GENERAL;
        }
    }

    return bytesToRead;
}


float CC1101::readRSSI()
{
  int8_t rssi = static_cast<int8_t>(readRegister(CC1101Register::RSSI));
  return float(rssi) / 2 - 74;
}


bool CC1101::awaitMode(CC1101Mode mode, uint32_t timeoutMs)
{
    const int delayMs = 10;
    int waitedMs = 0;
    while (_mode != mode)
    {
        if (waitedMs >= timeoutMs) return false;
        waitedMs += delayMs;
        delay(delayMs);
    }
    return true;
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
            if (!writeRegister(CC1101Register::PKTCTRL0, 0x32)) // Async, infinite packet
                TRACE("Unable to set PCKTCTRL0\n");
            break;

        case CC1101Mode::Transmit:
            strobeReg = CC1101Register::STX;
            newState = CC1101State::TX;
            if (!writeRegister(CC1101Register::PKTCTRL0, 0x00)) // FIFO, fixed packet
                TRACE("Unable to set PCKTCTRL0\n");
            break;

        default:
            return false;
    }

    uint8_t status;
    for (int retries = 0; retries < 20; retries++)
    {
        status = strobe(strobeReg);
        CC1101State state = getState(status);
        if (state == newState)
        {
            _mode = mode;
            return true;
        }
        delay(1);
    }

    TRACE("CC1101: Timeout waiting for state 0x%02X. Status: 0x%02X\n", newState, status);
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


