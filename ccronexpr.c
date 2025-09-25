/*
 * Copyright 2015, alex at staticlibs.net
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * File:   ccronexpr.c
 * Author: alex
 *
 * Created on February 24, 2015, 9:35 AM
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <math.h>

#include "ccronexpr.h"

#define CRON_MAX_SECONDS 60
#define CRON_MAX_MINUTES 60
#define CRON_MAX_HOURS 24
#define CRON_MAX_DAYS_OF_MONTH 32
#define CRON_MAX_MONTHS 12
#define CRON_MAX_YEARS_DIFF 4

#define CRON_CF_SECOND 0
#define CRON_CF_MINUTE 1
#define CRON_CF_HOUR_OF_DAY 2
#define CRON_CF_DAY_OF_WEEK 3
#define CRON_CF_DAY_OF_MONTH 4
#define CRON_CF_MONTH 5
#define CRON_CF_YEAR 6

#define CRON_CF_ARR_LEN 7


static const char* const DAYS_ARR[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
#define CRON_DAYS_ARR_LEN 7
static const char* const MONTHS_ARR[] = { "FOO", "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
#define CRON_MONTHS_ARR_LEN 13

#define CRON_MAX_STR_LEN_TO_SPLIT 256
#define CRON_SIZE_STRING_MAX_LEN 20

#ifndef CRON_TEST_MALLOC
#define cron_malloc(x) malloc(x)
#define cron_free(x) free(x)
#else /* CRON_TEST_MALLOC */
void* cron_malloc(size_t n);
void cron_free(void* p);
#endif /* CRON_TEST_MALLOC */

/**
 * Time functions from standard library.
 * This part defines: cron_mktime: create time_t from tm
 *                    cron_time: create tm from time_t
 */

/* forward declarations for platforms that may need them */
/* can be hidden in time.h */
#if !defined(_WIN32) && !defined(__AVR__) && !defined(ESP8266) && !defined(ESP_PLATFORM) && !defined(ANDROID) && !defined(TARGET_LIKE_MBED)
struct tm *gmtime_r(const time_t *timep, struct tm *result);
time_t timegm(struct tm* __tp);
struct tm *localtime_r(const time_t *timep, struct tm *result);
#endif /* PLEASE CHECK _WIN32 AND ANDROID NEEDS FOR THESE DECLARATIONS */
#ifdef __MINGW32__
/* To avoid warning when building with mingw */
time_t _mkgmtime(struct tm* tm);
#endif /* __MINGW32__ */

#if defined(ESP8266) || defined(ESP_PLATFORM) || defined(TARGET_LIKE_MBED)
  #if !defined(CRON_USE_LOCAL_TIME)
    #define CRON_USE_LOCAL_TIME
  #endif
#endif

/* function definitions */
#ifndef CRON_USE_LOCAL_TIME

static time_t cron_mktime_gm(struct tm* tm) {
#if defined(_WIN32)
/* http://stackoverflow.com/a/22557778 */
    return _mkgmtime(tm);
#elif defined(__AVR__)
/* https://www.nongnu.org/avr-libc/user-manual/group__avr__time.html */
    return mk_gmtime(tm);
#elif defined(ESP8266) || defined(ESP_PLATFORM) || defined(TARGET_LIKE_MBED)

#error "timegm() is not supported on the ESP platform, please use this library with CRON_USE_LOCAL_TIME"

#elif defined(ANDROID) && !defined(__LP64__)
    /* https://github.com/adobe/chromium/blob/cfe5bf0b51b1f6b9fe239c2a3c2f2364da9967d7/base/os_compat_android.cc#L20 */
    static const time_t kTimeMax = ~(1L << (sizeof (time_t) * CHAR_BIT - 1));
    static const time_t kTimeMin = (1L << (sizeof (time_t) * CHAR_BIT - 1));
    time64_t result = timegm64(tm);
    if (result < kTimeMin || result > kTimeMax) return -1;
    return result;
#else
    return timegm(tm);
#endif
}

static struct tm* cron_time_gm(time_t* date, struct tm* out) {
#if defined(__MINGW32__)
    (void)(out); /* To avoid unused warning */
    return gmtime(date);
#elif defined(_WIN32)
    errno_t err = gmtime_s(out, date);
    return 0 == err ? out : NULL;
#elif defined(__AVR__)
    /* https://www.nongnu.org/avr-libc/user-manual/group__avr__time.html */
    gmtime_r(date, out);
    return out;
#else
    return gmtime_r(date, out);
#endif
}

#else

static time_t cron_mktime_local(struct tm* tm) {
    tm->tm_isdst = -1;
    return mktime(tm);
}

