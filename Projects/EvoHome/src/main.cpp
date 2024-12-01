#include <Arduino.h>
#include <ESPCoreDump.h>
#include <ESPWiFi.h>
#include <ESPWebServer.h>
#include <ESPFileSystem.h>
#include <WiFiNTP.h>
#include <WiFiFTP.h>
#include <TimeUtils.h>
#include <Tracer.h>
#include <StringBuilder.h>
#include <LED.h>
#include <Log.h>
#include <WiFiStateMachine.h>
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

#ifdef USE_SIMPLE_LED
SimpleLED BuiltinLED(LED_BUILTIN);
#else
RGBLED BuiltinLED(LED_BUILTIN);
#endif

ESPWebServer WebServer(80); // Default HTTP port
WiFiNTP TimeServer;
WiFiFTPClient FTPClient(FTP_TIMEOUT_MS);
StringBuilder HttpResponse(12 * 1024);
HtmlWriter Html(HttpResponse, Files[Logo], Files[Styles], MAX_BAR_LENGTH);
Log<const char> EventLog(MAX_EVENT_LOG_SIZE);
WiFiStateMachine WiFiSM(BuiltinLED, TimeServer, WebServer, EventLog);
Navigation Nav;
CC1101 Radio(HSPI, CC1101_SCK_PIN, CC1101_MISO_PIN, CC1101_MOSI_PIN, CC1101_CSN_PIN, CC1101_GDO2_PIN);
RAMSES2 RAMSES(Radio, Serial1, BuiltinLED, WiFiSM);
Log<const RAMSES2Packet> PacketLog(RAMSES_PACKET_LOG_SIZE);
PacketStatsClass PacketStats;
EvoHomeInfo EvoHome;
RAMSES2Packet PacketToSend;

uint8_t testFrameBuffer[RAMSES_MAX_FRAME_SIZE];
String sendResult;
size_t packetsReceived = 0;
size_t logEntriesToSync = 0;

time_t currentTime = 0;
time_t syncFTPTime = 0;
time_t lastFTPSyncTime = 0;


void onPacketReceived(const RAMSES2Packet* packetPtr)
{
    packetsReceived++;
    packetPtr->print(Serial);
    PacketLog.add(packetPtr);
    PacketStats.processPacket(packetPtr);
    EvoHome.processPacket(packetPtr);

    logEntriesToSync = std::min(logEntriesToSync + 1, RAMSES_PACKET_LOG_SIZE);
    if (PersistentData.isFTPEnabled() && (logEntriesToSync >= PersistentData.ftpSyncEntries) & (syncFTPTime == 0))
        syncFTPTime = currentTime;
}


void printPacketLog(Print& output, const char* timeFormat = nullptr, const RAMSES2Packet* firstPacketPtr = nullptr)
{
    const RAMSES2Packet* packetPtr = firstPacketPtr;
    if (packetPtr == nullptr)
        packetPtr = PacketLog.getFirstEntry();

    while (packetPtr != nullptr)
    {
        packetPtr->print(output, timeFormat);
        packetPtr = PacketLog.getNextEntry();
    }
}


bool trySyncFTP(Print* printTo)
{
    Tracer tracer("trySyncFTP");

    if (!FTPClient.begin(
        PersistentData.ftpServer,
        PersistentData.ftpUser,
        PersistentData.ftpPassword,
        FTP_DEFAULT_CONTROL_PORT,
        printTo))
    {
        FTPClient.end();
        return false;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "%s.log", PersistentData.hostName);
    bool success = false;
    WiFiClient& dataClient = FTPClient.append(filename);
    if (dataClient.connected())
    {
        if (logEntriesToSync > 0)
        {
            const RAMSES2Packet* firstPacketPtr = PacketLog.getEntryFromEnd(logEntriesToSync);
            printPacketLog(dataClient, "%F %T", firstPacketPtr);
            logEntriesToSync = 0;
        }
        else if (printTo != nullptr)
            printTo->println("Nothing to sync.");
        dataClient.stop();

        if (FTPClient.readServerResponse() == 226)
        {
            lastFTPSyncTime = currentTime;
            success = true;
        }
        else
            FTPClient.setUnexpectedResponse();
    }

    FTPClient.end();
    return success;
}


void onTimeServerSynced()
{
    currentTime = TimeServer.getCurrentTime();

    if (RAMSES.begin(true))
        WiFiSM.logEvent("RAMSES2 initialized");
}


void onWiFiInitialized()
{
    if ((syncFTPTime != 0) && (currentTime >= syncFTPTime) && WiFiSM.isConnected())
    {
        if (trySyncFTP(nullptr))
        {
            WiFiSM.logEvent("FTP sync");
            syncFTPTime = 0;
        }
        else
        {
            WiFiSM.logEvent("FTP sync failed: %s", FTPClient.getLastError());
            syncFTPTime += FTP_RETRY_INTERVAL;
        }
    }
}


