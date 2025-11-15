struct ChargeLogEntry
{
    time_t time;
    float currentLimit;
    float outputCurrent;
    float temperature;

    void update(float newCurrentLimit, float newOutputCurrent, float newTemperature)
    {
        currentLimit += newCurrentLimit;
        outputCurrent += newOutputCurrent;
        temperature += newTemperature;
    }

    void average(int count)
    {
        if (count != 0)
        {
            currentLimit /= count;
            outputCurrent /= count;
            temperature /= count;
        }
    }

    void reset(time_t startTime)
    {
        time = startTime;
        currentLimit = 0;
        outputCurrent = 0;
        temperature = 0;
    }

    bool equals(ChargeLogEntry* otherPtr)
    {
        return std::abs(currentLimit - otherPtr->currentLimit) < 0.1
            && std::abs(outputCurrent - otherPtr->outputCurrent) < 0.1
            && std::abs(temperature - otherPtr->temperature) < 0.2;
    }

    void writeCsv(Print& destination)
    {
        destination.printf(
            "%s;%0.1f;%0.1f;%0.1f\r\n",
            formatTime("%F %H:%M:%S", time),
            currentLimit,
            outputCurrent,
            temperature);
    }

    void writeRow(HtmlWriter& html)
    {
        html.writeRowStart();
        html.writeCell(formatTime("%d %b %H:%M", time));
        html.writeCell(currentLimit);
        html.writeCell(outputCurrent);
        html.writeCell(temperature);
        html.writeRowEnd();
    }
};