static struct tm* cron_time_local(time_t* date, struct tm* out) {
#if defined(_WIN32)
    errno_t err = localtime_s(out, date);
    return 0 == err ? out : NULL;
#elif defined(__AVR__)
    /* https://www.nongnu.org/avr-libc/user-manual/group__avr__time.html */
    localtime_r(date, out);
    return out;
#else
    return localtime_r(date, out);
#endif
}

#endif

/* Defining 'cron_' time functions to use use UTC (default) or local time */
#ifndef CRON_USE_LOCAL_TIME
time_t cron_mktime(struct tm* tm) {
    return cron_mktime_gm(tm);
}

struct tm* cron_time(time_t* date, struct tm* out) {
    return cron_time_gm(date, out);
}

#else /* CRON_USE_LOCAL_TIME */
time_t cron_mktime(struct tm* tm) {
    return cron_mktime_local(tm);
}

struct tm* cron_time(time_t* date, struct tm* out) {
    return cron_time_local(date, out);
}

#endif /* CRON_USE_LOCAL_TIME */

/**
 * Functions.
 */

void cron_set_bit(uint8_t* rbyte, int idx) {
    uint8_t j = (uint8_t) (idx / 8);
    uint8_t k = (uint8_t) (idx % 8);

    rbyte[j] |= (uint8_t)(1 << k);
}

void cron_del_bit(uint8_t* rbyte, int idx) {
    uint8_t j = (uint8_t) (idx / 8);
    uint8_t k = (uint8_t) (idx % 8);

    rbyte[j] &= (uint8_t)~(1 << k);
}

uint8_t cron_get_bit(const uint8_t* rbyte, int idx) {
    uint8_t j = (uint8_t) (idx / 8);
    uint8_t k = (uint8_t) (idx % 8);

    if (rbyte[j] & (1 << k)) {
        return 1;
    } else {
        return 0;
    }
}

static void free_splitted(char** splitted, size_t len) {
    size_t i;
    if (!splitted) return;
    for (i = 0; i < len; i++) {
        if (splitted[i]) {
            cron_free(splitted[i]);
        }
    }
    cron_free(splitted);
}

static char* strdupl(const char* str, size_t len) {
    char* res = NULL;
    if (!str) return NULL;
    res = (char*) cron_malloc(len + 1);
    if (!res) return NULL;
    memcpy(res, str, len);
    res[len] = '\0';
    return res;
}

static int next_set_bit(uint8_t* bits, int max, int from_index, int* notfound) {
    int i;
    if (!bits) {
        *notfound = 1;
        return 0;
    }
    for (i = from_index; i < max; i++) {
        if (cron_get_bit(bits, i)) return i;
    }
    *notfound = 1;
    return 0;
}

static void push_to_fields_arr(int* arr, int fi) {
    int i;
    if (!arr || -1 == fi) {
        return;
    }
    for (i = 0; i < CRON_CF_ARR_LEN; i++) {
        if (arr[i] == fi) return;
    }
    for (i = 0; i < CRON_CF_ARR_LEN; i++) {
        if (-1 == arr[i]) {
            arr[i] = fi;
            return;
        }
    }
}

static int add_to_field(struct tm* calendar, int field, int val) {
    time_t res = 0;
    if (!calendar || -1 == field) {
        return 1;
    }
    switch (field) {
    case CRON_CF_SECOND:
        calendar->tm_sec = calendar->tm_sec + val;
        break;
    case CRON_CF_MINUTE:
        calendar->tm_min = calendar->tm_min + val;
        break;
    case CRON_CF_HOUR_OF_DAY:
        calendar->tm_hour = calendar->tm_hour + val;
        break;
    case CRON_CF_DAY_OF_WEEK: /* mkgmtime ignores this field */
    case CRON_CF_DAY_OF_MONTH:
        calendar->tm_mday = calendar->tm_mday + val;
        break;
    case CRON_CF_MONTH:
        calendar->tm_mon = calendar->tm_mon + val;
        break;
    case CRON_CF_YEAR:
        calendar->tm_year = calendar->tm_year + val;
        break;
    default:
        return 1; /* unknown field */
    }
    res = cron_mktime(calendar);
    if (CRON_INVALID_INSTANT == res) {
        return 1;
    }
    return 0;
}

/**
 * Reset the calendar setting all the fields provided to zero.
 */
static int reset_min(struct tm* calendar, int field) {
    time_t res = 0;
    if (!calendar || -1 == field) {
        return 1;
    }
    switch (field) {
    case CRON_CF_SECOND:
        calendar->tm_sec = 0;
        break;
    case CRON_CF_MINUTE:
        calendar->tm_min = 0;
        break;
    case CRON_CF_HOUR_OF_DAY:
        calendar->tm_hour = 0;
        break;
    case CRON_CF_DAY_OF_WEEK:
        calendar->tm_wday = 0;
        break;
    case CRON_CF_DAY_OF_MONTH:
        calendar->tm_mday = 1;
        break;
    case CRON_CF_MONTH:
        calendar->tm_mon = 0;
        break;
    case CRON_CF_YEAR:
        calendar->tm_year = 0;
        break;
    default:
        return 1; /* unknown field */
    }
    res = cron_mktime(calendar);
    if (CRON_INVALID_INSTANT == res) {
        return 1;
    }
    return 0;
}

