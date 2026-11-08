#include <RESTClient.h>
#include <Tracer.h>
#include <StreamUtils.h>

#ifndef ESP8266
QueueHandle_t RESTClient::_requestQueue = nullptr;
TaskHandle_t RESTClient::_taskHandle = nullptr;
#endif


bool RESTClient::begin(const String& baseUrl, const char* certificate)
{
    _baseUrl = baseUrl;
    _certificate = certificate;

#ifdef ESP8266
    isInitialized = true;
#else
    isInitialized = ensureWorker();
    if (!isInitialized) return false;

    uint32_t timeoutMs = static_cast<uint32_t>(_timeout) * 1000;
    _httpClient.setTimeout(timeoutMs);
    _httpClient.setConnectTimeout(timeoutMs);

    _asyncRequestJob.clientPtr = this;
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
#ifdef ESP8266
        int result = startRequest(url);
        if (result == HTTP_REQUEST_PENDING) _requestMillis = millis();
        return result;
#else
        _asyncRequestJob.url = url;
        _asyncRequestJob.method = RequestMethod::GET;
        _asyncRequestJob.payload = "";
        _asyncRequestJob.asynchronous = true;
        _asyncRequestJob.completed.store(false, std::memory_order_relaxed);
        _httpResult.store(0, std::memory_order_relaxed);

        if (!enqueueRequest(_asyncRequestJob))
            return HTTP_SEND_FAILED;

        _requestMillis = 1; // Not actual millis, but indicates that a request is in progress.
        return HTTP_REQUEST_PENDING;
#endif
    }

    if (!isResponseAvailable())
        return HTTP_REQUEST_PENDING;

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
bool RESTClient::ensureWorker()
{
    static SemaphoreHandle_t initMutex = xSemaphoreCreateMutex();
    if (!initMutex) return false;

    xSemaphoreTake(initMutex, portMAX_DELAY);

    if (!_requestQueue)
        _requestQueue = xQueueCreate(10, sizeof(RequestJob*));

    BaseType_t taskResult = pdPASS;
    if (_requestQueue && !_taskHandle)
    {
        taskResult = xTaskCreate(
            run,
            "RESTClient",
            4096,
            nullptr,
            tskIDLE_PRIORITY,
            &_taskHandle);
        if (taskResult != pdPASS)
            _taskHandle = nullptr;
    }

    bool success = _requestQueue && _taskHandle;
    xSemaphoreGive(initMutex);
    TRACE("RESTClient::ensureWorker: initialized=%d, task result=%d\n", success, taskResult);
    return success;
}


void RESTClient::run(void* taskParam)
{
    (void)taskParam;

    while (true)
    {
        RequestJob* requestJobPtr = nullptr;
        BaseType_t result = xQueueReceive(_requestQueue, &requestJobPtr, portMAX_DELAY);
        if (result != pdPASS)
        {
            TRACE("RESTClient::run: xQueueReceive failed with %d\n", result);
            continue;
        }

        if (!requestJobPtr)
        {
            TRACE("RESTClient::run: received a null request job\n");
            continue;
        }

        if (!requestJobPtr->clientPtr)
        {
            TRACE("RESTClient::run: request job has no client\n");
            continue;
        }

        requestJobPtr->clientPtr->runRequest(*requestJobPtr);
    }
}


bool RESTClient::enqueueRequest(RequestJob& requestJob, TickType_t waitTicks)
{
    if (!_requestQueue)
    {
        setLastError("Request queue is not initialized");
        return false;
    }

    RequestJob* requestJobPtr = &requestJob;
    if (xQueueSend(_requestQueue, &requestJobPtr, waitTicks) != pdPASS)
    {
        setLastError("Request queue is full");
        return false;
    }

    return true;
}


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
        _lastError = "Open failed";
        return HTTP_OPEN_FAILED;
    }

    return HTTP_REQUEST_PENDING;
}


bool RESTClient::isResponseAvailable()
{
    return _httpResult.load(std::memory_order_acquire) != 0;
}


int RESTClient::getResponse()
{
    return _httpResult.load(std::memory_order_acquire);
}


void RESTClient::runRequest(RequestJob& requestJob)
{
    uint32_t startedAt = millis();
    int result = startRequest(requestJob.url);
    if (result == HTTP_REQUEST_PENDING)
    {
        switch (requestJob.method)
        {
            case RequestMethod::GET:
                result = _httpClient.GET();
                break;
            case RequestMethod::POST:
                result = _httpClient.POST(requestJob.payload);
                break;
            case RequestMethod::PUT:
                result = _httpClient.PUT(requestJob.payload);
                break;
            case RequestMethod::DELETE:
                result = _httpClient.sendRequest("DELETE");
                break;
            default:
                result = HTTP_SEND_FAILED;
                _lastError = "Unsupported request method";
                break;
        }

        if (requestJob.asynchronous && result == HTTP_OK)
        {
            int size = _httpClient.getSize();
            if (size < 0) size = 4095;
            if (_responsePtr)
            {
                delete _responsePtr;
                _responsePtr = nullptr;
            }
            _responsePtr = new MemoryStream(size, _memoryType);
            _httpClient.writeToStream(_responsePtr);
        }
        else if (!requestJob.asynchronous)
            requestJob.response = _httpClient.getString();

        if (result < 0)
            _lastError = HTTPClient::errorToString(result);
        else if (result != HTTP_OK)
        {
            _lastError = "HTTP ";
            _lastError += result;
            if (!requestJob.asynchronous)
            {
                _lastError += ": ";
                _lastError += requestJob.response;
            }
        }

        _httpClient.end();
    }

    _responseTimeMs = millis() - startedAt;
    requestJob.result = result;
    if (requestJob.asynchronous)
    {
        _httpResult.store(result, std::memory_order_release);
    }
    requestJob.completed.store(true, std::memory_order_release);
}


int RESTClient::request(RequestMethod method, const String& urlSuffix, const String& payload, String& response)
{
    Tracer tracer("RESTClient::request", urlSuffix.c_str());

    String url = urlSuffix.startsWith("http") ? urlSuffix : _baseUrl + urlSuffix;
    RequestJob requestJob;
    requestJob.clientPtr = this;
    requestJob.url = url;
    requestJob.method = method;
    requestJob.payload = payload;
    requestJob.completed.store(false, std::memory_order_relaxed);

    if (!enqueueRequest(requestJob))
        return HTTP_SEND_FAILED;

    while (!requestJob.completed.load(std::memory_order_acquire))
        delay(1);

    response = requestJob.response;
    TRACE("HTTP %d:\n%s\n", requestJob.result, response.c_str());
    return requestJob.result;
}

void RESTClient::resetTLS() 
{
    constexpr uint32_t waitTimeoutMs = 30000;
    uint32_t waitStartedAt = millis();

    while (isRequestPending() && millis() - waitStartedAt < waitTimeoutMs)
        delay(1);

    if (isRequestPending())
    {
        setLastError("Timed out waiting for pending request before resetting TLS");
        return;
    }

    if (!_tlsClientPtr) return;
    if (_tlsClientPtr->connected()) _tlsClientPtr->stop();
    delete _tlsClientPtr;
    _tlsClientPtr = nullptr;
}

#endif
