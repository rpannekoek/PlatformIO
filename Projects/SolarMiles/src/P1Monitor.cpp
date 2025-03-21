#include "P1Monitor.h"
#include <Tracer.h>

enum DayStatsProperty
{
    Vmin,
    Vmax,
    Pmin,
    Pmax,
    Ein,
    Eout,
    Esolar,
    Egross
};


bool P1MonitorClass::begin(const char* host, uint32_t pollInterval, float powerDelta, float voltageDelta)
{
    _pollInterval = pollInterval;
    _powerDelta = powerDelta;
    _voltageDelta = voltageDelta;

    bool success = _p1Client.begin(host);
    if (success) _p1Client.requestData();
    return success;
}


bool P1MonitorClass::run(time_t time)
{
    if (!_p1Client.isInitialized) return false;

    if (_p1Client.isRequestPending())
    {
        int httpResult = _p1Client.requestData();
        if (httpResult == HTTP_REQUEST_PENDING) return false;
        if (httpResult != HTTP_CODE_OK)
        {
            _logger.logEvent("P1Monitor: %s", _p1Client.getLastError().c_str());
            return false;
        }    
    }

    if (time < _nextPollTime) return false;

    _nextPollTime = time + _pollInterval;
    updateLog(time);
    _p1Client.requestData();
    return true;
}


void P1MonitorClass::updateLog(time_t time)
{
    Tracer tracer("P1MonitorClass::updateLog");

    if (_newLogEntry.time == 0)
         _newLogEntry.reset(time + P1_AGGREGATION_INTERVAL);

    if (_currentDayStatsPtr == nullptr || time >= _currentDayStatsPtr->day + SECONDS_PER_DAY)
    {
        P1MonitorDayStatsEntry dayStatsEntry;
        dayStatsEntry.day = getStartOfDay(time);
        _currentDayStatsPtr = DayStats.add(&dayStatsEntry);
    }

    int i = 0;
    for (PhaseData& phaseData : _p1Client.electricity)
    {
        _newLogEntry.voltage[i] += phaseData.Voltage;
        _newLogEntry.power[i] += phaseData.Power;
        _currentDayStatsPtr->phase[i].update(phaseData.Voltage, phaseData.Power, solarPower[i], _pollInterval);
        i++;
    }
    _aggregations++;

    if (_p1Client.gasTimestamp != _lastGasTimestamp)
    {
        if (_lastGasTimestamp != 0)
        {
            uint32_t seconds = _p1Client.gasTimestamp - _lastGasTimestamp;
            float gasDeltaM3 = _p1Client.gasM3 - _lastGasM3;
            _gasPower = gasDeltaM3 * GAS_CALORIFIC_VALUE * SECONDS_PER_HOUR / seconds;
            TRACE("Gas delta: %0.3f m3 in %d s => %0.1f W\n", gasDeltaM3, seconds, _gasPower);
        }
        _lastGasM3 = _p1Client.gasM3;
        _lastGasTimestamp = _p1Client.gasTimestamp;
    }

    if (time >= _newLogEntry.time)
    {
        _newLogEntry.aggregate(_aggregations);
        _newLogEntry.gasPower = _gasPower;
        if ((_lastLogEntryPtr == nullptr) || !_newLogEntry.equals(_lastLogEntryPtr, _powerDelta, _voltageDelta))
        {
            _lastLogEntryPtr = Log.add(&_newLogEntry);
            logEntriesToSync = std::min(logEntriesToSync + 1, Log.size());
        }
        _newLogEntry.reset(time + P1_AGGREGATION_INTERVAL);
        _aggregations = 0;
    }
}


void P1MonitorClass::writeStatus(HtmlWriter& html)
{
    Tracer tracer("P1MonitorClass::writeStatus");

    html.writeSectionStart("Status");
    html.writeTableStart();
    html.writeRow("P1 Meter", "%d ms", _p1Client.getResponseTimeMs());
    html.writeRow("FTP Entries", "%d", logEntriesToSync);
    html.writeTableEnd();
    html.writeSectionEnd();
}