void handleHttpSyncFTPRequest()
{
    Tracer tracer("handleHttpSyncFTPRequest");

    Html.writeHeader("FTP Sync", Nav);

    Html.writePreStart();
    bool success = trySyncFTP(&HttpResponse); 
    Html.writePreEnd();

    if (success)
    {
        Html.writeParagraph("Success!");
        syncFTPTime = 0; // Cancel scheduled sync (if any)
    }
    else
        Html.writeParagraph("Failed: %s", FTPClient.getLastError());

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpEventLogRequest()
{
    Tracer tracer("handleHttpEventLogRequest");

    Html.writeHeader("Event log", Nav);

    if (WiFiSM.shouldPerformAction("clear"))
    {
        EventLog.clear();
        WiFiSM.logEvent("Event log cleared.");
    }

    const char* event = EventLog.getFirstEntry();
    while (event != nullptr)
    {
        Html.writeDiv("%s", event);
        event = EventLog.getNextEntry();
    }

    Html.writeActionLink("clear", "Clear event log", currentTime, ButtonClass);

    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormRequest()
{
    Tracer tracer("handleHttpConfigFormRequest");

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

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
}


void handleHttpConfigFormPost()
{
    Tracer tracer("handleHttpConfigFormPost");

    PersistentData.parseHtmlFormData([](const String& id) -> String { return WebServer.arg(id); });
    PersistentData.validate();
    PersistentData.writeToEEPROM();

    handleHttpConfigFormRequest();
}


void handleHttpZoneDataLogRequest()
{
    Tracer tracer("handleHttpZoneDataLogRequest");

    // Use chunked response
    WebServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    WebServer.send(200, ContentTypeHtml, "");

    Html.writeHeader("Zone Data Log", Nav);

    Html.writeTableStart();
    Html.writeRowStart();
    Html.writeHeaderCell("Time", 0, 2);
    for (int i = 0; i < EvoHome.zoneCount; i++)
    {
        ZoneInfo* zoneInfoPtr = EvoHome.getZoneInfo(i);
        Html.writeHeaderCell(zoneInfoPtr->name, 4);
    }
    Html.writeRowEnd();
    Html.writeRowStart();
    for (int i = 0; i < EvoHome.zoneCount; i++)
    {
        Html.writeHeaderCell("T<sub>set</sub>");
        Html.writeHeaderCell("T<sub>act</sub>");
        Html.writeHeaderCell("Heat");
        Html.writeHeaderCell("Battery");
    }
    Html.writeRowEnd();

    ZoneDataLogEntry* logEntryPtr = EvoHome.zoneDataLog.getFirstEntry();
    while (logEntryPtr != nullptr)
    {
        logEntryPtr->writeRow(Html, EvoHome.zoneCount);
        if (HttpResponse.length() >= HTTP_CHUNK_SIZE)
        {
            WebServer.sendContent(HttpResponse.c_str(), HttpResponse.length());
            HttpResponse.clear();
        }
        logEntryPtr = EvoHome.zoneDataLog.getNextEntry();
    }

    Html.writeTableEnd();
    Html.writeFooter();

    WebServer.sendContent(HttpResponse.c_str(), HttpResponse.length());
    WebServer.sendContent("");
}


void handleHttpPacketLogRequest()
{
    Tracer tracer("handleHttpPacketLogRequest");

    // Use chunked response
    WebServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    WebServer.send(200, ContentTypeHtml, "");

    Html.writeHeader("Packet Log", Nav);

    Html.writePreStart();

    const RAMSES2Packet* packetPtr = PacketLog.getFirstEntry();
    while (packetPtr != nullptr)
    {
        packetPtr->print(HttpResponse, "%T");
        if (HttpResponse.length() >= HTTP_CHUNK_SIZE)
        {
            WebServer.sendContent(HttpResponse.c_str(), HttpResponse.length());
            HttpResponse.clear();
        }
        packetPtr = PacketLog.getNextEntry();
    }

    Html.writePreEnd();
    Html.writeFooter();

    WebServer.sendContent(HttpResponse.c_str(), HttpResponse.length());
    WebServer.sendContent("");
}


void handleHttpPacketLogJsonRequest()
{
    Tracer tracer("handleHttpPacketLogJsonRequest");

    // Use chunked response
    WebServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    WebServer.send(200, ContentTypeJson, "");

    HttpResponse.clear();
    HttpResponse.print("[ ");

    const RAMSES2Packet* packetPtr = PacketLog.getFirstEntry();
    while (packetPtr != nullptr)
    {
        packetPtr->printJson(HttpResponse);
        packetPtr = PacketLog.getNextEntry();
        if (packetPtr != nullptr)
        {
            HttpResponse.print(", ");    
        }

        if (HttpResponse.length() >= HTTP_CHUNK_SIZE)
        {
            WebServer.sendContent(HttpResponse.c_str(), HttpResponse.length());
            HttpResponse.clear();
        }
    }
    HttpResponse.println(" ]");

    WebServer.sendContent(HttpResponse.c_str(), HttpResponse.length());
    WebServer.sendContent("");
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

    if (PacketToSend.payloadPtr == nullptr)
        PacketToSend.payloadPtr = PacketToSend.createPayload();

    Html.writeHeader("Send packet", Nav);

    Html.writeFormStart("/send", "grid");
    Html.writeDropdown("type", "type", RAMSES2Packet::typeId, 4, static_cast<int>(PacketToSend.type));

    for (int i = 0; i < 3; i++)
    {
        String name = "addr";
        name += i;
        StringBuilder addrStr(16);
        if (!PacketToSend.addr[i].isNull())
            PacketToSend.addr[i].print(addrStr);
        Html.writeTextBox(name, name, addrStr.c_str(), 10);
    }

    Html.writeTextBox("opcode", "opcode", String(PacketToSend.opcode, 16), 4);

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

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
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
        PacketToSend.opcode = opcode;
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


void handleHttpRootRequest()
{
    Tracer tracer("handleHttpRootRequest");

    if (WiFiSM.isInAccessPointMode())
    {
        handleHttpConfigFormRequest();
        return;
    }
    
    Html.writeHeader("Home", Nav);

    const char* ftpSync;
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
    Html.writeRow("Received", "%u", packetsReceived);
    Html.writeRow("Errors", "%d", RAMSES.errors);
    Html.writeRow("FTP Sync", ftpSync);
    Html.writeRow("Sync Entries", "%d / %d", logEntriesToSync, PersistentData.ftpSyncEntries);
    Html.writeTableEnd();
    Html.writeSectionEnd();

    Html.writeSectionStart("EvoHome Zones");
    EvoHome.writeCurrentValues(Html);
    Html.writeSectionEnd();

    Html.writeSectionStart("Packet Statistics");
    PacketStats.writeHtmlTable(Html);
    Html.writeSectionEnd();

    Html.writeDivEnd();
    Html.writeFooter();

    WebServer.send(200, ContentTypeHtml, HttpResponse.c_str());
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
        for (int i = 0; i < RAMSES_PACKET_LOG_SIZE; i++)
        {
            RAMSES2Packet* testPacketPtr = new RAMSES2Packet();
            testPacketPtr->type = static_cast<RAMSES2PackageType>(i % 4);
            testPacketPtr->param[0] = i + 1;
            testPacketPtr->addr[0].deviceType = i % 8;
            testPacketPtr->addr[0].deviceId = (i % 8) * 4;
            if (i % 2 == 1) 
            {
                testPacketPtr->addr[2].deviceType = i % 8;
                testPacketPtr->addr[2].deviceId = (i % 8) << 8;
            }
            testPacketPtr->opcode = (i % 20) << 3;
            if (testPacketPtr->opcode == 8)
            {
                HeatDemandPayload* testHeatDemandPayloadPtr = new HeatDemandPayload();
                testHeatDemandPayloadPtr->size = 2;
                testHeatDemandPayloadPtr->bytes[0] = i;
                testHeatDemandPayloadPtr->bytes[1] = i;
                testPacketPtr->payloadPtr = testHeatDemandPayloadPtr;
            }
            else
            {
                RAMSES2Payload* testPayloadPtr = new RAMSES2Payload(); 
                testPayloadPtr->size = 1 + i % 8;
                for (int i = 0; i < testPayloadPtr->size; i++) testPayloadPtr->bytes[i] = i << 2;
                testPacketPtr->payloadPtr = testPayloadPtr;
            }
            testPacketPtr->rssi = -i;
            testPacketPtr->timestamp = currentTime + i;

            onPacketReceived(testPacketPtr);
        }
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
        RAMSES.resetFrame();
        for (int i = 0; i < frameSize; i++)
            RAMSES.byteReceived(testFrameBuffer[i]);    
    }
    else if (cmd.startsWith("testDevInfo"))
    {
        PacketToSend.type = RAMSES2PackageType::Request;
        PacketToSend.addr[0].parse("18:002858"); // Fake HGI (this device)
        PacketToSend.addr[1].parse("01:044473"); // EvoHome Controller
        PacketToSend.opcode = 0x10E0;
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
        PacketToSend.opcode = 0x0004;
        PacketToSend.payloadPtr = PacketToSend.createPayload();
        PacketToSend.payloadPtr->size = 2;
        PacketToSend.payloadPtr->bytes[0] = 0;
        PacketToSend.payloadPtr->bytes[1] = 0;
        RAMSES.sendPacket(PacketToSend);
    }
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
    //EventLog.begin();

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
            .icon = Files[GraphIcon],
            .label = PSTR("Zone data log"),
            .urlPath =PSTR("zonelog"),
            .handler = handleHttpZoneDataLogRequest
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
            .icon = Files[ToolIcon],
            .label = PSTR("Send packet"),
            .urlPath =PSTR("send"),
            .handler = handleHttpSendPacketRequest,
            .postHandler = handleHttpSendPacketPost
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
            .icon = Files[UploadIcon],
            .label = PSTR("FTP Sync"),
            .urlPath = PSTR("sync"),
            .handler= handleHttpSyncFTPRequest
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

    WebServer.on("/json", handleHttpPacketLogJsonRequest);

    WiFiSM.registerStaticFiles(Files, _LastFile);
    WiFiSM.on(WiFiInitState::TimeServerSynced, onTimeServerSynced);
    WiFiSM.on(WiFiInitState::Initialized, onWiFiInitialized);
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
