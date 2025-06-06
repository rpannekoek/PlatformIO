#ifndef HTMLWRITER_H
#define HTMLWRITER_H

#include <StringBuilder.h>
#include <Navigation.h>

class HtmlWriter
{
    public:
        // Constructor
        HtmlWriter(StringBuilder& output, PGM_P icon, PGM_P css, size_t maxBarLength = 50);

        void setTitlePrefix(const String& prefix);

        void writeHeader(const String& title, bool includeHomePageLink, bool includeHeading, uint16_t refreshInterval = 0);
        void writeHeader(const String& title, const Navigation& navigation, uint16_t refreshInterval = 0);
        void writeFooter();

        void writeMeterDiv(float value, float minValue, float maxValue, const String& cssClass);
        void writeBar(float value, const String& cssClass, bool fill, bool useDiv = true, size_t maxBarLength = 0);
        void writeStackedBar(float value1, float value2, const String& cssClass1, const String& cssClass2, bool fill, bool useDiv = true);
        void writeGraphCell(float value, const String& barCssClass, bool fill, size_t maxBarLength = 0);
        void writeGraphCell(float value1, float value2, const String& barCssClass1, const String& barCssClass2, bool fill);
        void writeGraphCell(float value, float minValue, float maxValue, const String& cssClass, bool fill);

        void writeFormStart(const String& action, const String& cssClass = String());
        void writeFormEnd();
        void writeSubmitButton();
        void writeSubmitButton(const String& label, const String& cssClass = String("submit"));
        void writeLabel(const String& label, const String& forId);
        void writeTextBox(const String& name, const String& label, const String& value, uint16_t maxLength, const String& type = String("text"));
        void writeNumberBox(const String& name, const String& label, float value, float minValue, float maxValue, int decimals = 0);
        void writeCheckbox(const String& name, const String& label, bool value);
        void writeRadioButtons(const String& name, const String& label, const char** values, int numValues, int index);
        void writeSlider(const String& name, const String& label, const String& unitOfMeasure, int value, int minValue, int maxValue, int denominator = 1);
        void writeDropdown(const String& name, const String& label, const char** values, int numValues, int index = -1);

        void writeHeading(const String& title, int level = 1);
        void writeSectionStart(const String& title);
        void writeSectionEnd();
        void writeDivStart(const String& cssClass = String());
        void writeDivEnd();
        void writeDiv(const String& format, ...);
        void writeDiv(const String& cssClass, const __FlashStringHelper* format, ...);
        void writePreStart(const String& cssClass = String());
        void writePreEnd();

        void writeTableStart();
        void writeTableEnd();
        void writeRowStart();
        void writeRowStart(const String& cssClass);
        void writeRowEnd();
        void writeCellStart(const String& cssClass);
        void writeCellEnd();
        void writeHeaderCell(const String& value, int colspan = 0, int rowspan = 0);
        void writeCell(const String& format, ...);
        void writeCell(const char* value);
        void writeCell(int value);
        void writeCell(uint32_t value);
        void writeCell(float value, const __FlashStringHelper* format = nullptr);
        void writeRow(const String& name, const String& format, ...);

        void writePager(int totalPages, int currentPage);

        void writeParagraph(const String& format, ...);

        void writeLink(const String& href, const String& label, const String& cssClass = String());
        void writeActionLink(
            const String& action,
            const String& label,
            time_t currentTime,
            const String& cssClass = String("actionLink"),
            const String& icon = String());

    private:
        StringBuilder& _output;
        String _icon;
        String _css;
        String _titlePrefix;
        char _strBuffer[256];
        size_t _maxBarLength;
};

#endif