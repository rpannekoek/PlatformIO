#include <OTGW.h>

struct OpenThermLogEntry
{
    time_t time;
    uint16_t thermostatTSet;
    uint16_t thermostatMaxRelModulation;
    uint16_t boilerStatus;
    uint16_t boilerTSet;
    uint16_t boilerRelModulation;
    uint16_t tBoiler;
    uint16_t tReturn;
    uint16_t tBuffer;
    uint16_t tOutside;
    uint16_t pressure;
    uint16_t flowRate;
    uint16_t pHeatPump; // kW in OT f8.8 format
    uint16_t tRoom;
    float deviationHours;

    bool equals(OpenThermLogEntry* otherPtr)
    {
        return (otherPtr->thermostatTSet == thermostatTSet) &&
            (otherPtr->thermostatMaxRelModulation == thermostatMaxRelModulation) &&
            (otherPtr->boilerStatus == boilerStatus) &&
            (otherPtr->boilerTSet == boilerTSet) &&
            (otherPtr->boilerRelModulation == boilerRelModulation) &&
            isSimilar(otherPtr->tBoiler, tBoiler) &&
            isSimilar(otherPtr->tReturn, tReturn) &&
            isSimilar(otherPtr->tBuffer, tBuffer) &&
            isSimilar(otherPtr->tOutside, tOutside) &&
            isSimilar(otherPtr->pressure, pressure, 4) &&
            isSimilar(otherPtr->flowRate, flowRate) &&
            isSimilar(otherPtr->pHeatPump, pHeatPump, 4) &&
            isSimilar(otherPtr->tRoom, tRoom) &&
            abs(otherPtr->deviationHours - deviationHours) < 0.01;
    }

    static bool isSimilar(int lhs, int rhs, int maxDiff = 32)
    {
        return abs(rhs - lhs) < maxDiff;
    }

    void writeCsv(time_t time, Print& destination)
    {
        int masterStatus = boilerStatus >> 8;
        int slaveStatus = boilerStatus & 0xFF;

        destination.print(formatTime("%F %H:%M:%S", time));
        destination.printf(";%d;%d", masterStatus, slaveStatus);
        destination.printf(";%d", OpenThermGateway::getInteger(thermostatMaxRelModulation));
        destination.printf(";%d", OpenThermGateway::getInteger(thermostatTSet));
        destination.printf(";%d", OpenThermGateway::getInteger(boilerTSet));
        destination.printf(";%0.1f", OpenThermGateway::getDecimal(tBoiler));
        destination.printf(";%0.1f", OpenThermGateway::getDecimal(tReturn));
        destination.printf(";%0.1f", OpenThermGateway::getDecimal(tBuffer));
        destination.printf(";%0.1f", OpenThermGateway::getDecimal(tOutside));
        destination.printf(";%0.2f", OpenThermGateway::getDecimal(pHeatPump));
        destination.printf(";%0.2f", OpenThermGateway::getDecimal(pressure));
        destination.printf(";%d", OpenThermGateway::getInteger(boilerRelModulation));
        destination.printf(";%0.1f", OpenThermGateway::getDecimal(flowRate));
        destination.printf(";%0.1f", OpenThermGateway::getDecimal(tRoom));
        destination.printf(";%0.2f", deviationHours);
        destination.println();
    }

    void writeRow(HtmlWriter& html)
    {
        html.writeRowStart();
        html.writeCell(formatTime("%H:%M:%S", time));
        html.writeCell(OpenThermGateway::getMasterStatus(boilerStatus));
        html.writeCell(OpenThermGateway::getSlaveStatus(boilerStatus));
        html.writeCell(OpenThermGateway::getInteger(thermostatMaxRelModulation));
        html.writeCell(OpenThermGateway::getInteger(thermostatTSet));
        html.writeCell(OpenThermGateway::getInteger(boilerTSet));
        html.writeCell(OpenThermGateway::getDecimal(tBoiler));
        html.writeCell(OpenThermGateway::getDecimal(tReturn));
        html.writeCell(OpenThermGateway::getDecimal(tBuffer));
        html.writeCell(OpenThermGateway::getDecimal(tOutside));
        html.writeCell(OpenThermGateway::getDecimal(pHeatPump), F("%0.2f"));
        html.writeCell(OpenThermGateway::getDecimal(pressure), F("%0.2f"));
        html.writeCell(OpenThermGateway::getInteger(boilerRelModulation));
        html.writeCell(OpenThermGateway::getDecimal(flowRate));
        html.writeCell(OpenThermGateway::getDecimal(tRoom));
        html.writeCell(deviationHours, F("%0.2f"));
        html.writeRowEnd();
    }
};
