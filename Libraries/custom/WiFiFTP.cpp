#include "WiFiFTP.h"
#include <Tracer.h>
#include <ESPWifi.h>


WiFiFTPClient::WiFiFTPClient(int timeoutMs)
{
    _timeoutMs = timeoutMs;
#if (ESP_ARDUINO_VERSION_MAJOR > 2)
    _controlClient.setConnectionTimeout(timeoutMs);
    _dataClient.setConnectionTimeout(timeoutMs);
#endif
    _controlClient.setTimeout(timeoutMs);
    _dataClient.setTimeout(timeoutMs);

    _responseBuffer[0] = 0;
    _lastError[0] = 0;
}


bool WiFiFTPClient::begin(const char* host, const char* userName, const char* password, uint16_t port, Print* printTo)
{
    Tracer Tracer(F("WiFiFTPClient::begin"), host);

    _printPtr = printTo;
    _startMillis = millis();
    _durationMs = 0;

    if (!_controlClient.connect(host, port))
    {
        setLastError(F("Cannot connect to %s:%d"), host, port);
        return false;
    }
    _host = host;

    bool success = initialize(userName, password);
    if (!success)
    {
        TRACE(F("Unable to initialize FTP server\n"));
        end();
    }

    return success;
}


void WiFiFTPClient::end()
{
    Tracer Tracer(F("WiFiFTPClient::end"));

    if (_dataClient.connected())
        _dataClient.stop();

    if (_controlClient.connected())
    {
        sendCommand(F("QUIT"), nullptr, false);
        // We want to read (and print) the FTP server response for QUIT,
        // but we don't want to return it in getLastResponse(), so we use a separate response buffer.
        char responseBuffer[16];
        readServerResponse(responseBuffer, sizeof(responseBuffer));
        _controlClient.stop();
    }

    _durationMs = millis() - _startMillis;
    TRACE(F("Duration: %u ms\n"), _durationMs);

    _printPtr = nullptr;
}


void WiFiFTPClient::setLastError(String format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(_lastError, sizeof(_lastError) - 1, format.c_str(), args);
    _lastError[length] = 0;
    va_end(args);

    TRACE("ERROR: %s\n", _lastError);

    if (_asyncState != AsyncFTPState::Idle) setAsyncState(AsyncFTPState::Error);
}


void WiFiFTPClient::setUnexpectedResponse(const char* response)
{
    if (response == nullptr)
        response = _responseBuffer;

    if (response[0] == 0)
        setLastError(F("Timeout for %s"), _lastCommand.c_str());
    else
        setLastError(F("%s => %s"), _lastCommand.c_str(), response);
}


bool WiFiFTPClient::initialize(const char* userName, const char* password)
{
    Tracer Tracer(F("WiFiFTPClient::initialize"), userName);

    // Retrieve server welcome message
    _lastCommand = F("connect");
    int responseCode = readServerResponse();
    bool success = (responseCode >= 200) && (responseCode < 300);
    if (!success)
    {
        setUnexpectedResponse();
        return false;
    }

    responseCode = sendCommand(F("USER"), userName);
    if (responseCode == 331)
    {
        // User name OK, password required.
        responseCode = sendCommand(F("PASS"), password);
    }

    if (responseCode != 230)
    {
        setUnexpectedResponse();
        return false;
    }

    return passive();
}


bool WiFiFTPClient::passive()
{
    Tracer tracer(F("WiFiFTPClient::passive"));

    int responseCode = sendCommand(F("PASV"));
    if (responseCode != 227)
    {
        setUnexpectedResponse();
        return false;
    }

    return parsePassiveResult();   
}


bool WiFiFTPClient::parsePassiveResult()
{
    // Parse server data port
    int params[6];
    strtok(_responseBuffer, "(");
    for (int i = 0; i < 6; i++) 
    {
        const char* token = strtok(nullptr, ",)");
        if (token == nullptr)
        {
            setLastError(F("Unable to parse PASV response"));
            return false;
        }
        params[i] = atoi(token);
    }   
    _serverDataPort = (params[4] << 8) + params[5];
    TRACE(F("Server data port: %d\n"), _serverDataPort);
    return true;
}


