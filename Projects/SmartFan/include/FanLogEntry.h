#include <Arduino.h>
#include <TimeUtils.h>
#include <HtmlWriter.h>

struct FanLogEntry
{
    time_t time;
    float humidity; // %
    float humidityBaselineDelta; // %
    float temperature;
    float pressure; // hPa
    uint8_t fanLevel;

    bool equals(const FanLogEntry* other)
    {
        return abs(humidity - other->humidity) < 0.1
            && abs(humidityBaselineDelta - other->humidityBaselineDelta) < 0.1
            && abs(temperature - other->temperature) < 0.1
            && abs(pressure - other->pressure) < 0.1
            && (fanLevel == other->fanLevel);
    }

    void writeCsv(Print& output)
    {
        output.printf("%s;", formatTime("%F %T", time));
        output.printf("%0.1f;", humidity);
        output.printf("%0.1f;", humidityBaselineDelta);
        output.printf("%0.1f;", temperature);
        output.printf("%0.1f;", pressure);
        output.printf("%d", fanLevel);
        output.println();
    }

    void writeHtml(HtmlWriter& html)
    {
        html.writeRowStart();
        html.writeCell(formatTime("%T", time));
        html.writeCell(humidity);
        html.writeCell(humidityBaselineDelta);
        html.writeCell(temperature);
        html.writeCell(pressure);
        html.writeCell(fanLevel);
        html.writeRowEnd();
    }
};