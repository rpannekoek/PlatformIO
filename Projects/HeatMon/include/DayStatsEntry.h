struct DayStatsEntry
{
    time_t time;
    uint32_t valveActivatedSeconds = 0;
    float energyOut = 0; // kWh
    float energyIn = 0; // kWh

    float getCOP()
    {
        return (energyIn == 0) ? 0 : (energyOut / energyIn);
    }

    void writeRow(HtmlWriter& html, float maxEnergy, bool showValve)
    {
        html.writeRowStart();
        html.writeCell(formatTime("%d %b", time));
        if (showValve)
            html.writeCell(formatTimeSpan(valveActivatedSeconds, false));
        html.writeCell(energyOut, F("%0.2f"));
        html.writeCell(energyIn, F("%0.2f"));
        html.writeCell(getCOP());
        html.writeGraphCell(
            energyIn,
            energyOut,
            0,
            maxEnergy,
            F("eInBar"),
            F("energyBar"),
            false
            );
        html.writeRowEnd();
    }

    static void writeHeader(HtmlWriter& html, bool showValve)
    {
        html.writeRowStart();
        html.writeHeaderCell(F("Day"));
        if (showValve)
            html.writeHeaderCell(F("Valve on"));
        html.writeHeaderCell(F("E<sub>out</sub> (kWh)"));
        html.writeHeaderCell(F("E<sub>in</sub> (kWh)"));
        html.writeHeaderCell(F("COP"));
        html.writeRowEnd();
    }
};