static int reset_all_min(struct tm* calendar, int* fields) {
    int i;
    int res = 0;
    if (!calendar || !fields) {
        return 1;
    }
    for (i = 0; i < CRON_CF_ARR_LEN; i++) {
        if (-1 != fields[i]) {
            res = reset_min(calendar, fields[i]);
            if (0 != res) return res;
        }
    }
    return 0;
}

static int set_field(struct tm* calendar, int field, int val) {
    time_t res = 0;
    if (!calendar || -1 == field) {
        return 1;
    }
    switch (field) {
    case CRON_CF_SECOND:
        calendar->tm_sec = val;
        break;
    case CRON_CF_MINUTE:
        calendar->tm_min = val;
        break;
    case CRON_CF_HOUR_OF_DAY:
        calendar->tm_hour = val;
        break;
    case CRON_CF_DAY_OF_WEEK:
        calendar->tm_wday = val;
        break;
    case CRON_CF_DAY_OF_MONTH:
        calendar->tm_mday = val;
        break;
    case CRON_CF_MONTH:
        calendar->tm_mon = val;
        break;
    case CRON_CF_YEAR:
        calendar->tm_year = val;
        break;
    default:
        return 1; /* unknown field */
    }
    res = cron_mktime(calendar);
    if (CRON_INVALID_INSTANT == res) {
        return 1;
    }
    return 0;
}

/**
 * Search the bits provided for the next set bit after the value provided,
 * and reset the calendar.
 */
static int find_next(uint8_t* bits, int max, int value, struct tm* calendar, int field, int nextField, int* lower_orders, int* res_out) {
    int notfound = 0;
    int err = 0;
    int next_value = next_set_bit(bits, max, value, &notfound);
    /* roll over if needed */
    if (notfound) {
        err = add_to_field(calendar, nextField, 1);
        if (err) goto return_error;
        err = reset_min(calendar, field);
        if (err) goto return_error;
        notfound = 0;
        next_value = next_set_bit(bits, max, 0, &notfound);
    }
    if (notfound || next_value != value) {
        err = set_field(calendar, field, next_value);
        if (err) goto return_error;
        err = reset_all_min(calendar, lower_orders);
        if (err) goto return_error;
    }
    return next_value;

    return_error:
    *res_out = 1;
    return 0;
}

static int find_next_day(struct tm* calendar, uint8_t* days_of_month, int day_of_month, uint8_t* days_of_week, int day_of_week, int* resets, int* res_out) {
    int err;
    unsigned int count = 0;
    unsigned int max = 366;
    while ((!cron_get_bit(days_of_month, day_of_month) || !cron_get_bit(days_of_week, day_of_week)) && count++ < max) {
        err = add_to_field(calendar, CRON_CF_DAY_OF_MONTH, 1);

        if (err) goto return_error;
        day_of_month = calendar->tm_mday;
        day_of_week = calendar->tm_wday;
        reset_all_min(calendar, resets);
    }
    return day_of_month;

    return_error:
    *res_out = 1;
    return 0;
}

