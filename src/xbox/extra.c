#include <stdlib.h>

float _strtod(const char* s, __attribute__((unused)) char** endptr);

double atof(const char *str) {_strtod(str, NULL);}

// Source - https://stackoverflow.com/a/4392789
// Posted by ruslik, modified by community. See post 'Timeline' for change history
// Retrieved 2026-04-04, License - CC BY-SA 2.5

float _strtof(const char* s, __attribute__((unused)) char** endptr) {
    float rez = 0, fact = 1;
    if (*s == '-'){
        s++;
        fact = -1;
    };
    for (int point_seen = 0; *s; s++){
        if (*s == '.'){
            point_seen = 1; 
            continue;
        };
        int d = *s - '0';
        if (d >= 0 && d <= 9){
            if (point_seen) fact /= 10.0f;
                rez = rez * 10.0f + (float)d;
        };
    };
    return rez * fact;
};

float _strtod(const char* s, __attribute__((unused)) char** endptr) {
    double rez = 0, fact = 1;
    if (*s == '-'){
        s++;
        fact = -1;
    };
    for (int point_seen = 0; *s; s++){
        if (*s == '.'){
            point_seen = 1; 
            continue;
        };
        int d = *s - '0';
        if (d >= 0 && d <= 9){
            if (point_seen) fact /= 10.0;
                rez = rez * 10.0f + (double)d;
        };
    };
    return rez * fact;
};
