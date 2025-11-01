#ifndef ONECTA_CLIENT_H
#define ONECTA_CLIENT_H

#include <Logger.h>
#include <SpiRamAllocator.h>
#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <vector>
#include <functional>

class OnectaClient
{
    public:
        JsonDocument jsonDoc;
        std::vector<String> deviceIds;

        OnectaClient(const char* clientId, const char* clientSecret, char* refreshToken, ILogger& logger);

        uint32_t responseTimeMs() { return _responseTimeMs; }
        uint32_t rateLimitPerDay() { return _rateLimitPerDay; }
        uint32_t rateLimitRemaining() { return _rateLimitRemaining; }
        time_t requestAfter() { return _requestAfter; }
        void onTokenRefresh(std::function<void(void)> cb) { _refreshTokenCallback = cb; }

        bool discoverDevices();
        bool requestDeviceStatus(const String& deviceId, time_t time);
        void cleanup();

    private:
        const char* _clientId;
        const char* _clientSecret;
        char* _refreshToken;
        String _accessToken;
        uint32_t _tokenExpiresMillis = 0;
        time_t _requestAfter = 0;
        uint32_t _responseTimeMs;
        uint32_t _rateLimitPerDay;
        uint32_t _rateLimitRemaining;

        std::function<void(void)> _refreshTokenCallback;
        ILogger& _logger;
        NetworkClientSecure _tlsClient;
        JsonDocument _sitesFilterDoc;
        JsonDocument _deviceFilterDoc;
        SpiRamAllocator _spiRamAllocator;

        bool exchangeTokens();
        bool request(const String& urlPath, const JsonDocument& filterDoc);
};

#endif