static int do_next(cron_expr* expr, struct tm* calendar, int dot) {
    int i;
    int res = 0;
    int resets[CRON_CF_ARR_LEN];
    int empty_list[CRON_CF_ARR_LEN];
    int second = 0;
    int update_second = 0;
    int minute = 0;
    int update_minute = 0;
    int hour = 0;
    int update_hour = 0;
    int day_of_week = 0;
    int day_of_month = 0;
    int update_day_of_month = 0;
    int month = 0;
    int update_month = 0;

    for (i = 0; i < CRON_CF_ARR_LEN; i++) {
        resets[i] = -1;
        empty_list[i] = -1;
    }

    second = calendar->tm_sec;
    update_second = find_next(expr->seconds, CRON_MAX_SECONDS, second, calendar, CRON_CF_SECOND, CRON_CF_MINUTE, empty_list, &res);
    if (0 != res) goto return_result;
    if (second == update_second) {
        push_to_fields_arr(resets, CRON_CF_SECOND);
    }

    minute = calendar->tm_min;
    update_minute = find_next(expr->minutes, CRON_MAX_MINUTES, minute, calendar, CRON_CF_MINUTE, CRON_CF_HOUR_OF_DAY, resets, &res);
    if (0 != res) goto return_result;
    if (minute == update_minute) {
        push_to_fields_arr(resets, CRON_CF_MINUTE);
    } else {
        res = do_next(expr, calendar, dot);
        if (0 != res) goto return_result;
    }

    hour = calendar->tm_hour;
    update_hour = find_next(expr->hours, CRON_MAX_HOURS, hour, calendar, CRON_CF_HOUR_OF_DAY, CRON_CF_DAY_OF_WEEK, resets, &res);
    if (0 != res) goto return_result;
    if (hour == update_hour) {
        push_to_fields_arr(resets, CRON_CF_HOUR_OF_DAY);
    } else {
        res = do_next(expr, calendar, dot);
        if (0 != res) goto return_result;
    }

    day_of_week = calendar->tm_wday;
    day_of_month = calendar->tm_mday;
    update_day_of_month = find_next_day(calendar, expr->days_of_month, day_of_month, expr->days_of_week, day_of_week, resets, &res);
    if (0 != res) goto return_result;
    if (day_of_month == update_day_of_month) {
        push_to_fields_arr(resets, CRON_CF_DAY_OF_MONTH);
    } else {
        res = do_next(expr, calendar, dot);
        if (0 != res) goto return_result;
    }

    month = calendar->tm_mon; /*day already adds one if no day in same month is found*/
    update_month = find_next(expr->months, CRON_MAX_MONTHS, month, calendar, CRON_CF_MONTH, CRON_CF_YEAR, resets, &res);
    if (0 != res) goto return_result;
    if (month != update_month) {
        if (calendar->tm_year - dot > 4) {
            res = -1;
            goto return_result;
        }
        res = do_next(expr, calendar, dot);
        if (0 != res) goto return_result;
    }
    goto return_result;

    return_result:
    return res;
}

static int to_upper(char* str) {
    int i = 0;
    if (!str) return 1;
    for (i = 0; '\0' != str[i]; i++) {
        int c = (int)str[i];
        str[i] = (char) toupper(c);
    }
    return 0;
}

static void strreverse(char* begin, char* end)
{
    char aux;
    while (end > begin)
        aux = *end, *end-- = *begin, *begin++ = aux;
}

/* included from https://github.com/client9/stringencoders/blob/master/src/modp_numtoa.c */
size_t to_string(size_t value, char* str)
{
    char* wstr = str;
    /* Conversion. Number is reversed. */
    do
        *wstr++ = (char)(48 + (value % 10));
    while (value /= 10);
    *wstr = '\0';
    /* Reverse string */
    strreverse(str, wstr - 1);
    return (size_t)(wstr - str);
}

static char* str_replace(char *orig, const char *rep, const char *with) {
    char *result; /* the return string */
    char *ins; /* the next insert point */
    char *tmp; /* varies */
    size_t len_rep; /* length of rep */
    size_t len_with; /* length of with */
    size_t len_front; /* distance between rep and end of last rep */
    size_t count; /* number of replacements */
    if (!orig) return NULL;
    if (!rep) rep = "";
    if (!with) with = "";
    len_rep = strlen(rep);
    len_with = strlen(with);

    ins = orig;
    for (count = 0; NULL != (tmp = strstr(ins, rep)); ++count) {
        ins = tmp + len_rep;
    }

    /* first time through the loop, all the variable are set correctly
     from here on,
     tmp points to the end of the result string
     ins points to the next occurrence of rep in orig
     orig points to the remainder of orig after "end of rep"
     */
    tmp = result = (char*) cron_malloc(strlen(orig) + (len_with - len_rep) * count + 1);
    if (!result) return NULL;

    while (count--) {
        ins = strstr(orig, rep);
        len_front = (size_t)(ins - orig);
        tmp = strncpy(tmp, orig, len_front) + len_front;
        tmp = strcpy(tmp, with) + len_with;
        orig += len_front + len_rep; /* move to next "end of rep" */
    }
    strcpy(tmp, orig);
    return result;
}

static int parse_uint(const char* str, int* errcode) {
    char* endptr = NULL;
    long int l;
    errno = 0;
    l = strtol(str, &endptr, 10);
    if (errno == ERANGE || *endptr != '\0' || l < 0 || l > INT_MAX) {
        *errcode = 1;
        return 0;
    } else {
        *errcode = 0;
        return (int) l;
    }
}

