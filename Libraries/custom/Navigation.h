#ifndef NAVIGATION_H
#define NAVIGATION_H

#include <ESPWebServer.h>
#include <vector>

struct MenuItem
{
    PGM_P icon;
    PGM_P label;
    PGM_P urlPath = nullptr;
    std::function<void(void)> handler;
    std::function<void(void)> postHandler = nullptr;
};

struct Navigation
{
    bool isLocalizable = false;
    String width = "10em";
    std::vector<MenuItem> menuItems;

    void registerHttpHandlers(ESPWebServer& webServer);
};

#endif