void P1MonitorClass::writeCurrentValues(HtmlWriter& html, int maxPhasePower)
{
    Tracer tracer("P1MonitorClass::writeCurrentValues");

    int maxTotalPower = maxPhasePower * _p1Client.electricity.size();
    float gasKWh = _p1Client.gasM3 * GAS_CALORIFIC_VALUE / 1000;
    PhaseData total
    {
        .Name = "Total",
        .Voltage = 0,
        .Current = 0,
        .Power = 0
    };
    float totalSolarPower = 0;

    html.writeSectionStart("Current values");
    html.writeTableStart();

    int i = 0;
    for (PhaseData& phaseData : _p1Client.electricity)
    {
        html.writeRowStart();
        html.writeHeaderCell(phaseData.Name);
        html.writeCell(phaseData.Voltage, F("%0.1f V"));
        html.writeCell(phaseData.Current, F("%0.1f A"));
        html.writeCellStart("");
        html.writeDiv((phaseData.Power >= 0) ? "pIn" : "pOut", F("%0.0f W"), phaseData.Power);
        html.writeDiv("pSolar", F("%0.0f W"), solarPower[i]);
        html.writeCellEnd();
        html.writeCellStart("graph");
        html.writeMeterDiv(abs(phaseData.Power), 0, maxPhasePower, (phaseData.Power >= 0) ? "deliveredBar" : "returnedBar");
        html.writeMeterDiv(solarPower[i], 0, maxPhasePower, "solarBar");
        html.writeCellEnd();
        html.writeRowEnd();

        total.Voltage += phaseData.Voltage;
        total.Current += phaseData.Current;
        total.Power += phaseData.Power;
        totalSolarPower += solarPower[i];
        i++;
    }

    total.Voltage /= _p1Client.electricity.size();

    html.writeRowStart();
    html.writeHeaderCell(total.Name);
    html.writeCell(total.Voltage, F("%0.1f V"));
    html.writeCell(total.Current, F("%0.1f A"));
    html.writeCellStart("");
    html.writeDiv((total.Power >= 0) ? "pIn" : "pOut", F("%0.0f W"), total.Power);
    html.writeDiv("pSolar", F("%0.0f W"), totalSolarPower);
    html.writeCellEnd();
    html.writeCellStart("graph");
    html.writeMeterDiv(abs(total.Power), 0, maxTotalPower, (total.Power >= 0) ? "deliveredBar" : "returnedBar");
    html.writeMeterDiv(totalSolarPower, 0, maxTotalPower, "solarBar");
    html.writeCellEnd();
    html.writeRowEnd();

    html.writeRowStart();
    html.writeHeaderCell("Gas");
    html.writeHeaderCell(String(gasKWh, 1) + " kWh", 2);
    html.writeCell(_gasPower, F("%0.0f W"));
    html.writeCellStart("graph");
    html.writeMeterDiv(_gasPower, 0, maxTotalPower, "gasBar");
    html.writeCellEnd();
    html.writeRowEnd();

    html.writeTableEnd();
    html.writeSectionEnd();
}


void P1MonitorClass::writeDayStats(HtmlWriter& html)
{
    Tracer tracer("P1MonitorClass::writeDayStats");

    html.writeSectionStart("Day statistics");
    html.writeTableStart();
    html.writeRowStart();
    html.writeHeaderCell("Day", 2);
    P1MonitorDayStatsEntry* dayStatsEntryPtr = DayStats.getFirstEntry();
    while (dayStatsEntryPtr != nullptr)
    {
        html.writeHeaderCell(formatTime("%a", dayStatsEntryPtr->day));
        dayStatsEntryPtr = DayStats.getNextEntry();
    }
    html.writeRowEnd();

    int phase = 0;
    for (PhaseData& phaseData : _p1Client.electricity)
    {
        html.writeRowStart();
        html.writeHeaderCell(phaseData.Name, 0, 8);
        html.writeHeaderCell("U<sub>min</sub>");
        writeDayStats(html, phase, Vmin);
        html.writeRowEnd();
        html.writeRowStart();
        html.writeHeaderCell("U<sub>max</sub>");
        writeDayStats(html, phase, Vmax);
        html.writeRowEnd();
        html.writeRowStart();
        html.writeHeaderCell("P<sub>min</sub>");
        writeDayStats(html, phase, Pmin);
        html.writeRowEnd();
        html.writeRowStart();
        html.writeHeaderCell("P<sub>max</sub>");
        writeDayStats(html, phase, Pmax);
        html.writeRowEnd();
        html.writeRowStart();
        html.writeHeaderCell("E<sub>in</sub>");
        writeDayStats(html, phase, Ein);
        html.writeRowEnd();
        html.writeRowStart();
        html.writeHeaderCell("E<sub>out</sub>");
        writeDayStats(html, phase, Eout);
        html.writeRowEnd();
        html.writeRowStart();
        html.writeHeaderCell("E<sub>solar</sub>");
        writeDayStats(html, phase, Esolar);
        html.writeRowEnd();
        html.writeRowStart();
        html.writeHeaderCell("E<sub>gross</sub>");
        writeDayStats(html, phase, Egross);
        html.writeRowEnd();

        phase++;
    }

    html.writeTableEnd();
    html.writeSectionEnd();
}