static char** split_str(const char* str, char del, size_t* len_out) {
    size_t i = 0;
    size_t stlen = 0;
    size_t len = 0;
    int accum = 0;
    char* buf = NULL;
    char** res = NULL;
    size_t bi = 0;
    size_t ri = 0;
    char* tmp = NULL;

    if (!str) goto return_error;
    for (i = 0; '\0' != str[i]; i++) {
        stlen += 1;
        if (stlen >= CRON_MAX_STR_LEN_TO_SPLIT) goto return_error;
    }

    for (i = 0; i < stlen; i++) {
        int c = (unsigned char)str[i];
        if (del == str[i]) {
            if (accum > 0) {
                len += 1;
                accum = 0;
            }
        } else if (!isspace(c)) {
            accum += 1;
        }
    }
    /* tail */
    if (accum > 0) {
        len += 1;
    }
    if (0 == len) return NULL;

    buf = (char*) cron_malloc(stlen + 1);
    if (!buf) goto return_error;
    memset(buf, 0, stlen + 1);
    res = (char**) cron_malloc(len * sizeof(char*));
    if (!res) goto return_error;
    memset(res, 0, len * sizeof(char*));

    for (i = 0; i < stlen; i++) {
        int c = (unsigned char)str[i];
        if (del == str[i]) {
            if (bi > 0) {
                if (ri >= len)  goto return_error;
                tmp = strdupl(buf, bi);
                if (!tmp) goto return_error;
                res[ri++] = tmp;
                memset(buf, 0, stlen + 1);
                bi = 0;
            }
        } else if (!isspace(c)) {
            buf[bi++] = str[i];
        }
    }
    /* tail */
    if (bi > 0) {
        if (ri >= len)  goto return_error;
        tmp = strdupl(buf, bi);
        if (!tmp) goto return_error;
        res[ri++] = tmp;
    }
    cron_free(buf);
    *len_out = len;
    return res;

    return_error:
    if (buf) {
        cron_free(buf);
    }
    free_splitted(res, len);
    *len_out = 0;
    return NULL;
}

static char* replace_ordinals(char* value, const char* const * arr, size_t arr_len) {
    size_t i;
    char* cur = value;
    char* res = NULL;
    int first = 1;
    char strnum[CRON_SIZE_STRING_MAX_LEN + 1];

    for (i = 0; i < arr_len; i++) {
        to_string(i, strnum);

        res = str_replace(cur, arr[i], strnum);
        if (!first) {
            cron_free(cur);
        }
        if (!res) {
            return NULL;
        }
        cur = res;
        if (first) {
            first = 0;
        }
    }
    return res;
}

static int has_char(const char* str, char ch) {
    size_t i = 0;
    if (!str) return 0;
    for (i = 0; str[i] != '\0'; i++) {
        if (str[i] == ch) return 1;
    }
    return 0;
}

static void get_range(const char* field, int min, int max, int* res, const char** error) {
    char** parts = NULL;
    size_t len = 0;
    int err = 0;
    int val;
    if (!res) {
        *error = "NULL";
        goto return_error;
    }

    res[0] = 0;
    res[1] = 0;
    if (1 == strlen(field) && '*' == field[0]) {
        res[0] = min;
        res[1] = max - 1;
    } else if (!has_char(field, '-')) {
        err = 0;
        val = parse_uint(field, &err);
        if (err) {
            *error = "Unsigned integer parse error 1";
            goto return_error;
        }

        res[0] = val;
        res[1] = val;
    } else {
        parts = split_str(field, '-', &len);
        if (2 != len) {
            *error = "Specified range requires two fields";
            goto return_error;
        }
        err = 0;
        res[0] = parse_uint(parts[0], &err);
        if (err) {
            *error = "Unsigned integer parse error 2";
            goto return_error;
        }
        res[1] = parse_uint(parts[1], &err);
        if (err) {
            *error = "Unsigned integer parse error 3";
            goto return_error;
        }
    }
    if (res[0] >= max || res[1] >= max) {
        *error = "Specified range exceeds maximum";
        goto return_error;
    }
    if (res[0] < min || res[1] < min) {
        *error = "Specified range is less than minimum";
        goto return_error;
    }
    if (res[0] > res[1]) {
        *error = "Specified range start exceeds range end";
        goto return_error;
    }

    free_splitted(parts, len);
    *error = NULL;
    return;

    return_error:
    free_splitted(parts, len);
}

static void set_number_hits(const char* value, uint8_t* target, int min, int max, const char** error) {
    size_t i = 0;
    int i1 = 0;
    size_t len = 0;
    int range[2];
    int err = 0;
    int delta;

    char** fields = split_str(value, ',', &len);
    if (!fields) {
        *error = "Comma split error";
        goto return_result;
    }

    for (i = 0; i < len; i++) {
        if (!has_char(fields[i], '/')) {
            /* Not an incrementer so it must be a range (possibly empty) */

            get_range(fields[i], min, max, range, error);

            if (*error) {
                goto return_result;
            }

            for (i1 = range[0]; i1 <= range[1]; i1++) {
                cron_set_bit(target, i1);

            }
        } else {
            size_t len2 = 0;
            char** split = split_str(fields[i], '/', &len2);
            if (2 != len2) {
                *error = "Incrementer must have two fields";
                free_splitted(split, len2);
                goto return_result;
            }
            get_range(split[0], min, max, range, error);
            if (*error) {
                free_splitted(split, len2);
                goto return_result;
            }
            if (!has_char(split[0], '-')) {
                range[1] = max - 1;
            }
            delta = parse_uint(split[1], &err);
            if (err) {
                *error = "Unsigned integer parse error 4";
                free_splitted(split, len2);
                goto return_result;
            }
            if (0 == delta) {
                *error = "Incrementer may not be zero";
                free_splitted(split, len2);
                goto return_result;
            }
            for (i1 = range[0]; i1 <= range[1]; i1 += delta) {
                cron_set_bit(target, i1);
            }
            free_splitted(split, len2);
        }
    }
    goto return_result;

    return_result:
    free_splitted(fields, len);

}

