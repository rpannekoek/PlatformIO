struct DayStatsEntry
{
    time_t startTime;
    time_t stopTime;
    uint32_t antiFreezeSeconds = 0;
    uint32_t onSeconds = 0;
    uint32_t onCount = 0;
    uint32_t defrosts = 0;
    float energyIn = 0; // kWh
    float energyOut = 0; // kWh


    uint32_t inline getAvgOnSeconds()
    {
        return (onCount == 0) ? 0 : onSeconds / onCount;
    }


    float inline getCOP()
    {
        return (energyIn == 0) ? 0 : energyOut / energyIn;
    }


    void update(time_t time, uint32_t secondsSinceLastUpdate, float powerInKW, float powerOutKW, bool antiFreezeActivated)
    {
        float hoursSinceLastUpdate = float(secondsSinceLastUpdate) / 3600;

        if (powerInKW > 0)
        {
            if (onSeconds == 0)
                startTime = time;
            stopTime = time;
            onSeconds += secondsSinceLastUpdate;
            energyIn += powerInKW * hoursSinceLastUpdate;
        }

        if (antiFreezeActivated)
            antiFreezeSeconds += secondsSinceLastUpdate;

        energyOut += powerOutKW * hoursSinceLastUpdate;
    }

    void writeRow(HtmlWriter& html, float maxEnergy)
    {
        html.writeRowStart();
        html.writeCell(formatTime("%a", startTime));
        html.writeCell(formatTime("%H:%M", startTime));
        html.writeCell(formatTime("%H:%M", stopTime));
        html.writeCell(formatTimeSpan(onSeconds));
        html.writeCell(formatTimeSpan(getAvgOnSeconds()));
        html.writeCell(onCount);
        html.writeCell(defrosts);
        html.writeCell(formatTimeSpan(antiFreezeSeconds, false));
        html.writeCell(energyIn, F("%0.2f"));
        html.writeCell(energyOut, F("%0.2f"));
        html.writeCell(getCOP());
        html.writeGraphCell(
            energyIn,
            energyOut,
            0,
            maxEnergy,
            F("inBar"),
            F("outBar"),
            false);
        html.writeRowEnd();
    }

    static void writeHeader(HtmlWriter& html)
    {
        html.writeRowStart();
        html.writeHeaderCell(F("Day"));
        html.writeHeaderCell(F("Start"));
        html.writeHeaderCell(F("Stop"));
        html.writeHeaderCell(F("On time"));
        html.writeHeaderCell(F("Avg on"));
        html.writeHeaderCell(F("Runs"));
        html.writeHeaderCell(F("&#10054;")); // Defrosts
        html.writeHeaderCell(F("Anti-freeze"));
        html.writeHeaderCell(F("E<sub>in</sub> (kWh)"));
        html.writeHeaderCell(F("E<sub>out</sub> (kWh)"));
        html.writeHeaderCell(F("COP"));
        html.writeRowEnd();
    }
};
