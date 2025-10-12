#ifndef REST_CLIENT_H
#define REST_CLIENT_H

#include <ArduinoJson.h>

#ifdef ESP8266
#include <AsyncHTTPRequest_Generic.hpp>
#else
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#endif

constexpr int HTTP_REQUEST_PENDING = 0;
constexpr int HTTP_OK = 200;
constexpr int HTTP_OPEN_FAILED = -100;
constexpr int HTTP_SEND_FAILED = -101;
constexpr int RESPONSE_PARSING_FAILED = -102;

enum struct RequestMethod
{
    GET,
    POST,
    PUT,
    DELETE
};

class RESTClient
{
    public:
        bool isInitialized = false;

        // Constructor
        RESTClient(uint16_t timeout) { _timeout = timeout; }

        String getBaseUrl() { return _baseUrl; }
        String getLastError() { return _lastError; }
        bool isResponsePending() { return _requestMillis != 0; }
        bool isRequestPending() { return isResponsePending() && !isResponseAvailable(); }
        uint32_t getResponseTimeMs() { return _responseTimeMs; }

        void setBearerToken(const String& bearerToken);
        virtual int requestData(const String& urlSuffix = "");
        int awaitData(const String& urlSuffix = "");

    protected:
        JsonDocument _filterDoc;

        bool begin(const String& baseUrl, const char* certificate = nullptr);
        void addHeader(const String& name, const String& value);
        virtual bool parseResponse(const JsonDocument& response) = 0;
        void setLastError(const String& message) { _lastError = message; }
        int request(RequestMethod method, const String& urlSuffix, const String& payload, String& response);

    private:
#ifdef ESP8266    
        AsyncHTTPRequest _asyncHttpRequest;
#else
        TaskHandle_t _taskHandle;
        NetworkClientSecure _tlsClient;
        HTTPClient _httpClient;
        volatile int _httpResult;

        void runHttpRequests();
        inline static void run(void* taskParam)
        {
            RESTClient* instancePtr = static_cast<RESTClient*>(taskParam);
            instancePtr->runHttpRequests();
        }
#endif
        const char* _certificate = nullptr;
        String _baseUrl;
        String _bearerToken;
        String _lastError;
        uint16_t _timeout;
        volatile uint32_t _requestMillis = 0;
        uint32_t _responseTimeMs = 0;
        String _response;

        int startRequest(const String& url);
        bool isResponseAvailable();
        int getResponse();
};

#endif