static void set_months(char* value, uint8_t* targ, const char** error) {
    int i;
    int max = 12;

    char* replaced = NULL;

    to_upper(value);
    replaced = replace_ordinals(value, MONTHS_ARR, CRON_MONTHS_ARR_LEN);
    if (!replaced) {
        *error = "Invalid month format";
        return;
    }
    set_number_hits(replaced, targ, 1, max + 1, error);
    cron_free(replaced);

    /* ... and then rotate it to the front of the months */
    for (i = 1; i <= max; i++) {
        if (cron_get_bit(targ, i)) {
            cron_set_bit(targ, i - 1);
            cron_del_bit(targ, i);
        }
    }
}

static void set_days_of_week(char* field, uint8_t* targ, const char** error) {
    const int max = 7;
    char* replaced = NULL;

    if (1 == strlen(field) && '?' == field[0]) {
        field[0] = '*';
    }
    to_upper(field);
    replaced = replace_ordinals(field, DAYS_ARR, CRON_DAYS_ARR_LEN);
    if (!replaced) {
        *error = "Invalid day format";
        return;
    }
    set_number_hits(replaced, targ, 0, max + 1, error);
    cron_free(replaced);
    if (cron_get_bit(targ, 7)) {
        /* Sunday can be represented as 0 or 7*/
        cron_set_bit(targ, 0);
        cron_del_bit(targ, 7);
    }
}

static void set_days_of_month(char* field, uint8_t* targ, const char** error) {
    /* Days of month start with 1 (in Cron and Calendar) so add one */
    if (1 == strlen(field) && '?' == field[0]) {
        field[0] = '*';
    }
    set_number_hits(field, targ, 1, CRON_MAX_DAYS_OF_MONTH, error);
}

void cron_parse_expr(const char* expression, cron_expr* target, const char** error) {
    const char* err_local;
    size_t len = 0;
    char** fields = NULL;
    if (!error) {
        error = &err_local;
    }
    *error = NULL;
    if (!expression) {
        *error = "Invalid NULL expression";
        goto return_res;
    }
    if (!target) {
        *error = "Invalid NULL target";
        goto return_res;
    }

    fields = split_str(expression, ' ', &len);
    if (len != 6) {
        *error = "Invalid number of fields, expression must consist of 6 fields";
        goto return_res;
    }
    memset(target, 0, sizeof(*target));
    set_number_hits(fields[0], target->seconds, 0, 60, error);
    if (*error) goto return_res;
    set_number_hits(fields[1], target->minutes, 0, 60, error);
    if (*error) goto return_res;
    set_number_hits(fields[2], target->hours, 0, 24, error);
    if (*error) goto return_res;
    set_days_of_month(fields[3], target->days_of_month, error);
    if (*error) goto return_res;
    set_months(fields[4], target->months, error);
    if (*error) goto return_res;
    set_days_of_week(fields[5], target->days_of_week, error);
    if (*error) goto return_res;

    goto return_res;

    return_res:
    free_splitted(fields, len);
}

time_t cron_next(cron_expr* expr, time_t date) {
    /*
     The plan:

     1 Round up to the next whole second

     2 If seconds match move on, otherwise find the next match:
     2.1 If next match is in the next minute then roll forwards

     3 If minute matches move on, otherwise find the next match
     3.1 If next match is in the next hour then roll forwards
     3.2 Reset the seconds and go to 2

     4 If hour matches move on, otherwise find the next match
     4.1 If next match is in the next day then roll forwards,
     4.2 Reset the minutes and seconds and go to 2

     ...
     */
    struct tm calval;
    struct tm* calendar;
    time_t original;
    int res;
    time_t calculated;

    if (!expr) return CRON_INVALID_INSTANT;
    memset(&calval, 0, sizeof(struct tm));
    calendar = cron_time(&date, &calval);
    if (!calendar) return CRON_INVALID_INSTANT;
    original = cron_mktime(calendar);
    if (CRON_INVALID_INSTANT == original) return CRON_INVALID_INSTANT;

    res = do_next(expr, calendar, calendar->tm_year);
    if (0 != res) return CRON_INVALID_INSTANT;

    calculated = cron_mktime(calendar);
    if (CRON_INVALID_INSTANT == calculated) return CRON_INVALID_INSTANT;
    if (calculated == original) {
        /* We arrived at the original timestamp - round up to the next whole second and try again... */
        res = add_to_field(calendar, CRON_CF_SECOND, 1);
        if (0 != res) return CRON_INVALID_INSTANT;
        res = do_next(expr, calendar, calendar->tm_year);
        if (0 != res) return CRON_INVALID_INSTANT;
    }

    return cron_mktime(calendar);
}


