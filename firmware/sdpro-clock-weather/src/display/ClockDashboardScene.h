// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * SmallTV-Ultra Korean Custom Firmware
 * Copyright (C) 2026 Times-Z
 */

#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <string>

namespace ClockDashboard {

struct ForecastEntry {
    bool hasData = false;
    std::time_t timestamp = 0;
    float temperature = 0.0F;
    float precipitation = 0.0F;
    float precipitationProbability = -1.0F;
    float humidity = -1.0F;
    int weatherCode = -1;
};

struct WeatherInput {
    bool hasData = false;
    bool isRaining = false;
    float currentTemperature = 0.0F;
    float currentRain = 0.0F;
    float currentPrecipitation = 0.0F;
    float currentPrecipitationProbability = -1.0F;
    float currentHumidity = -1.0F;
    bool hasTodayHighTemperature = false;
    float todayHighTemperature = 0.0F;
    int currentWeatherCode = -1;
    std::string timezone;
    std::string locationName;
    std::string status;
    std::array<ForecastEntry, 5> forecast{};
};

struct Input {
    bool showClock = false;
    bool showWeather = false;
    bool use24Hour = true;
    bool showSeconds = false;
    bool validTime = false;
    bool isAccessPointMode = false;
    bool showLegacyUpdateRoute = false;
    std::time_t now = 0;
    std::string accessPointSsid;
    WeatherInput weather{};
};

struct Scene {
    struct ForecastVisual {
        bool hasData = false;
        std::string hourLabel;
        std::string temperatureLabel;
        std::string conditionLabel;
        std::string precipitationLabel;
        int weatherCode = -1;
    };

    std::string waitLine1;
    std::string waitLine2;
    std::string locationName;
    std::string clockTitle;
    std::string clockTime;
    std::string clockPrimary;
    std::string clockSecondary;
    std::string clockSuffix;
    std::string clockDate;
    std::string weatherTitle;
    std::string weatherCurrent;
    std::string todayHighLabel;
    std::array<std::string, 4> weatherForecast{};
    std::array<ForecastVisual, 4> weatherForecastVisuals{};
    int weatherIconCode = -1;
};

inline constexpr int16_t SCREEN_W = 240;
inline constexpr int16_t SCREEN_H = 240;
inline constexpr int16_t SCREEN_PADDING = 10;
inline constexpr int16_t HEADER_Y = 14;
inline constexpr int16_t HEADER_X = 14;
inline constexpr int16_t HEADER_RIGHT_PADDING = 0;
inline constexpr int16_t CURRENT_ICON_SIZE = 52;
inline constexpr int16_t TIME_LEFT_X = 5;
inline constexpr int16_t TIME_TOP_Y = 48;
inline constexpr int16_t TIME_RIGHT_PADDING = 10;
inline constexpr int16_t TIME_SECONDARY_GAP = 2;
inline constexpr int16_t DATE_Y = 85;
inline constexpr int16_t DIVIDER_Y = 106;
inline constexpr int16_t FORECAST_TOP = 136;
inline constexpr int16_t FORECAST_LEFT = 6;
inline constexpr int16_t FORECAST_GAP = 4;
inline constexpr int16_t FORECAST_WIDTH = 54;
inline constexpr int16_t FORECAST_HEIGHT = 99;
inline constexpr int16_t FORECAST_ICON_SIZE = 28;
inline constexpr int16_t FORECAST_ICON_Y_OFFSET = 40;
inline constexpr int16_t WAIT_LINE_1_Y = 56;
inline constexpr int16_t WAIT_LINE_2_Y = 96;
inline constexpr int16_t WAIT_LINE_HEIGHT = 28;
inline constexpr int16_t CLOCK_TITLE_Y = 16;
inline constexpr int16_t CLOCK_TIME_Y = 50;
inline constexpr int16_t CLOCK_DATE_Y = 128;
inline constexpr int16_t WEATHER_TITLE_Y = 154;
inline constexpr int16_t WEATHER_CURRENT_Y = 178;
inline constexpr int16_t WEATHER_FORECAST_Y = 202;
inline constexpr int16_t DASHBOARD_INNER_PADDING = 14;
inline constexpr int16_t CLOCK_CARD_X = 8;
inline constexpr int16_t CLOCK_CARD_Y = 8;
inline constexpr int16_t CLOCK_CARD_W = 224;
inline constexpr int16_t CLOCK_CARD_H = 134;
inline constexpr int16_t WEATHER_CARD_X = 8;
inline constexpr int16_t WEATHER_CARD_Y = 150;
inline constexpr int16_t WEATHER_CARD_W = 224;
inline constexpr int16_t WEATHER_CARD_H = 82;
inline constexpr int16_t CLOCK_PANEL_ACCENT_Y = 34;
inline constexpr int16_t WEATHER_PANEL_ACCENT_Y = 172;
inline constexpr int16_t CLOCK_TITLE_HEIGHT = 18;
inline constexpr int16_t CLOCK_TIME_HEIGHT = 52;
inline constexpr int16_t CLOCK_DATE_HEIGHT = 18;
inline constexpr int16_t WEATHER_TITLE_HEIGHT = 18;
inline constexpr int16_t WEATHER_CURRENT_HEIGHT = 18;
inline constexpr int16_t WEATHER_FORECAST_ROW_HEIGHT = 10;
inline constexpr int16_t CLOCK_TEXT_MIN_X = CLOCK_CARD_X + DASHBOARD_INNER_PADDING;
inline constexpr int16_t CLOCK_TEXT_MAX_W = CLOCK_CARD_W - (DASHBOARD_INNER_PADDING * 2);
inline constexpr int16_t CLOCK_TIME_MIN_X = CLOCK_CARD_X + 10;
inline constexpr int16_t CLOCK_TIME_MAX_W = CLOCK_CARD_W - 20;
inline constexpr int16_t WEATHER_TEXT_MIN_X = WEATHER_CARD_X + DASHBOARD_INNER_PADDING;
inline constexpr int16_t WEATHER_TEXT_MAX_W = WEATHER_CARD_W - (DASHBOARD_INNER_PADDING * 2);
inline constexpr int16_t WEATHER_ICON_CLEAR_X = WEATHER_CARD_X + 12;
inline constexpr int16_t WEATHER_ICON_CLEAR_Y = WEATHER_CURRENT_Y - 1;
inline constexpr int16_t WEATHER_ICON_DRAW_X = WEATHER_CARD_X + 13;
inline constexpr int16_t WEATHER_ICON_DRAW_Y = WEATHER_CURRENT_Y;
inline constexpr int16_t WEATHER_ICON_BOX_SIZE = 22;
inline constexpr int16_t WEATHER_ICON_SIZE = 20;
inline constexpr int16_t WEATHER_CURRENT_TEXT_X = WEATHER_CARD_X + 42;

auto buildScene(const Input& input) -> Scene;
auto weatherIconSlot(int weatherCode) -> const char*;

}  // namespace ClockDashboard
