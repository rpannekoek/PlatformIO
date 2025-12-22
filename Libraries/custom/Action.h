#pragma once

#include <WString.h>

struct Action
{
    String name;
    String label;
    uint16_t reloadAfterSeconds = 0;
};
