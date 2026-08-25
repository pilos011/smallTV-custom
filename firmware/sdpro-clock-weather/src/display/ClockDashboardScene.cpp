// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * SmallTV-Ultra Korean Custom Firmware
 * Copyright (C) 2026 Times-Z
 */

#include "display/ClockDashboardScene.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ClockDashboard {

static auto textFor(const char* korean) -> std::string {
    return std::string(korean);
}

static auto replaceAll(std::string value, char from, char to) -> std::string {
    for (char& c : value) {
        if (c == from) {
            c = to;
        }
    }
    return value;
}

static auto formatDecimal(float value) -> std::string {
    const long scaled = std::lround(static_cast<double>(value) * 10.0);
    const bool negative = scaled < 0;
    const unsigned long magnitude = static_cast<unsigned long>(negative ? -scaled : scaled);
    const unsigned long whole = magnitude / 10UL;
    const unsigned long fraction = magnitude % 10UL;

    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%s%lu.%lu", negative ? "-" : "", whole, fraction);
    return std::string(buffer);
}

static auto formatClockLine(const Input& input) -> std::string {
    std::tm* timeInfo = std::localtime(&input.now);
    if (timeInfo == nullptr) {
        return "";
    }

    int hour = timeInfo->tm_hour;
    std::string suffix;

    if (!input.use24Hour) {
        suffix = (hour >= 12) ? " 오후" : " 오전";
        hour %= 12;
        if (hour == 0) {
            hour = 12;
        }
    }

    char buffer[16] = {};
    if (input.showSeconds) {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hour, timeInfo->tm_min, timeInfo->tm_sec);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, timeInfo->tm_min);
    }

    return std::string(buffer) + suffix;
}

static auto formatDateLine(const Input& input) -> std::string {
    std::tm* timeInfo = std::localtime(&input.now);
    if (timeInfo == nullptr) {
        return "";
    }

    static constexpr const char* KO_WEEKDAYS[] = {"일", "월", "화", "수", "목", "금", "토"};
    const char* weekday = KO_WEEKDAYS[timeInfo->tm_wday];

    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d/%02d/%02d", timeInfo->tm_year + 1900, timeInfo->tm_mon + 1,
                  timeInfo->tm_mday);
    return std::string(buffer) + " " + weekday;
}

static auto formatForecastHour(std::time_t timestamp) -> std::string {
    std::tm* timeInfo = std::gmtime(&timestamp);
    if (timeInfo == nullptr) {
        return "--";
    }

    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d", timeInfo->tm_hour);
    return std::string(buffer);
}

static auto formatForecastHourLabel(std::time_t timestamp) -> std::string {
    std::string hour = formatForecastHour(timestamp);
    hour += "시";
    return hour;
}

static auto hourSerialFromTm(const std::tm& timeInfo) -> long {
    return ((static_cast<long>(timeInfo.tm_year) * 366L) + static_cast<long>(timeInfo.tm_yday)) * 24L +
           static_cast<long>(timeInfo.tm_hour);
}

static auto currentLocalHourSerial(std::time_t timestamp) -> long {
    std::tm* timeInfo = std::localtime(&timestamp);
    if (timeInfo == nullptr) {
        return -1;
    }
    return hourSerialFromTm(*timeInfo);
}

static auto forecastDisplayHourSerial(std::time_t timestamp) -> long {
    std::tm* timeInfo = std::gmtime(&timestamp);
    if (timeInfo == nullptr) {
        return -1;
    }
    return hourSerialFromTm(*timeInfo);
}

static auto daySerialFromTm(const std::tm& timeInfo) -> long {
    return (static_cast<long>(timeInfo.tm_year) * 366L) + static_cast<long>(timeInfo.tm_yday);
}

static auto localDaySerial(std::time_t timestamp) -> long {
    std::tm* timeInfo = std::localtime(&timestamp);
    if (timeInfo == nullptr) {
        return -1;
    }
    return daySerialFromTm(*timeInfo);
}

static auto forecastDisplayDaySerial(std::time_t timestamp) -> long {
    std::tm* timeInfo = std::gmtime(&timestamp);
    if (timeInfo == nullptr) {
        return -1;
    }
    return daySerialFromTm(*timeInfo);
}

