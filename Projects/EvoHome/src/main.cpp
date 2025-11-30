#include <WiFiStateMachine.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <LED.h>
#include <Log.h>
#include <HtmlWriter.h>
#include <Navigation.h>
#include "Constants.h"
#include "PersistentData.h"
#include "RAMSES2.h"
#include "PacketStats.h"
#include "EvoHomeInfo.h"

enum FileId
{
    Logo,
    Styles,
    HomeIcon,
    GraphIcon,
    LogFileIcon,
    SettingsIcon,
    UploadIcon,
    BinaryIcon,
    ToolIcon,
    _LastFile
};

const char* Files[] =
{
    "Logo.png",
    "styles.css",
    "Home.svg",
    "Graph.svg",
    "LogFile.svg",
    "Settings.svg",
    "Upload.svg",
    "Binary.svg",
    "Tool.svg"
};

#ifdef USE_RGB_LED
RGBLED BuiltinLED(LED_BUILTIN);
#else
SimpleLED BuiltinLED(LED_BUILTIN);
#endif

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(FTP_TIMEOUT_MS);
StringBuilder HttpResponse(8 * 1024); // 8 kB HTTP response buffer (we use chunked responses)
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles]);
StringLog EventLog(MAX_EVENT_LOG_SIZE, 96);
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Navigation Nav;
CC1101 Radio(HSPI, CC1101_SCK_PIN, CC1101_MISO_PIN, CC1101_MOSI_PIN, CC1101_CSN_PIN, CC1101_GDO2_PIN, CC1101_GDO0_PIN);
RAMSES2 RAMSES(Radio, Serial1, BuiltinLED, WiFiSM);
Log<const RAMSES2Packet> PacketLog(RAMSES_PACKET_LOG_SIZE);
PacketStatsClass PacketStats;
EvoHomeInfo EvoHome;
RAMSES2Packet PacketToSend;

uint8_t testFrameBuffer[RAMSES_MAX_FRAME_SIZE];
String sendResult;
size_t packetsReceived = 0;
size_t packetLogEntriesToSync = 0;

time_t currentTime = 0;
time_t lastPacketReceivedTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;


void onPacketReceived(const RAMSES2Packet* packetPtr)
{
    // Reset stats at midnight
    if ((currentTime / SECONDS_PER_DAY) > (lastPacketReceivedTime / SECONDS_PER_DAY))
    {
        PacketStats.resetRSSI();
        EvoHome.resetZoneStatistics(currentTime);
        WiFiSM.logEvent("Reset stats");
    }
    lastPacketReceivedTime = currentTime;

    packetsReceived++;
    packetPtr->print(Serial);
    PacketLog.add(packetPtr);
    PacketStats.processPacket(packetPtr);
    EvoHome.processPacket(packetPtr);

    if (PersistentData.ftpSyncPacketLog)
        packetLogEntriesToSync = std::min(packetLogEntriesToSync + 1, RAMSES_PACKET_LOG_SIZE);
    
    if (PersistentData.isFTPEnabled() && (syncFTPTime == 0))
    {
        if (packetLogEntriesToSync >= PersistentData.ftpSyncEntries)
            syncFTPTime = currentTime;
        else if (EvoHome.zoneDataLogEntriesToSync >= PersistentData.ftpSyncEntries)
            syncFTPTime = currentTime;
    }
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer("trySyncFTP");

    FTPClient.beginAsync(
        PersistentData.ftpServer,
        PersistentData.ftpUser,
        PersistentData.ftpPassword,
        FTP_DEFAULT_CONTROL_PORT,
        printTo);

    auto zoneDataLogWriter = [printTo](Print& output)
    {
        if (!EvoHome.writeZoneDataLogCsv(output) && (printTo != nullptr))
            printTo->println("Nothing to sync.");
    };

    String filename = PersistentData.hostName;
    filename += ".csv";
    FTPClient.appendAsync(filename, zoneDataLogWriter);

    if (PersistentData.ftpSyncPacketLog && (packetLogEntriesToSync != 0))
    {
        auto packetLogWriter = [](Print& output)
        {
            for (const RAMSES2Packet* packetPtr : PacketLog)
                packetPtr->print(output, "%F %T");
            packetLogEntriesToSync = 0;
        };

        filename = PersistentData.hostName;
        filename += "_Packets.log";
        FTPClient.appendAsync(filename, packetLogWriter);
    }

   if (printTo == nullptr) return true; // Run async

    // Run synchronously
    bool success = FTPClient.run();
    if (success) lastFTPSyncTime = currentTime;
    return success;
}