/* https://github.com/staticlibs/ccronexpr/pull/8 */

static int prev_set_bit(uint8_t* bits, int from_index, int to_index, int* notfound) {
    int i;
    if (!bits) {
        *notfound = 1;
        return 0;
    }
    for (i = from_index; i >= to_index; i--) {
        if (cron_get_bit(bits, i)) return i;
    }
    *notfound = 1;
    return 0;
}

static int last_day_of_month(int month, int year) {
    struct tm cal;
    time_t t;
    memset(&cal,0,sizeof(cal));
    cal.tm_sec=0;
    cal.tm_min=0;
    cal.tm_hour=0;
    cal.tm_mon = month+1;
    cal.tm_mday = 0;
    cal.tm_year=year;
    t=mktime(&cal);
    return gmtime(&t)->tm_mday;
}

/**
 * Reset the calendar setting all the fields provided to zero.
 */
static int reset_max(struct tm* calendar, int field) {
    time_t res = 0;
    if (!calendar || -1 == field) {
        return 1;
    }
    switch (field) {
    case CRON_CF_SECOND:
        calendar->tm_sec = 59;
        break;
    case CRON_CF_MINUTE:
        calendar->tm_min = 59;
        break;
    case CRON_CF_HOUR_OF_DAY:
        calendar->tm_hour = 23;
        break;
    case CRON_CF_DAY_OF_WEEK:
        calendar->tm_wday = 6;
        break;
    case CRON_CF_DAY_OF_MONTH:
        calendar->tm_mday = last_day_of_month(calendar->tm_mon, calendar->tm_year);
        break;
    case CRON_CF_MONTH:
        calendar->tm_mon = 11;
        break;
    case CRON_CF_YEAR:
        /* I don't think this is supposed to happen ... */
        fprintf(stderr, "reset CRON_CF_YEAR\n");
        break;
    default:
        return 1; /* unknown field */
    }
    res = cron_mktime(calendar);
    if (CRON_INVALID_INSTANT == res) {
        return 1;
    }
    return 0;
}

static int reset_all_max(struct tm* calendar, int* fields) {
    int i;
    int res = 0;
    if (!calendar || !fields) {
        return 1;
    }
    for (i = 0; i < CRON_CF_ARR_LEN; i++) {
        if (-1 != fields[i]) {
            res = reset_max(calendar, fields[i]);
            if (0 != res) return res;
        }
    }
    return 0;
}

/**
 * Search the bits provided for the next set bit after the value provided,
 * and reset the calendar.
 */
static int find_prev(uint8_t* bits, int max, int value, struct tm* calendar, int field, int nextField, int* lower_orders, int* res_out) {
    int notfound = 0;
    int err = 0;
    int next_value = prev_set_bit(bits, value, 0, &notfound);
    /* roll under if needed */
    if (notfound) {
        err = add_to_field(calendar, nextField, -1);
        if (err) goto return_error;
        err = reset_max(calendar, field);
        if (err) goto return_error;
        notfound = 0;
        next_value = prev_set_bit(bits, max - 1, value, &notfound);
    }
    if (notfound || next_value != value) {
        err = set_field(calendar, field, next_value);
        if (err) goto return_error;
        err = reset_all_max(calendar, lower_orders);
        if (err) goto return_error;
    }
    return next_value;

    return_error:
    *res_out = 1;
    return 0;
}

static int find_prev_day(struct tm* calendar, uint8_t* days_of_month, int day_of_month, uint8_t* days_of_week, int day_of_week, int* resets, int* res_out) {
    int err;
    unsigned int count = 0;
    unsigned int max = 366;
    while ((!cron_get_bit(days_of_month, day_of_month) || !cron_get_bit(days_of_week, day_of_week)) && count++ < max) {
        err = add_to_field(calendar, CRON_CF_DAY_OF_MONTH, -1);

        if (err) goto return_error;
        day_of_month = calendar->tm_mday;
        day_of_week = calendar->tm_wday;
        reset_all_max(calendar, resets);
    }
    return day_of_month;

    return_error:
    *res_out = 1;
    return 0;
}