static auto formatTodayHighLabel(const WeatherInput& weather, std::time_t now, bool validTime) -> std::string {
    if (weather.hasTodayHighTemperature) {
        return std::string("↑") + std::to_string(static_cast<int>(std::lround(weather.todayHighTemperature)));
    }

    float highest = weather.currentTemperature;
    const long todaySerial = validTime ? localDaySerial(now) : -1;

    for (const ForecastEntry& entry : weather.forecast) {
        if (!entry.hasData) {
            continue;
        }
        const long entryDaySerial = forecastDisplayDaySerial(entry.timestamp);
        if (todaySerial >= 0 && entryDaySerial >= 0 && entryDaySerial != todaySerial) {
            continue;
        }
        if (entry.temperature > highest) {
            highest = entry.temperature;
        }
    }

    return std::string("↑") + std::to_string(static_cast<int>(std::lround(highest)));
}

static auto formatForecastMetric(const ForecastEntry& entry) -> std::string {
    if (entry.precipitation > 0.0F) {
        std::string label = formatDecimal(entry.precipitation);
        label += "mm";
        return label;
    }

    if (entry.humidity >= 0.0F) {
        const int humidity = static_cast<int>(std::lround(entry.humidity));
        return std::to_string(humidity) + "%";
    }

    return "";
}

static void splitClockLine(const std::string& clockLine, std::string& primary, std::string& secondary, std::string& suffix) {
    suffix.clear();
    std::string timePart = clockLine;
    const std::size_t spacePos = clockLine.find(' ');
    if (spacePos != std::string::npos) {
        timePart = clockLine.substr(0, spacePos);
        suffix = clockLine.substr(spacePos + 1);
    }

    const std::size_t firstColon = timePart.find(':');
    const std::size_t secondColon = firstColon == std::string::npos ? std::string::npos : timePart.find(':', firstColon + 1);
    if (firstColon == std::string::npos) {
        primary = timePart;
        secondary.clear();
        return;
    }

    if (secondColon == std::string::npos) {
        primary = timePart;
        secondary.clear();
        return;
    }

    primary = timePart.substr(0, secondColon);
    secondary = timePart.substr(secondColon + 1);
}

static void deriveLocationFromTimezone(const std::string& timezone, std::string& city) {
    if (timezone == "Asia/Seoul") {
        city = "서울";
        return;
    }
    if (timezone == "Asia/Tokyo") {
        city = "도쿄";
        return;
    }
    if (timezone == "Asia/Shanghai") {
        city = "상하이";
        return;
    }
    if (timezone == "Asia/Singapore") {
        city = "싱가포르";
        return;
    }
    if (timezone == "Asia/Bangkok") {
        city = "방콕";
        return;
    }
    if (timezone == "Europe/London") {
        city = "런던";
        return;
    }
    if (timezone == "Europe/Berlin") {
        city = "베를린";
        return;
    }
    if (timezone == "America/New_York") {
        city = "뉴욕";
        return;
    }
    if (timezone == "America/Chicago") {
        city = "시카고";
        return;
    }
    if (timezone == "America/Denver") {
        city = "덴버";
        return;
    }
    if (timezone == "America/Los_Angeles") {
        city = "로스앤젤레스";
        return;
    }
    if (timezone == "Australia/Sydney") {
        city = "시드니";
        return;
    }
    if (timezone == "UTC" || timezone == "auto" || timezone.empty()) {
        city = "시간";
        return;
    }

    const std::size_t slashPos = timezone.find('/');
    if (slashPos == std::string::npos) {
        city = replaceAll(timezone, '_', ' ');
        return;
    }

    city = replaceAll(timezone.substr(slashPos + 1), '_', ' ');
}

