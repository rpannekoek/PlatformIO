#include <ESPCoreDump.h>

#ifdef ESP32
bool writeCoreDump(Print& output)
{
    esp_core_dump_summary_t coreDumpSummary;
    esp_err_t err = esp_core_dump_get_summary(&coreDumpSummary);
    if (err != ESP_OK) 
    {
        output.printf("No core dump available. err: %d\n", err);
        return false;
    };
    
    output.printf("PC: 0x%08lx\n", coreDumpSummary.exc_pc);
#ifndef ARDUINO_LOLIN_C3_MINI
    output.printf("EXCCAUSE: 0x%08lx\n", coreDumpSummary.ex_info.exc_cause);
    output.printf("EXCVADDR: 0x%08lx\n", coreDumpSummary.ex_info.exc_vaddr);
    output.print("Backtrace: ");
    for (int i = 0; i < coreDumpSummary.exc_bt_info.depth; i++)
        output.printf("0x%08lx ", coreDumpSummary.exc_bt_info.bt[i]);
    output.println();
#endif

    esp_core_dump_image_erase();

    return true;
}
#else
bool writeCoreDump(Print& output)
{
    output.println("Core dump not supported on ESP8266.\n");
    return false;
}
#endif
