#ifndef WIFIFTP_H
#define WIFIFTP_H

#include <deque>
#include <stdint.h>
#include <WiFiClient.h>
#include <Print.h>

constexpr uint16_t FTP_DEFAULT_CONTROL_PORT = 21;
constexpr uint16_t FTP_DEFAULT_DATA_PORT = 22;

#define FTP_ERROR_TIMEOUT -1
#define FTP_ERROR_BAD_RESPONSE -2
#define FTP_ERROR_COMMAND_TOO_LONG -3

enum struct AsyncFTPState
{
    Idle = 0,
    Connect = 1,
    Welcome = 2,
    User = 3,
    Password = 4,
    Passive = 5,
    ExecCommand = 6,
    FinishCommand = 7,
    End = 8,
    Done = 9,
    Error = 10
};

struct AsyncFTPCommand
{
    String arg;
    std::function<WiFiClient&()> execute;
    std::function<void(Print&)> dataWriter;
};

class WiFiFTPClient
{
    public:
        WiFiFTPClient(int timeoutMs);

        bool begin(const char* host, const char* userName, const char* password, uint16_t port = FTP_DEFAULT_CONTROL_PORT, Print* printTo = nullptr);
        void end();
        uint32_t getDurationMs() { return _durationMs; }

        bool passive();
        int sendCommand(String cmd, const char* arg = nullptr, bool awaitResponse = true);
        int readServerResponse(char* responseBuffer = nullptr, size_t responseBufferSize = 0);
        WiFiClient& getDataClient();

        const char* getLastError() { return _lastError; }

        WiFiClient& store(String filename);
        WiFiClient& append(String filename);

        // Support for async FTP:
        AsyncFTPState getAsyncState() { return _asyncState; }
        bool isAsyncPending() { return (_asyncState != AsyncFTPState::Idle) && (_asyncState < AsyncFTPState::Done); }
        bool isAsyncSuccess() { return _asyncState == AsyncFTPState::Done; }
        void beginAsync(const char* host, const char* userName, const char* password, uint16_t port = FTP_DEFAULT_CONTROL_PORT, Print* printTo = nullptr);
        void endAsync() { setAsyncState(AsyncFTPState::Idle); }
        void appendAsync(String filename, std::function<void(Print&)> dataWriter);
        bool runAsync();
        bool run();

        void setUnexpectedResponse(const char* response = nullptr);

    private:
        int _timeoutMs;
        WiFiClient _controlClient;
        WiFiClient _dataClient;
        String _lastCommand;
        char _responseBuffer[128];
        uint16_t _serverDataPort;
        uint16_t _port;
        const char* _host;
        const char* _userName;
        const char* _password;
        Print* _printPtr;
        char _lastError[128];
        uint32_t _durationMs;
        uint32_t _startMillis;
        uint32_t _asyncStateChangeMillis;
        volatile AsyncFTPState _asyncState;
        std::deque<AsyncFTPCommand> _asyncCommands;

        bool initialize(const char* userName, const char* password);
        bool parsePassiveResult();
        void setAsyncState(AsyncFTPState state);
        void setLastError(String format, ...);
};

#endif