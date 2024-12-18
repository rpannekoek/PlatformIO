#ifndef REST_CLIENT_H
#define REST_CLIENT_H

#include <ArduinoJson.h>

#ifdef ESP8266
#include <AsyncHTTPRequest_Generic.hpp>
#else
#include <HTTPClient.h>
#endif

constexpr int HTTP_REQUEST_PENDING = 0;
constexpr int HTTP_OK = 200;
constexpr int HTTP_OPEN_FAILED = -100;
constexpr int HTTP_SEND_FAILED = -101;
constexpr int RESPONSE_PARSING_FAILED = -102;

class RESTClient
{
    public:
        bool isInitialized = false;

        // Constructor
        RESTClient(uint16_t timeout) { _timeout = timeout; }

        String getLastError() { return _lastError; }
        bool isRequestPending() { return _requestMillis != 0; }

        int requestData(const String& urlSuffix = "");

    protected:
        JsonDocument _filterDoc;

        bool begin(const String& baseUrl);
        virtual bool parseResponse(const JsonDocument& response) = 0;

    private:
#ifdef ESP8266    
        AsyncHTTPRequest _asyncHttpRequest;
#else
        TaskHandle_t _taskHandle;
        HTTPClient _httpClient;
        volatile int _httpResult;

        void runHttpRequests();
        inline static void run(void* taskParam)
        {
            RESTClient* instancePtr = static_cast<RESTClient*>(taskParam);
            instancePtr->runHttpRequests();
        }
#endif
        String _baseUrl;
        String _lastError;
        uint16_t _timeout;
        volatile uint32_t _requestMillis = 0;
        String _response;

        int sendRequest(const String& url);
        bool isResponseAvailable();
        int getResponse();
};

#endif