int WiFiFTPClient::sendCommand(String cmd, const char* arg, bool awaitResponse)
{
    Tracer Tracer(F("WiFiFTPClient::sendCommand"), cmd.c_str());

    _lastCommand = cmd;
    if (arg != nullptr)
    {
        cmd += " ";
        cmd += arg;
    }

    if (_printPtr != nullptr)
        _printPtr->println(cmd);

    _controlClient.println(cmd);

    return awaitResponse ? readServerResponse() : 0;
}


int WiFiFTPClient::readServerResponse(char* responseBuffer, size_t responseBufferSize)
{
    Tracer tracer(F("WiFiFTPClient::readServerResponse"));

    if (responseBuffer == nullptr)
    {
        responseBuffer = _responseBuffer;
        responseBufferSize = sizeof(_responseBuffer);
    }

    // Work-around for hangs on ESP32S2 (?)
    int waitedMs = 0;
    while (!_controlClient.available())
    {
        delay(10);
        waitedMs += 10;
        if (waitedMs >= _timeoutMs)
        {
            TRACE(F("Timeout\n"));
            responseBuffer[0] = 0;
            return FTP_ERROR_TIMEOUT;
        }
    }

    size_t bytesRead = _controlClient.readBytesUntil('\n', responseBuffer, responseBufferSize - 1);
    responseBuffer[bytesRead] = 0;
    TRACE(F("Response: %s\n"), responseBuffer);

    if (_printPtr != nullptr)
        _printPtr->print(responseBuffer);

    if (bytesRead == 0)
        return FTP_ERROR_TIMEOUT;

    int responseCode;
    if (sscanf(responseBuffer, "%d", &responseCode) != 1)
        return FTP_ERROR_BAD_RESPONSE;

    TRACE(F("Response code: %d\n"), responseCode);
    return responseCode;
}


WiFiClient& WiFiFTPClient::getDataClient()
{
    Tracer tracer(F("WiFiFTPClient::getDataClient"));

    if (!_dataClient.connect(_host, _serverDataPort))
    {
        setLastError(F("Cannot connect to %s:%d"), _host, _serverDataPort);
    }

    return _dataClient;
}


WiFiClient& WiFiFTPClient::store(String filename)
{
    Tracer tracer(F("WiFiFTPClient::store"), filename.c_str());

    sendCommand(F("STOR"), filename.c_str(), false);

    WiFiClient& dataClient = getDataClient();
    if (dataClient.connected())
    {
        if (readServerResponse() == 150)
            _lastCommand = F("upload");
        else
        {
            setUnexpectedResponse();
            dataClient.stop();
        }
    }

    return dataClient;
}


WiFiClient& WiFiFTPClient::append(String filename)
{
    Tracer tracer(F("WiFiFTPClient::append"), filename.c_str());

    sendCommand(F("APPE"), filename.c_str(), false);

    WiFiClient& dataClient = getDataClient();
    if (dataClient.connected())
    {
        if (readServerResponse() == 150)
            _lastCommand = F("upload");
        else
        {
            setUnexpectedResponse();
            dataClient.stop();
        }
    }

    return dataClient;
}


void WiFiFTPClient::setAsyncState(AsyncFTPState state)
{
    uint32_t currentMillis = millis();
    TRACE(F("WiFiFTPClient::setAsyncState(%d) +%u ms\n"), state, currentMillis - _asyncStateChangeMillis);
    _asyncStateChangeMillis = currentMillis;
    _asyncState = state;
}


void WiFiFTPClient::beginAsync(const char* host, const char* userName, const char* password, uint16_t port, Print* printTo)
{
    Tracer tracer(F("WiFiFTPClient::beginAsync"), host);

    _host = host;
    _userName = userName;
    _password = password;
    _port = port;
    _printPtr = printTo;

    _lastCommand.clear();
    _asyncCommands.clear();
    _startMillis = millis();
    _durationMs = 0;
    _asyncState = AsyncFTPState::Idle;
    _asyncStateChangeMillis = _startMillis;
}


