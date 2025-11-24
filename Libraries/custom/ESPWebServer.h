#ifndef ESP_WEBSERVER_H
#define ESP_WEBSERVER_H

#ifdef ESP8266
    #include <ESP8266WebServer.h>
    typedef ESP8266WebServer ESPWebServer;
#else
    #include <WebServer.h>
    typedef WebServer ESPWebServer;
#endif

#include <StringBuilder.h>
#include <Tracer.h>

class ChunkedResponse
{
    public:
        ChunkedResponse(StringBuilder& builder, ESPWebServer& webServer, const String& contentType)
            : _builder(builder), _webServer(webServer)
        {
            TRACE(F("Using chunked response: %s\n"), contentType.c_str());

            _webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
            _webServer.send(200, contentType, String());

            _builder.onLowSpace([this](size_t) {
                TRACE(F("Chunk: %d\n"), _builder.length());
                _webServer.sendContent(_builder.c_str(), _builder.length());
                _builder.clear();
                });
        }

        ~ChunkedResponse()
        {
            TRACE(F("Final chunk: %d\n"), _builder.length());
            _webServer.sendContent(_builder.c_str(), _builder.length());
            _webServer.sendContent("");
  
            _builder.onLowSpace(nullptr);
            _builder.clear();
        }

    private:
        StringBuilder& _builder;
        ESPWebServer& _webServer;
};

#endif