void onTimeServerSynced()
{
    currentTime = TimeServer.getCurrentTime();

    RAMSES.maxHeaderBitErrors = PersistentData.maxHeaderBitErrors;
    RAMSES.maxManchesterBitErrors = PersistentData.maxManchesterBitErrors;
    if (RAMSES.begin(true))
        WiFiSM.logEvent("RAMSES2 initialized");
}


void onWiFiInitialized()
{
    if (!WiFiSM.isConnected()) return;

    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime))
    {
        trySyncFTP(nullptr); // Start async FTP
        syncFTPTime = 0;
    }
    else
    {
        if (FTPClient.runAsync())
        {
            if (FTPClient.isAsyncSuccess())
            {
                WiFiSM.logEvent("FTP sync");
                lastFTPSyncTime = currentTime;
            }
            else
            {
                WiFiSM.logEvent("FTP sync failed: %s", FTPClient.getLastError());
                syncFTPTime = currentTime + FTP_RETRY_INTERVAL;
            }
            FTPClient.endAsync();
        }
    }
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer("handleHttpSyncFTPRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader("FTP Sync", Nav);

    Html.writePreStart();
    bool success = trySyncFTP(&HttpResponse); 
    Html.writePreEnd();

    if (success)
    {
        Html.writeParagraph("Success! Duration: %u ms", FTPClient.getDurationMs());
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph("Failed: %s", FTPClient.getLastError());

    Html.writeFooter();
}


void handleHttpEventLogRequest()
{
    Tracer tracer("handleHttpEventLogRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader("Event log", Nav);

    if (WiFiSM.shouldPerformAction("clear"))
    {
        EventLog.clear();
        WiFiSM.logEvent("Event log cleared.");
    }

    for (const char* event : EventLog)
        Html.writeDiv("%s", event);

    Html.writeActionLink("clear", "Clear event log", currentTime, ButtonClass);

    Html.writeFooter();
}


void handleHttpConfigFormRequest()
{
    Tracer tracer("handleHttpConfigFormRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader("Settings", Nav);

    Html.writeFormStart("/config", "grid");
    PersistentData.writeHtmlForm(Html);
    Html.writeSubmitButton("Save");
    Html.writeFormEnd();

    if (WiFiSM.shouldPerformAction("reset"))
    {
        Html.writeParagraph("Resetting...");
        WiFiSM.reset();
    }
    else
        Html.writeActionLink("reset", "Reset ESP", currentTime, ButtonClass);

    Html.writeFooter();
}


void handleHttpConfigFormPost()
{
    Tracer tracer("handleHttpConfigFormPost");

    PersistentData.parseHtmlFormData([](const String& id) -> String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    RAMSES.maxHeaderBitErrors = PersistentData.maxHeaderBitErrors;
    RAMSES.maxManchesterBitErrors = PersistentData.maxManchesterBitErrors;

    handleHttpConfigFormRequest();
}


void handleHttpZoneDataLogRequest()
{
    Tracer tracer("handleHttpZoneDataLogRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader("Zone Data Log", Nav);

    int currentPage = WebServer.hasArg("page") ? WebServer.arg("page").toInt() : 0;
    int totalPages = (EvoHome.zoneDataLog.count() > 0) ? ((EvoHome.zoneDataLog.count() - 1)/ PAGE_SIZE) + 1 : 1;
    Html.writePager(totalPages, currentPage);

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("Time", 0, 2);
    for (int i = 0; i < EvoHome.zoneCount; i++)
    {
        ZoneInfo* zoneInfoPtr = EvoHome.getZoneInfo(i);
        Html.writeHeaderCell(zoneInfoPtr->name, 4);
    }
    Html.writeHeaderCell("Boiler heat", 0, 2);
    Html.writeRowEnd();
    Html.writeRowStart();
    for (int i = 0; i < EvoHome.zoneCount; i++)
    {
        Html.writeHeaderCell("T<sub>set</sub>");
        Html.writeHeaderCell("T<sub>ovr</sub>");
        Html.writeHeaderCell("T<sub>act</sub>");
        Html.writeHeaderCell("Heat");
    }
    Html.writeRowEnd();

    int n = 0;
    for (auto i = EvoHome.zoneDataLog.at(currentPage * PAGE_SIZE); i != EvoHome.zoneDataLog.end(); ++i)
    {
        i->writeRow(Html, EvoHome.zoneCount);
        if (++n == PAGE_SIZE) break;
    }

    Html.writeTableEnd();
    Html.writeFooter();
}


void handleHttpPacketStatsRequest()
{
    Tracer tracer("handleHttpPacketStatsRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader("Packet statistics", Nav);

    if (WiFiSM.shouldPerformAction("clear"))
    {
        PacketStats.clear();
        Html.writeParagraph("Packet statistics cleared.");
    }

    PacketStats.writeHtmlTable(Html);

    Html.writeActionLink("clear", "Clear statistics", currentTime, ButtonClass);
    Html.writeFooter();
}


void handleHttpPacketLogRequest()
{
    Tracer tracer("handleHttpPacketLogRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    Html.writeHeader("Packet Log", Nav);

    Html.writePreStart();

    for (const RAMSES2Packet* packetPtr : PacketLog)
        packetPtr->print(HttpResponse, "%T");

    Html.writePreEnd();
    Html.writeFooter();
}


void handleHttpPacketLogJsonRequest()
{
    Tracer tracer("handleHttpPacketLogJsonRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeJson);

    HttpResponse.clear();
    HttpResponse.print("[ ");

    bool first = true;
    for (const RAMSES2Packet* packetPtr : PacketLog)
    {
        if (first) first = false;
        else HttpResponse.print(", ");

        packetPtr->printJson(HttpResponse);
    }
    HttpResponse.println(" ]");
}


void handleHttpZoneInfoJsonRequest()
{
    Tracer tracer("handleHttpZoneInfoJsonRequest");

    HttpResponse.clear();
    EvoHome.writeZoneInfoJson(HttpResponse);

    WebServer.send(200, ContentTypeJson, HttpResponse.c_str());
}


void hexDump(Print& output, const uint8_t* dataPtr, size_t size)
{
    for (int i = 0; i < size; i++)
    {
        output.printf("%02X ", dataPtr[i]);
        if (i % 16 == 15) output.println();
    }
}


void handleHttpSendPacketRequest()
{
    Tracer tracer("handleHttpSendPacketRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);

    if (PacketToSend.payloadPtr == nullptr)
        PacketToSend.payloadPtr = PacketToSend.createPayload();

    Html.writeHeader("Send packet", Nav);

    Html.writeFormStart("/send", "grid");
    Html.writeDropdown("type", "type", RAMSES2Packet::typeId, 4, static_cast<int>(PacketToSend.type));

    StringBuilder addrStr(16);
    for (int i = 0; i < 3; i++)
    {
        String name = "addr";
        name += i;
        addrStr.clear();
        if (!PacketToSend.addr[i].isNull())
            PacketToSend.addr[i].print(addrStr, true);
        Html.writeTextBox(name, name, addrStr.c_str(), 10);
    }

    Html.writeTextBox("opcode", "opcode", String(static_cast<uint16_t>(PacketToSend.opcode), 16), 4);

    static StringBuilder payloadStr(RAMSES_MAX_PAYLOAD_SIZE * 3 + 1);
    payloadStr.clear();
    hexDump(payloadStr, PacketToSend.payloadPtr->bytes, PacketToSend.payloadPtr->size);
    Html.writeTextBox("payload", "payload", payloadStr.c_str(), RAMSES_MAX_PAYLOAD_SIZE * 3);

    Html.writeSubmitButton("Send");
    Html.writeFormEnd();

    static uint8_t packetBuffer[RAMSES_MAX_PACKET_SIZE];
    size_t packetSize = PacketToSend.serialize(packetBuffer, sizeof(packetBuffer));

    Html.writeHeading("Packet data", 2);
    Html.writePreStart();
    hexDump(HttpResponse, packetBuffer, packetSize);
    Html.writePreEnd();

    size_t frameSize = RAMSES.createFrame(PacketToSend, testFrameBuffer);

    Html.writeHeading("Frame data", 2);
    Html.writePreStart();
    hexDump(HttpResponse, testFrameBuffer, frameSize);
    Html.writePreEnd();

    Html.writeParagraph(sendResult);
    sendResult.clear();

    Html.writeFooter();
}


void handleHttpSendPacketPost()
{
    Tracer tracer("handleHttpSendPacketPost");

    PacketToSend.type = static_cast<RAMSES2PackageType>(WebServer.arg("type").toInt());

    for (int i = 0; i < 3; i++)
    {
        String name = "addr";
        name += i;
        String addrString = WebServer.arg(name); 
        if (addrString.length() == 0)
            PacketToSend.addr[i].setNull();
        else if (!PacketToSend.addr[i].parse(addrString))
            TRACE("Parsing addr%d '%s' failed\n", i, addrString.c_str());
    }

    int opcode = 0;
    if (sscanf(WebServer.arg("opcode").c_str(), "%04X", &opcode) == 1)
        PacketToSend.opcode = static_cast<RAMSES2Opcode>(opcode);
    else
        TRACE("Parsing opcode failed\n");

    if (!PacketToSend.payloadPtr->parse(WebServer.arg("payload")))
        TRACE("Parsing payload failed\n");

    if (RAMSES.sendPacket(PacketToSend))
        sendResult = "Success";
    else
        sendResult = "Failed";

    handleHttpSendPacketRequest();
}


const char* asBinary(uint8_t data)
{
    static char result[9];
    uint8_t bit = 0x80;
    for (int i = 0; i < 8; i++)
    {
        result[i] = (data & bit) ? '1' : '0';
        bit >>= 1;
    }
    result[8] = 0;
    return result;
}


void handleHttpFrameErrorsRequest()
{
    Tracer tracer("handleHttpFrameErrorsRequest");
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);
    RAMSES2ErrorInfo& errors = RAMSES.errors;

    Html.writeHeader("Frame Errors", Nav);

    Html.writeHeading("Errors by type", 2);
    Html.writeTableStart();
    Html.writeRow("Total errors", "%u", errors.getTotal());
    Html.writeRow("Frame too short", "%u", errors.frameTooShort);
    Html.writeRow("Frame too long", "%u", errors.frameTooLong);
    Html.writeRow("Manchester code", "%u", errors.invalidManchesterCode);
    Html.writeRow("Checksum", "%u", errors.invalidChecksum);
    Html.writeRow("Deserialization", "%u", errors.deserializationFailed);
    Html.writeTableEnd();

    Html.writeHeading("Manchester encoding errors", 2);
    Html.writeTableStart();
    Html.writeRow("Ignored", "%u", errors.ignoredManchesterCode);
    Html.writeRow("Repaired", "%u", errors.repairedManchesterCode);
    Html.writeRow("Failed", "%u", errors.invalidManchesterCode);
    Html.writeTableEnd();

    Html.writeParagraph(
        "Last manchester error: %u bit errors at %s",
        errors.lastManchesterBitErrors,
        formatTime("%T", errors.lastManchesterErrorTimestamp));
    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("Position");
    Html.writeHeaderCell("Error bits");
    Html.writeRowEnd();
    for (const ManchesterErrorInfo& errInfo : errors.manchesterErrors)
    {
        Html.writeRowStart();
        Html.writeCell(errInfo.packetIndex + 1);
        Html.writeCell(asBinary(errInfo.errorBits));
        Html.writeRowEnd();        
    }
    Html.writeTableEnd();

    Html.writeHeading("Last error packet", 2);
    Html.writeParagraph(
        "Last error packet (fragment) received at %s",
        formatTime("%T", errors.lastErrorPacketTimestamp));
    Html.writePreStart();
    hexDump(HttpResponse, errors.lastErrorPacket, errors.lastErrorPacketSize);
    HttpResponse.println();
    if (errors.lastManchesterErrorTimestamp == errors.lastErrorPacketTimestamp)
    {
        int packetIndex = 0;
        for (const ManchesterErrorInfo& errInfo : errors.manchesterErrors)
        {
            if (errInfo.packetIndex >= 16) break;
            for (int i = packetIndex; i < errInfo.packetIndex; i++) HttpResponse.print("   ");
            HttpResponse.print("^^ ");
            packetIndex = errInfo.packetIndex + 1;
        }
        HttpResponse.println();
    }
    RAMSES2Packet packet;
    if (packet.deserialize(errors.lastErrorPacket, errors.lastErrorPacketSize))
        packet.print(HttpResponse);
    else
        HttpResponse.println("[Unable to deserialize packet]");
    Html.writePreEnd();

    Html.writeHeading("Header mismatches", 2);
    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("Position");
    Html.writeHeaderCell("Count");
    Html.writeHeaderCell("Avg bit errors");
    Html.writeHeaderCell("Last value");
    Html.writeHeaderCell("Last error bits");
    Html.writeRowEnd();
    for (int i = 0; i < 3; i++)
    {
        const HeaderMismatchInfo& headerMismatchInfo = errors.headerMismatchInfo[i];
        Html.writeRowStart();
        Html.writeCell(i + 1);
        Html.writeCell(headerMismatchInfo.count);
        Html.writeCell(headerMismatchInfo.getAvgBitErrors(), F("%0.1f"));
        Html.writeCell(asBinary(headerMismatchInfo.lastValue));
        Html.writeCell(asBinary(headerMismatchInfo.lastErrorBits));
        Html.writeRowEnd();
    }
    Html.writeTableEnd();

    Html.writeFooter();
}


void handleHttpRootRequest()
{
    Tracer tracer("handleHttpRootRequest");

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }
    
    ChunkedResponse response(HttpResponse, WebServer, ContentTypeHtml);
    Html.writeHeader("Home", Nav);

    String ftpSync;
    if (!PersistentData.isFTPEnabled())
        ftpSync = "Disabled";
    else if (lastFTPSyncTime == 0)
        ftpSync = "Not yet";
    else
        ftpSync = formatTime("%H:%M", lastFTPSyncTime);

    Html.writeDivStart("flex-container");

    Html.writeSectionStart("Status");
    Html.writeTableStart();
    Html.writeRow("WiFi RSSI", "%d dBm", static_cast<int>(WiFi.RSSI()));
    Html.writeRow("Free Heap", "%0.1f kB", float(ESP.getFreeHeap()) / 1024);
    Html.writeRow("Uptime", "%0.1f days", float(WiFiSM.getUptime()) / SECONDS_PER_DAY);
    Html.writeRow("Last packet", "%s", formatTime("%T", lastPacketReceivedTime));
    Html.writeRow("Received", "%u", packetsReceived);
    uint32_t totalErrors = RAMSES.errors.getTotal();
    float errorPercentage = (packetsReceived + totalErrors) == 0 ? 0 : float(totalErrors) * 100 / (packetsReceived + totalErrors);
    Html.writeRow("Errors", "%0.1f %%", errorPercentage);
    Html.writeRow("FTP Sync", ftpSync);
    Html.writeRow(
        "Sync Entries", "%d / %d",
        std::max(packetLogEntriesToSync, EvoHome.zoneDataLogEntriesToSync),
        PersistentData.ftpSyncEntries);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart("Current values");
    EvoHome.writeCurrentValues(Html);
    Html.writeSectionEnd();

    Html.writeSectionStart("Devices");
    EvoHome.writeDeviceInfo(Html);
    Html.writeSectionEnd();

    Html.writeSectionStart("Zone statistics");
    EvoHome.writeZoneStatistics(Html);
    Html.writeSectionEnd();

    Html.writeDivEnd();
    Html.writeFooter();
}


void handleSerialRequest()
{
    Tracer tracer("handleSerialRequest");
    Serial.setTimeout(100);

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    Serial.println(cmd);

    if (cmd.startsWith("testP"))
    {
        RAMSES.switchToIdle();
        Tracer::traceFreeHeap();
        for (int i = 0; i < 100; i++)
        {
            RAMSES2Packet* testPacketPtr = new RAMSES2Packet();
            testPacketPtr->type = static_cast<RAMSES2PackageType>(i % 4);
            testPacketPtr->param[0] = i + 1;
            testPacketPtr->addr[0].deviceType = static_cast<RAMSES2DeviceType>(i % 8);
            testPacketPtr->addr[0].deviceId = (i % 8) * 4;
            if (testPacketPtr->type == RAMSES2PackageType::Request) 
            {
                testPacketPtr->addr[1].deviceType = static_cast<RAMSES2DeviceType>(i % 8);
                testPacketPtr->addr[1].deviceId = (i % 8) << 8;
            }
            else
            {
                testPacketPtr->addr[2].deviceType = static_cast<RAMSES2DeviceType>(i % 8);
                testPacketPtr->addr[2].deviceId = (i % 8) << 8;
            }
            if (testPacketPtr->type == RAMSES2PackageType::Info)
            {
                if (i & 8)
                {
                    testPacketPtr->opcode = RAMSES2Opcode::ZoneHeatDemand;
                    testPacketPtr->payloadPtr = testPacketPtr->createPayload();
                    testPacketPtr->payloadPtr->size = 2;
                    testPacketPtr->payloadPtr->bytes[0] = (i >> 2) % 4;
                    testPacketPtr->payloadPtr->bytes[1] = i % 100;
                }
                else
                {
                    testPacketPtr->opcode = RAMSES2Opcode::ZoneSetpoint;
                    testPacketPtr->payloadPtr = testPacketPtr->createPayload();
                    testPacketPtr->payloadPtr->size = 3;
                    testPacketPtr->payloadPtr->bytes[0] = (i >> 2) % 4;
                    testPacketPtr->payloadPtr->bytes[1] = i % 100;
                    testPacketPtr->payloadPtr->bytes[2] = 0;
                }
            }
            else
            {
                // Arbitrary test data
                testPacketPtr->opcode = static_cast<RAMSES2Opcode>((i % 16) << 4);
                RAMSES2Payload* testPayloadPtr = new RAMSES2Payload(); 
                testPayloadPtr->size = 1 + i % 8;
                for (int i = 0; i < testPayloadPtr->size; i++) testPayloadPtr->bytes[i] = i << 2;
                testPacketPtr->payloadPtr = testPayloadPtr;
            }
            testPacketPtr->rssi = -i;
            testPacketPtr->timestamp = currentTime + i;

            onPacketReceived(testPacketPtr);
        }
        Tracer::traceFreeHeap();
    }
    else if (cmd.startsWith("testW"))
    {
        for (int i = 0; i < 10; i++)
        {
            handleHttpRootRequest();
            Tracer::traceFreeHeap();
        }
    }
    else if (cmd.startsWith("testC"))
    {
        EvoHome.writeZoneDataLogCsv(Serial);
    }
    else if (cmd.startsWith("testS"))
    {
        static uint8_t packetBuffer[RAMSES_MAX_PACKET_SIZE];
        size_t packetSize = PacketToSend.serialize(packetBuffer, sizeof(packetBuffer));

        RAMSES2Packet deserialized;
        deserialized.deserialize(packetBuffer, packetSize);
        deserialized.print(Serial);
    }
    else if (cmd.startsWith("testR"))
    {
        size_t frameSize = RAMSES.createFrame(PacketToSend, testFrameBuffer);

        // Simulate packet received
        RAMSES.resetFrame(true);
        RAMSES.dataReceived(testFrameBuffer, frameSize);
    }
    else if (cmd.startsWith("testM"))
    {
        size_t frameSize = RAMSES.createFrame(PacketToSend, testFrameBuffer);

        // Create manchester encoding single bit error
        testFrameBuffer[12] ^= 1;

        TRACE("Introduced manchester encoding error:\n");
        Tracer::hexDump(testFrameBuffer, frameSize);

        // Simulate packet received
        RAMSES.resetFrame(true);
        RAMSES.dataReceived(testFrameBuffer, frameSize);
    }
    if (cmd.startsWith("testU"))
    {
        for (int i = 0; i < 5000; i++)
            Serial1.write(0xF0);
    }
    else if (cmd.startsWith("testDevInfo"))
    {
        PacketToSend.type = RAMSES2PackageType::Request;
        PacketToSend.addr[0].parse("18:002858"); // Fake HGI (this device)
        PacketToSend.addr[1].parse("01:044473"); // EvoHome Controller
        PacketToSend.opcode = RAMSES2Opcode::DeviceInfo;
        PacketToSend.payloadPtr = PacketToSend.createPayload();
        PacketToSend.payloadPtr->size = 1;
        PacketToSend.payloadPtr->bytes[0] = 0;
        RAMSES.sendPacket(PacketToSend);
    }
    else if (cmd.startsWith("testZoneName"))
    {
        PacketToSend.type = RAMSES2PackageType::Request;
        PacketToSend.addr[0].parse("18:002858"); // Fake HGI (this device)
        PacketToSend.addr[1].parse("01:044473"); // EvoHome Controller
        PacketToSend.opcode = RAMSES2Opcode::ZoneName;
        PacketToSend.payloadPtr = PacketToSend.createPayload();
        PacketToSend.payloadPtr->size = 2;
        PacketToSend.payloadPtr->bytes[0] = 0;
        PacketToSend.payloadPtr->bytes[1] = 0;
        RAMSES.sendPacket(PacketToSend);
    }
}


void onWiFiUpdating()
{
    RAMSES.end();
}


// Boot code
void setup() 
{
    Serial.begin(DEBUG_BAUDRATE);
    Serial.println();

    #ifdef DEBUG_ESP_PORT
    DEBUG_ESP_PORT.setDebugOutput(true);
    Tracer::traceTo(DEBUG_ESP_PORT);
    Tracer::traceFreeHeap();
    #endif

    BuiltinLED.begin();

    PersistentData.begin();
    TimeServer.begin(PersistentData.ntpServer);
    Html.setTitlePrefix(PersistentData.hostName);
    
    Nav.menuItems =
    {
        MenuItem
        {
            .icon = Files[HomeIcon],
            .label = PSTR("Home"),
            .handler = handleHttpRootRequest            
        },
        MenuItem
        {
            .icon = Files[LogFileIcon],
            .label = PSTR("Event log"),
            .urlPath =PSTR("events"),
            .handler = handleHttpEventLogRequest
        },
        MenuItem
        {
            .icon = Files[GraphIcon],
            .label = PSTR("Zone data log"),
            .urlPath =PSTR("zonelog"),
            .handler = handleHttpZoneDataLogRequest
        },
        MenuItem
        {
            .icon = Files[UploadIcon],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler= handleHttpSyncFTPRequest
        },
        MenuItem
        {
            .icon = Files[BinaryIcon],
            .label = PSTR("Packet log"),
            .urlPath =PSTR("packets"),
            .handler = handleHttpPacketLogRequest
        },
        MenuItem
        {
            .icon = Files[BinaryIcon],
            .label = PSTR("Packet stats"),
            .urlPath =PSTR("packetstats"),
            .handler = handleHttpPacketStatsRequest
        },
        MenuItem
        {
            .icon = Files[BinaryIcon],
            .label = PSTR("Frame errors"),
            .urlPath =PSTR("errors"),
            .handler = handleHttpFrameErrorsRequest
        },
        MenuItem
        {
            .icon = Files[ToolIcon],
            .label = PSTR("Send packet"),
            .urlPath =PSTR("send"),
            .handler = handleHttpSendPacketRequest,
            .postHandler = handleHttpSendPacketPost
        },
        MenuItem
        {
            .icon = Files[SettingsIcon],
            .label = PSTR("Settings"),
            .urlPath =PSTR("config"),
            .handler = handleHttpConfigFormRequest,
            .postHandler = handleHttpConfigFormPost
        },
    };
    Nav.registerHttpHandlers(WebServer);

    WebServer.on("/packets/json", handleHttpPacketLogJsonRequest);
    WebServer.on("/json", handleHttpZoneInfoJsonRequest);

    WiFiSM.registerStaticFiles(Files, _LastFile);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
    WiFiSM.on(WiFiInitState::Updating, onWiFiUpdating);
    WiFiSM.scanAccessPoints();
    WiFiSM.begin(PersistentData.wifiSSID, PersistentData.wifiKey, PersistentData.hostName);

    RAMSES.onPacketReceived(onPacketReceived);

    Tracer::traceFreeHeap();

    BuiltinLED.setOff();    
}


// Called repeatedly
void loop() 
{
    currentTime = WiFiSM.getCurrentTime();

    if (Serial.available())
    {
        BuiltinLED.setOn(true);
        handleSerialRequest();
        BuiltinLED.setOff();
    }

    WiFiSM.run();
}
