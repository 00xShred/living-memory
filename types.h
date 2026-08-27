#pragma once
#include <Arduino.h>

struct TagRecord {
    String tag_id;
    String display_type;
    String message;
    String image_file;
    bool valid;
};

enum ScreenMode {
    SCREEN_DASHBOARD,
    SCREEN_CALENDAR,
    SCREEN_LETTER,
    SCREEN_WEATHER_FULL,
    SCREEN_MEMORY,
};

struct AppSettings {
    String timezone;
    String location_name;
    float latitude;
    float longitude;
    int next_visit_year;
    int next_visit_month;
    int next_visit_day;
    String next_visit_label;
    bool valid;
};

struct LetterData {
    String date;
    String title;
    String body;
    bool valid;
};

struct MemoryData {
    String date;
    String image_file;
    String caption;
    bool valid;
};

struct WeatherDay {
    String weekday;
    int max_c;
    int min_c;
    int rain_percent;
    int weather_code;
    String description;
};

struct WeatherNow {
    int temperature_c;
    int max_c;
    int min_c;
    int humidity_percent;
    int wind_kmh;
    int rain_percent;
    int weather_code;
    String description;
    bool valid;
};

struct WeatherData {
    WeatherNow current;
    WeatherDay days[7];
    int day_count;
    bool valid;
};

struct AppData {
    AppSettings settings;
    LetterData letter;
    MemoryData memory;
    WeatherData weather;
};
