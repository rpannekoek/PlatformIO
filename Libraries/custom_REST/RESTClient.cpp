#include <RESTClient.h>
#include <Tracer.h>
#include <StreamUtils.h>


bool RESTClient::begin(const String& baseUrl, const char* certificate, bool usePSRAM)
{
    _baseUrl = baseUrl;
    _certificate = certificate;
    _requestMillis = 0;
    _usePSRAM = usePSRAM;

#ifdef ESP8266
    isInitialized = true;
#else
    uint32_t timeoutMs = static_cast<uint32_t>(_timeout) * 1000;
    _httpClient.setTimeout(timeoutMs);
    _httpClient.setConnectTimeout(timeoutMs);

    BaseType_t res = xTaskCreate(
        run,
        "RESTClient",
        4096, // Stack size
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
#ifndef ESP8266     
        _httpClient.setAuthorizationType("Bearer");
        _httpClient.setAuthorization(bearerToken.c_str());
#endif
    }
}


void RESTClient::setHeader(const String& name, const String& value)
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
        int result = startRequest(url);
        if (result == HTTP_REQUEST_PENDING) _requestMillis = millis();
        return result;
    }

    if (!isResponseAvailable()) return HTTP_REQUEST_PENDING;

    int httpCode = getResponse();
    int responseSize = _responsePtr ? _responsePtr->size() : -1; 
    TRACE(
        F("HTTP %d response after %u ms. Size: %d\n"),
        httpCode,
        _responseTimeMs,
        responseSize);
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
    DeserializationError jsonError = DeserializationError::EmptyInput;
    if (_responsePtr)
    {
        jsonError = (_filterDoc.size() == 0)
            ? deserializeJson(jsonDoc, _responsePtr->c_str())
            : deserializeJson(jsonDoc, _responsePtr->c_str(), DeserializationOption::Filter(_filterDoc));

        delete _responsePtr;
        _responsePtr = nullptr;
    }
        
    if (jsonError != DeserializationError::Ok)
    {
        _lastError = F("JSON error: "); 
        _lastError += jsonError.c_str();
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


int RESTClient::request(RequestMethod method, const String& urlSuffix, const String& payload, JsonDocument& response)
{
    String responseStr;
    int rc = request(method, urlSuffix, payload, responseStr);
    if (rc != HTTP_OK) return rc;

    DeserializationError parseError = deserializeJson(response, responseStr);
    if (parseError != DeserializationError::Ok)
    {
        setLastError(String("JSON error: ") + parseError.c_str());
        return RESPONSE_PARSING_FAILED;
    }

    return HTTP_OK;
}


#ifdef ESP8266
int RESTClient::startRequest(const String& url)
{
    Tracer tracer(F("RESTClient::startRequest"), url.c_str());

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
        _responsePtr = new MemoryStream(_asyncHttpRequest.responseText());
    else if (result < 0)
        _lastError = _asyncHttpRequest.responseHTTPString();
    return result;
}


int RESTClient::request(RequestMethod method, const String& urlSuffix, const String& payload, String& response)
{
    return HTTP_OPEN_FAILED; // TODO
}

void RESTClient::resetTLS() 
{
}

#else
int RESTClient::startRequest(const String& url)
{
    TRACE("RESTClient::startRequest(\"%s\")\n", url.c_str());

    bool success;
    if (url.startsWith("https:"))
    {
        if (!_tlsClientPtr)
        {
            _tlsClientPtr = new NetworkClientSecure();
            if (_certificate == nullptr)
                _tlsClientPtr->setInsecure(); // Skip certificate validation
            else
                _tlsClientPtr->setCACert(_certificate);
        }
        success = _httpClient.begin(*_tlsClientPtr, url); 
    }
    else
        success = _httpClient.begin(url);

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
        if (isRequestPending())
        {
            int httpResult = _httpClient.GET();
            _responseTimeMs = millis() - _requestMillis; 
            if (httpResult == HTTP_OK)
            {
                int size = _httpClient.getSize();
                if (size < 0) size = 4095;
                _responsePtr = new MemoryStream(size, _usePSRAM);
                _httpClient.writeToStream(_responsePtr);
            }
            else if (httpResult < 0)
                _lastError = HTTPClient::errorToString(httpResult);
            _httpClient.end();
            _httpResult = httpResult;
        }
        delay(10);
    }
}


int RESTClient::request(RequestMethod method, const String& urlSuffix, const String& payload, String& response)
{
    Tracer tracer(F("RESTClient::request"), urlSuffix.c_str());

    String url = urlSuffix.startsWith("http") ? urlSuffix : _baseUrl + urlSuffix;
    int startResult = startRequest(url);
    if (startResult != HTTP_REQUEST_PENDING) return startResult;

    int result = -1;
    switch (method)
    {
        case RequestMethod::GET:
            result = _httpClient.GET();
            break;
        case RequestMethod::POST:
            result = _httpClient.POST(payload);
            break;
        case RequestMethod::PUT:
            result = _httpClient.PUT(payload);
            break;
        default:
            TRACE("Unexpected Request method");
            return false;
    }
    response = _httpClient.getString();
    _httpClient.end();
    TRACE("HTTP %d:\n%s\n", result, response.c_str());

    if (result != HTTP_OK)
    {
        if (result < 0)
            _lastError = HTTPClient::errorToString(result);
        else
        {
            _lastError = "HTTP ";
            _lastError += result;
            _lastError += ": ";
            _lastError += response;
        }
    }
    return result;
}

void RESTClient::resetTLS() 
{
    if (_tlsClientPtr)
    {
        if (_tlsClientPtr->connected()) _tlsClientPtr->stop();
        delete _tlsClientPtr;
        _tlsClientPtr = nullptr;
    }
}

#endif