void P1MonitorClass::writeDayStats(HtmlWriter& html, int phase, int property)
{
    P1MonitorDayStatsEntry* dayStatsEntryPtr = DayStats.getFirstEntry();
    while (dayStatsEntryPtr != nullptr)
    {
        PhaseDayStats& phaseStats = dayStatsEntryPtr->phase[phase];

        switch (property)
        {
            case Vmin:
                html.writeCell(phaseStats.minVoltage, F("%0.1f V"));
                break;

            case Vmax:
                html.writeCell(phaseStats.maxVoltage, F("%0.1f V"));
                break;
            
            case Pmin:
                html.writeCell(phaseStats.minPower, F("%0.0f W"));
                break;

            case Pmax:
                html.writeCell(phaseStats.maxPower, F("%0.0f W"));
                break;

            case Ein:
                html.writeCell(phaseStats.energyIn, F("%0.0f Wh"));
                break;

            case Eout:
                html.writeCell(phaseStats.energyOut, F("%0.0f Wh"));
                break;

            case Esolar:
                html.writeCell(phaseStats.solarEnergy, F("%0.0f Wh"));
                break;

            case Egross:
                html.writeCell(phaseStats.grossEnergy, F("%0.0f Wh"));
                break;
        }
        dayStatsEntryPtr = DayStats.getNextEntry();
    }
}


void P1MonitorClass::writeLog(HtmlWriter& html, int page, int pageSize)
{
    Tracer tracer("P1MonitorClass::writeLog");

    html.writeSectionStart("Power log");

    int totalPages = (Log.count() - 1) / pageSize + 1;
    html.writePager(totalPages, page);

    P1MonitorLogEntry* logEntryPtr = Log.getFirstEntry();
    for (int i = 0; (i < page * pageSize) && (logEntryPtr != nullptr); i++)
        logEntryPtr = Log.getNextEntry();

    html.writeTableStart();
    html.writeRowStart();
    html.writeHeaderCell("Time", 0, 2);
    for (PhaseData& phaseData : _p1Client.electricity)
        html.writeHeaderCell(phaseData.Name, 2);
    html.writeHeaderCell("Gas");
    html.writeRowEnd();
    html.writeRowStart();
    for (int i = 0; i < _p1Client.electricity.size(); i++)
    {
        html.writeHeaderCell("Voltage");
        html.writeHeaderCell("Power");
    }
    html.writeHeaderCell("Power");
    html.writeRowEnd();

    for (int i = 0; i < pageSize && (logEntryPtr != nullptr); i++)
    {
        logEntryPtr->writeTableRow(html);
        logEntryPtr = Log.getNextEntry();        
    }
    html.writeTableEnd();
    html.writeSectionEnd();
}


void P1MonitorClass::writeLogCsv(Print& output, int entries)
{
    P1MonitorLogEntry* logEntryPtr = Log.getEntryFromEnd(entries);
    while (logEntryPtr != nullptr)
    {
        logEntryPtr->writeCsv(output);
        logEntryPtr = Log.getNextEntry();
    }
}


void P1MonitorLogEntry::writeTableRow(HtmlWriter& html)
{
    html.writeRowStart();
    html.writeCell(formatTime("%H:%M", time));
    for (int i = 0; i < 3; i++)
    {
        html.writeCell(voltage[i]);
        html.writeCell(power[i], F("%0.0f"));
    }
    html.writeCell(gasPower, F("%0.0f"));
    html.writeRowEnd();
}


void P1MonitorLogEntry::writeCsv(Print& output)
{
    output.printf("%s;", formatTime("%F %H:%M", time));
    for (int i = 0; i < 3; i++)
    {
        output.printf("%0.1f;", voltage[i]);
        output.printf("%0.0f;", power[i]);        
    }
    output.printf("%0.0f", gasPower);
    output.println();
}