static auto compactLocationName(std::string locationName) -> std::string {
    if (locationName.empty()) {
        return locationName;
    }

    const std::pair<const char*, const char*> replacements[] = {
        {"특별자치시", "시"},
        {"특별시", "시"},
        {"광역시", "시"},
        {"특별자치도", "도"},
    };

    for (const auto& replacement : replacements) {
        std::size_t pos = 0;
        while ((pos = locationName.find(replacement.first, pos)) != std::string::npos) {
            locationName.replace(pos, std::strlen(replacement.first), replacement.second);
            pos += std::strlen(replacement.second);
        }
    }

    std::string displayName;
    std::size_t start = 0;
    while (start < locationName.size()) {
        const std::size_t space = locationName.find(' ', start);
        const std::size_t end = space == std::string::npos ? locationName.size() : space;
        std::string part = locationName.substr(start, end - start);

        while (!part.empty() && part.front() == ' ') {
            part.erase(part.begin());
        }
        while (!part.empty() && part.back() == ' ') {
            part.pop_back();
        }

        if (!part.empty()) {
            const bool provinceToken = part.size() >= std::strlen("도") && part.rfind("도") == part.size() - std::strlen("도");
            if (!provinceToken) {
                displayName = part;
            }
        }

        if (space == std::string::npos) {
            break;
        }
        start = space + 1;
    }

    return displayName.empty() ? locationName : displayName;
}

auto weatherIconSlot(int weatherCode) -> const char* {
    switch (weatherCode) {
        case 0:
            return "clear";
        case 1:
        case 2:
        case 3:
            return "cloudy";
        case 45:
        case 48:
            return "fog";
        case 51:
        case 53:
        case 55:
        case 61:
        case 63:
        case 65:
        case 80:
        case 81:
        case 82:
            return "rain";
        case 71:
        case 73:
        case 75:
        case 85:
        case 86:
            return "snow";
        case 95:
        case 96:
        case 99:
            return "storm";
        default:
            return "cloudy";
    }
}

auto buildScene(const Input& input) -> Scene {
    Scene scene;

    if (input.isAccessPointMode) {
        if (input.showLegacyUpdateRoute) {
            scene.waitLine2 = textFor("http://192.168.4.1/legacyupdate");
            const std::string ssid = input.accessPointSsid.empty() ? "GeekMagic" : input.accessPointSsid;
            scene.waitLine1 = std::string("AP SSID: ") + ssid;
        } else {
            scene.waitLine1 = textFor("http://192.168.4.1/");
            scene.waitLine2 = textFor("WiFi setup required");
        }
        return scene;
    }

    if (!input.validTime && input.showClock) {
        scene.waitLine1 = textFor("시간 동기화 대기 중");
        scene.waitLine2 = textFor("와이파이 연결 후 NTP 동기화");
        return scene;
    }

    if (input.showClock) {
        scene.clockTime = formatClockLine(input);
        splitClockLine(scene.clockTime, scene.clockPrimary, scene.clockSecondary, scene.clockSuffix);
        scene.clockDate = formatDateLine(input);
    }

    if (!input.showWeather) {
        return scene;
    }

    if (!input.weather.hasData) {
        scene.weatherIconCode = -1;
        return scene;
    }

    if (!input.weather.locationName.empty()) {
        scene.locationName = compactLocationName(input.weather.locationName);
    }
    if (scene.locationName.empty()) {
        std::string derivedLocation;
        deriveLocationFromTimezone(input.weather.timezone, derivedLocation);
        if (scene.locationName.empty()) {
            scene.locationName = derivedLocation;
        }
    }
    scene.weatherIconCode = input.weather.currentWeatherCode;
    scene.todayHighLabel = formatTodayHighLabel(input.weather, input.now, input.validTime);

    const long currentHourSerial = input.validTime ? currentLocalHourSerial(input.now) : -1;
    size_t visualIndex = 0;
    for (size_t i = 0; i < input.weather.forecast.size() && visualIndex < scene.weatherForecastVisuals.size(); ++i) {
        const ForecastEntry& entry = input.weather.forecast[i];
        if (!entry.hasData) {
            continue;
        }
        const long entryHourSerial = forecastDisplayHourSerial(entry.timestamp);
        if (currentHourSerial >= 0 && entryHourSerial >= 0 && entryHourSerial < currentHourSerial) {
            continue;
        }

        Scene::ForecastVisual visual;
        visual.hasData = true;
        visual.hourLabel = formatForecastHourLabel(entry.timestamp);
        visual.temperatureLabel = std::to_string(static_cast<int>(std::lround(entry.temperature)));
        visual.temperatureLabel += "℃";
        visual.weatherCode = entry.weatherCode;
        visual.precipitationLabel = formatForecastMetric(entry);
        scene.weatherForecastVisuals[visualIndex] = visual;
        ++visualIndex;
    }

    return scene;
}

}  // namespace ClockDashboard
