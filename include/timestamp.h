#include <time.h>
#include <stdint.h>

#ifndef __MMAKER_TIMESTAMP_H__
#define __MMAKER_TIMESTAMP_H__

/* _ntime per second */
#define TIMESTAMP_ONE_SECOND  (10000000ULL)

#ifdef _WIN32
#include <Windows.h>

#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
#define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64
#else
#define DELTA_EPOCH_IN_MICROSECS  11644473600000000ULL
#endif

#ifndef _TIMEZONE_DEFINED 
struct timezone
{
    int  tz_minuteswest; /* minutes W of Greenwich */
    int  tz_dsttime;     /* type of dst correction */
};
#endif

/* return 100ns of system clock */
static inline uint64_t _ntime()
{
    FILETIME ft; uint64_t tmpres = 0;
    GetSystemTimeAsFileTime(&ft);

    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;

    return (tmpres);
}

/* return us/ms since unix epoch */
#define _utime() (((_ntime() / 10ULL) - DELTA_EPOCH_IN_MICROSECS)          )
#define _mtime() (((_ntime() / 10ULL) - DELTA_EPOCH_IN_MICROSECS) / 1000ULL)

int gettimeofday(struct timeval* tv, struct timezone* tz)
{
    static int tzflag = 0;

    if (NULL != tv) {
        uint64_t tmpres = _utime();

        tv->tv_sec  = (long)(tmpres / 1000000ULL);
        tv->tv_usec = (long)(tmpres % 1000000ULL);
    }

    if (NULL != tz) {
        if (!tzflag) {
            _tzset();
            tzflag++;
        }
        tz->tz_minuteswest = _timezone / 60;
        tz->tz_dsttime = _daylight;
    }

    return 0;
}

/* Windows sleep in 100ns units */
static inline bool nanosleep(HANDLE h_timer, LONGLONG time_in_100ns)
{
    LARGE_INTEGER li;   /* Time defintion */
    li.QuadPart = -time_in_100ns;
    if (!SetWaitableTimer(h_timer, &li, 0, NULL, NULL, FALSE)) {
        return false;
    }

    /* Start & wait for timer */
    WaitForSingleObject(h_timer, INFINITE);

    /* Slept without problems */
    return true;
}

#else // !_WIN32

#include <sys/time.h>

static inline uint64_t _utime()
{
    struct timeval tv; gettimeofday(&tv, NULL);
    uint64_t millisecondsSinceEpoch = (uint64_t)(tv.tv_sec) * 1000000ULL + (uint64_t)(tv.tv_usec);
    return (millisecondsSinceEpoch);
}

#define _mtime() (_utime() / 1000ULL)
#define _ntime() (_utime() * 10ULL  )

#endif

#endif
