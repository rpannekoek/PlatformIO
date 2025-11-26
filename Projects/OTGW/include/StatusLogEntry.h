struct StatusLogEntry
{
    time_t startTime;
    time_t stopTime;
    uint32_t chSeconds = 0;
    uint32_t dhwSeconds = 0;
    uint32_t overrideSeconds = 0;
    uint32_t flameSeconds = 0;

    static void writeHeader(HtmlWriter& html)
    {
        html.writeRowStart();
        html.writeHeaderCell(F("Day"));
        html.writeHeaderCell(F("CH start"));
        html.writeHeaderCell(F("CH stop"));
        html.writeHeaderCell(F("Override"));
        html.writeHeaderCell(F("CH on"));
        html.writeHeaderCell(F("DHW on"));
        html.writeHeaderCell(F("Flame"));
        html.writeRowEnd();
    }
    
    void writeRow(HtmlWriter& html, uint32_t maxFlameSeconds)
    {
        html.writeRowStart();
        html.writeCell(formatTime("%a", startTime));
        html.writeCell(formatTime("%H:%M", startTime));
        html.writeCell(formatTime("%H:%M", stopTime));
        html.writeCell(formatTimeSpan(overrideSeconds));
        html.writeCell(formatTimeSpan(chSeconds));
        html.writeCell(formatTimeSpan(dhwSeconds));
        html.writeCell(formatTimeSpan(flameSeconds));
        html.writeGraphCell(flameSeconds, 0, maxFlameSeconds, F("flameBar"), false);
        html.writeRowEnd();
    }
};
