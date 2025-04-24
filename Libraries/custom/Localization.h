#ifndef LOCALIZATION_H
#define LOCALIZATION_H

#include <functional>
#include <map>
#include <vector>
#include <WString.h>

#define L10N(name) Localization::localize(name)

class Localization
{
    public:
        static std::map<const char*, std::vector<const char*>> translations;
        static inline std::function<String(void)> getLanguage;

        static const char* localize(const char* english)
        {
            int langId = -1;
            if (getLanguage)
            {
                String language = getLanguage();
                if (language.startsWith("nl")) langId = 0;
            }

            if (langId < 0) return english;

            std::vector<const char*> strings = translations[english];
            return (langId < strings.size())
                ? strings[langId]    
                : english; // Translation not found
        }
};

#endif