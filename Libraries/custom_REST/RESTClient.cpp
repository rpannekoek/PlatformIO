#include <RESTClient.h>
#include <Tracer.h>


bool RESTClient::begin(const String& baseUrl, const char* certificate)
{
    _baseUrl = baseUrl;
    _certificate = certificate;
    _requestMillis = 0;

#ifdef ESP8266
    isInitialized = true;
#else
    uint32_t timeoutMs = static_cast<uint32_t>(_timeout) * 1000;
    _httpClient.setTimeout(timeoutMs);
    _httpClient.setConnectTimeout(timeoutMs);

    BaseType_t res = xTaskCreate(
        run,
        "RESTClient",
        4096, // Stack Size (words)
        this,
        tskIDLE_PRIORITY, // Minimal priority
        &_taskHandle);

    TRACE("RESTClient::begin: xTaskCreate returned %d\n", res);
    isInitialized = (res == pdPASS);
#endif

    return isInitialized;
}


void RESTClient::setBearerToken(const String& bearerToken)
{
    Tracer tracer("RESTClient::setBearerToken", bearerToken.c_str());

    _bearerToken = bearerToken;

    if (bearerToken.length() > 0)
    {
#ifdef ESP32        
        _httpClient.setAuthorizationType("Bearer");
        _httpClient.setAuthorization(bearerToken.c_str());
#endif
    }
}


void RESTClient::addHeader(const String& name, const String& value)
{
#ifdef ESP8266
    _asyncHttpRequest.setReqHeader(name.c_str(), value.c_str());
#else
    _httpClient.addHeader(name, value);
#endif
}


int RESTClient::requestData(const String& urlSuffix)
{
    if (_requestMillis == 0)
    {
        String url = _baseUrl + urlSuffix;
        int result = sendRequest(url);
        if (result == HTTP_REQUEST_PENDING) _requestMillis = millis();
        return result;
    }

    if (!isResponseAvailable()) return HTTP_REQUEST_PENDING;

    int httpCode = getResponse();
    TRACE(
        F("HTTP %d response after %u ms. Size: %d\n"),
        httpCode,
        _responseTimeMs,
        _response.length());
    _requestMillis = 0;

    if (httpCode < 0) return httpCode;

    if (httpCode != HTTP_OK)
    {
        _lastError = F("HTTP ");
        _lastError += httpCode;
        return httpCode;
    }

    TRACE(F("_filterDoc.size()=%d\n"), _filterDoc.size());

    JsonDocument jsonDoc;
    DeserializationError parseError = (_filterDoc.size() == 0)
        ? deserializeJson(jsonDoc, _response)
        : deserializeJson(jsonDoc, _response, DeserializationOption::Filter(_filterDoc));
    _response.clear();

    if (parseError != DeserializationError::Ok)
    {
        _lastError = F("JSON error: "); 
        _lastError += parseError.c_str();
        return RESPONSE_PARSING_FAILED;
    }

    return parseResponse(jsonDoc)
        ? HTTP_OK
        : RESPONSE_PARSING_FAILED;
}


int RESTClient::awaitData(const String& urlSuffix)
{
    Tracer tracer("RESTClient::awaitData");

    while (true)
    {
        int result = requestData(urlSuffix);
        if (result != HTTP_REQUEST_PENDING) 
            return result;
        delay(10);
    }
}


#ifdef ESP8266
int RESTClient::sendRequest(const String& url)
{
    Tracer tracer(F("RESTClient::sendRequest"), url.c_str());

    if (!_asyncHttpRequest.open("GET", url.c_str()))
    {
        _lastError = F("Open failed");
        return HTTP_OPEN_FAILED;
    }
    if (!_asyncHttpRequest.send())
    {
        _lastError = F("Send failed");
        return HTTP_SEND_FAILED;
    }
    _asyncHttpRequest.setTimeout(_timeout);
    return HTTP_REQUEST_PENDING;
}


bool RESTClient::isResponseAvailable()
{
    return _asyncHttpRequest.readyState() == readyStateDone;
}


int RESTClient::getResponse()
{
    _responseTimeMs = millis() - _requestMillis;
    int result = _asyncHttpRequest.responseHTTPcode();
    if (result == HTTP_OK)
        _response = _asyncHttpRequest.responseText();
    else if (result < 0)
        _lastError = _asyncHttpRequest.responseHTTPString();
    return result;
}
#else
int RESTClient::sendRequest(const String& url)
{
    Tracer tracer(F("RESTClient::sendRequest"), url.c_str());

    bool success = (_certificate == nullptr)
        ? _httpClient.begin(url)
        : _httpClient.begin(url, _certificate);

    if (!success)
    {
        _lastError = F("Open failed");
        return HTTP_OPEN_FAILED;
    }

    _httpResult = 0;
    return HTTP_REQUEST_PENDING;
}


bool RESTClient::isResponseAvailable()
{
    return _httpResult != 0;
}


int RESTClient::getResponse()
{
    return _httpResult;
}


void RESTClient::runHttpRequests()
{
    while (true)
    {
        if ((_requestMillis != 0) && (_httpResult == 0))
        {
            int httpResult = _httpClient.GET();
            _responseTimeMs = millis() - _requestMillis; 
            if (httpResult == HTTP_OK)
                _response = _httpClient.getString();
            else if (httpResult < 0)
                _lastError = HTTPClient::errorToString(_httpResult);
            _httpClient.end();
            _httpResult = httpResult;
        }
        delay(10);
    }
}
#endif