void WiFiFTPClient::appendAsync(String filename, std::function<void(Print&)> dataWriter)
{
    Tracer tracer(F("WiFiFTPClient::appendAsync"), filename.c_str());

    AsyncFTPCommand asyncCommand
    {
        .arg = filename,
        .execute = nullptr,
        .dataWriter = dataWriter
    };
    asyncCommand.execute = std::bind(&WiFiFTPClient::append, this, asyncCommand.arg),
    _asyncCommands.push_back(asyncCommand);

    if (_asyncState == AsyncFTPState::Idle) setAsyncState(AsyncFTPState::Connect);
}


bool WiFiFTPClient::run()
{
    Tracer tracer(F("WiFiFTPClient::run"));

    while (!runAsync()) delay(10);
    
    bool success = _asyncState == AsyncFTPState::Done;
    endAsync();

    return success;
}


bool WiFiFTPClient::runAsync()
{
    int responseCode;

    switch (_asyncState)
    {
        case AsyncFTPState::Idle:
        case AsyncFTPState::Done:
        case AsyncFTPState::Error:
            break; // Nothing to do

        case AsyncFTPState::Connect:
            if (_controlClient.connect(_host, _port))
                setAsyncState(AsyncFTPState::Welcome);
            else
                setLastError(F("Cannot connect to %s:%d"), _host, _port);
            break;

        case AsyncFTPState::Welcome:
            // Retrieve server welcome message
            responseCode = readServerResponse();
            if ((responseCode >= 200) && (responseCode < 300))
            {
                sendCommand(F("USER"), _userName, false);
                setAsyncState(AsyncFTPState::User);
            }
            else
                setUnexpectedResponse();
            break;

        case AsyncFTPState::User:
            responseCode = readServerResponse();
            if (responseCode == 230)
            {
                // No password required.
                sendCommand(F("PASV"), nullptr, false);
                setAsyncState(AsyncFTPState::Passive);
            }
            else if (responseCode == 331)
            {
                // User name OK, password required.
                sendCommand(F("PASS"), _password, false);
                setAsyncState(AsyncFTPState::Password);
            }
            else
                setUnexpectedResponse();
            break;

        case AsyncFTPState::Password:
            responseCode = readServerResponse();
            if (responseCode == 230)
            {
                sendCommand(F("PASV"), nullptr, false);
                setAsyncState(AsyncFTPState::Passive);
            }
            else
                setUnexpectedResponse();
            break;

        case AsyncFTPState::Passive:
            responseCode = readServerResponse();
            if (responseCode == 227)
            {
                if (parsePassiveResult())
                {
                    if (_asyncCommands.size() == 0)
                        setAsyncState(AsyncFTPState::End);
                    else   
                        setAsyncState(AsyncFTPState::ExecCommand);
                }
            }
            else
                setUnexpectedResponse();
            break;

        case AsyncFTPState::ExecCommand:
        {
            AsyncFTPCommand& asyncCommand = _asyncCommands.front();
            WiFiClient& dataClient = asyncCommand.execute();
            if (dataClient.connected())
            {
                asyncCommand.dataWriter(dataClient);
                dataClient.stop();
                setAsyncState(AsyncFTPState::FinishCommand);
            }
            else
                setAsyncState(AsyncFTPState::Error);
            _asyncCommands.pop_front();
            break;
        }

        case AsyncFTPState::FinishCommand:
            responseCode = readServerResponse();
            if (responseCode == 226)
            {
                if (_asyncCommands.size() == 0)
                    setAsyncState(AsyncFTPState::End);
                else
                {
                    sendCommand(F("PASV"), nullptr, false);
                    setAsyncState(AsyncFTPState::Passive);
                }                        
            }
            else
                setUnexpectedResponse();
            break;

        case AsyncFTPState::End:
            end();
            setAsyncState(AsyncFTPState::Done);
            break;
    }

    return _asyncState >= AsyncFTPState::Done;
}