static int do_prev(cron_expr* expr, struct tm* calendar, int dot) {
    int i;
    int res = 0;
    int resets[CRON_CF_ARR_LEN];
    int empty_list[CRON_CF_ARR_LEN];
    int second = 0;
    int update_second = 0;
    int minute = 0;
    int update_minute = 0;
    int hour = 0;
    int update_hour = 0;
    int day_of_week = 0;
    int day_of_month = 0;
    int update_day_of_month = 0;
    int month = 0;
    int update_month = 0;

    for (i = 0; i < CRON_CF_ARR_LEN; i++) {
        resets[i] = -1;
        empty_list[i] = -1;
    }

    second = calendar->tm_sec;
    update_second = find_prev(expr->seconds, CRON_MAX_SECONDS, second, calendar, CRON_CF_SECOND, CRON_CF_MINUTE, empty_list, &res);
    if (0 != res) goto return_result;
    if (second == update_second) {
        push_to_fields_arr(resets, CRON_CF_SECOND);
    }

    minute = calendar->tm_min;
    update_minute = find_prev(expr->minutes, CRON_MAX_MINUTES, minute, calendar, CRON_CF_MINUTE, CRON_CF_HOUR_OF_DAY, resets, &res);
    if (0 != res) goto return_result;
    if (minute == update_minute) {
        push_to_fields_arr(resets, CRON_CF_MINUTE);
    } else {
        res = do_prev(expr, calendar, dot);
        if (0 != res) goto return_result;
    }

    hour = calendar->tm_hour;
    update_hour = find_prev(expr->hours, CRON_MAX_HOURS, hour, calendar, CRON_CF_HOUR_OF_DAY, CRON_CF_DAY_OF_WEEK, resets, &res);
    if (0 != res) goto return_result;
    if (hour == update_hour) {
        push_to_fields_arr(resets, CRON_CF_HOUR_OF_DAY);
    } else {
        res = do_prev(expr, calendar, dot);
        if (0 != res) goto return_result;
    }

    day_of_week = calendar->tm_wday;
    day_of_month = calendar->tm_mday;
    update_day_of_month = find_prev_day(calendar, expr->days_of_month, day_of_month, expr->days_of_week, day_of_week, resets, &res);
    if (0 != res) goto return_result;
    if (day_of_month == update_day_of_month) {
        push_to_fields_arr(resets, CRON_CF_DAY_OF_MONTH);
    } else {
        res = do_prev(expr, calendar, dot);
        if (0 != res) goto return_result;
    }

    month = calendar->tm_mon; /*day already adds one if no day in same month is found*/
    update_month = find_prev(expr->months, CRON_MAX_MONTHS, month, calendar, CRON_CF_MONTH, CRON_CF_YEAR, resets, &res);
    if (0 != res) goto return_result;
    if (month != update_month) {
        if (dot - calendar->tm_year > CRON_MAX_YEARS_DIFF) {
            res = -1;
            goto return_result;
        }
        res = do_prev(expr, calendar, dot);
        if (0 != res) goto return_result;
    }
    goto return_result;

    return_result:
    return res;
}

time_t cron_prev(cron_expr* expr, time_t date) {
    /*
     The plan:

     1 Round down to a whole second

     2 If seconds match move on, otherwise find the next match:
     2.1 If next match is in the next minute then roll forwards

     3 If minute matches move on, otherwise find the next match
     3.1 If next match is in the next hour then roll forwards
     3.2 Reset the seconds and go to 2

     4 If hour matches move on, otherwise find the next match
     4.1 If next match is in the next day then roll forwards,
     4.2 Reset the minutes and seconds and go to 2

     ...
     */
    struct tm calval;
    struct tm* calendar;
    int res;
    time_t original;
    time_t calculated;
    if (!expr) return CRON_INVALID_INSTANT;
    memset(&calval, 0, sizeof(struct tm));
    calendar = cron_time(&date, &calval);
    if (!calendar) return CRON_INVALID_INSTANT;
    original = cron_mktime(calendar);
    if (CRON_INVALID_INSTANT == original) return CRON_INVALID_INSTANT;

    /* calculate the previous occurrence */
    res = do_prev(expr, calendar, calendar->tm_year);
    if (0 != res) return CRON_INVALID_INSTANT;

    /* check for a match, try from the next second if one wasn't found */
    calculated = cron_mktime(calendar);
    if (CRON_INVALID_INSTANT == calculated) return CRON_INVALID_INSTANT;
    if (calculated == original) {
        /* We arrived at the original timestamp - round up to the next whole second and try again... */
        res = add_to_field(calendar, CRON_CF_SECOND, -1);
        if (0 != res) return CRON_INVALID_INSTANT;
        res = do_prev(expr, calendar, calendar->tm_year);
        if (0 != res) return CRON_INVALID_INSTANT;
    }

    return cron_mktime(calendar);
}
