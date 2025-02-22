// This file Copyright © 2010-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>

#include <event2/buffer.h>

#include "transmission.h"
#include "file.h"
#include "log.h"
#include "tr-assert.h"
#include "utils.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

using namespace std::literals;

static tr_log_level tr_message_level = TR_LOG_ERROR;
static bool myQueueEnabled = false;
static tr_log_message* myQueue = nullptr;
static tr_log_message** myQueueTail = &myQueue;
static int myQueueLength = 0;

/***
****
***/

tr_log_level tr_logGetLevel()
{
    return tr_message_level;
}

/***
****
***/

static std::recursive_mutex message_mutex_;

static tr_sys_file_t tr_logGetFile()
{
    static bool initialized = false;
    static tr_sys_file_t file = TR_BAD_SYS_FILE;

    if (!initialized)
    {
        switch (tr_env_get_int("TR_DEBUG_FD", 0))
        {
        case 1:
            file = tr_sys_file_get_std(TR_STD_SYS_FILE_OUT, nullptr);
            break;

        case 2:
            file = tr_sys_file_get_std(TR_STD_SYS_FILE_ERR, nullptr);
            break;

        default:
            file = TR_BAD_SYS_FILE;
            break;
        }

        initialized = true;
    }

    return file;
}

void tr_logSetLevel(tr_log_level level)
{
    tr_message_level = level;
}

void tr_logSetQueueEnabled(bool isEnabled)
{
    myQueueEnabled = isEnabled;
}

bool tr_logGetQueueEnabled()
{
    return myQueueEnabled;
}

tr_log_message* tr_logGetQueue()
{
    auto const lock = std::lock_guard(message_mutex_);

    auto* const ret = myQueue;
    myQueue = nullptr;
    myQueueTail = &myQueue;
    myQueueLength = 0;

    return ret;
}

void tr_logFreeQueue(tr_log_message* list)
{
    while (list != nullptr)
    {
        tr_log_message* next = list->next;
        tr_free(list->message);
        tr_free(list->name);
        tr_free(list);
        list = next;
    }
}

/**
***
**/

char* tr_logGetTimeStr(char* buf, size_t buflen)
{
    auto const tv = tr_gettimeofday();
    time_t const seconds = tv.tv_sec;
    auto const milliseconds = int(tv.tv_usec / 1000);
    char msec_str[8];
    tr_snprintf(msec_str, sizeof msec_str, "%03d", milliseconds);

    struct tm now_tm;
    tr_localtime_r(&seconds, &now_tm);
    char date_str[32];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", &now_tm);

    tr_snprintf(buf, buflen, "%s.%s", date_str, msec_str);
    return buf;
}

/***
****
***/

void logAddImpl(
    [[maybe_unused]] char const* file,
    [[maybe_unused]] int line,
    tr_log_level level,
    [[maybe_unused]] std::string_view name,
    std::string_view msg)
{
    if (std::empty(msg))
    {
        return;
    }

    auto const lock = std::lock_guard(message_mutex_);
#ifdef _WIN32

    OutputDebugStringA(tr_strvJoin(msg, "\r\n").c_str());

#elif defined(__ANDROID__)

    int prio;

    switch (level)
    {
    case TR_LOG_CRITICAL:
        prio = ANDROID_LOG_FATAL;
        break;
    case TR_LOG_ERROR:
        prio = ANDROID_LOG_ERROR;
        break;
    case TR_LOG_WARN:
        prio = ANDROID_LOG_WARN;
        break;
    case TR_LOG_INFO:
        prio = ANDROID_LOG_INFO;
        break;
    case TR_LOG_DEBUG:
        prio = ANDROID_LOG_DEBUG;
        break;
    case TR_LOG_TRACE:
        prio = ANDROID_LOG_VERBOSE;
    }

#ifdef NDEBUG
    __android_log_print(prio, "transmission", "%" TR_PRIsv, TR_PRIsv_ARG(msg));
#else
    __android_log_print(prio, "transmission", "[%s:%d] %" TR_PRIsv, file, line, TR_PRIsv_ARG(msg));
#endif

#else

    if (tr_logGetQueueEnabled())
    {
        auto* const newmsg = tr_new0(tr_log_message, 1);
        newmsg->level = level;
        newmsg->when = tr_time();
        newmsg->message = tr_strvDup(msg);
        newmsg->file = file;
        newmsg->line = line;
        newmsg->name = tr_strvDup(name);

        *myQueueTail = newmsg;
        myQueueTail = &newmsg->next;
        ++myQueueLength;

        if (myQueueLength > TR_LOG_MAX_QUEUE_LENGTH)
        {
            tr_log_message* old = myQueue;
            myQueue = old->next;
            old->next = nullptr;
            tr_logFreeQueue(old);
            --myQueueLength;
            TR_ASSERT(myQueueLength == TR_LOG_MAX_QUEUE_LENGTH);
        }
    }
    else
    {
        char timestr[64];

        tr_sys_file_t fp = tr_logGetFile();

        if (fp == TR_BAD_SYS_FILE)
        {
            fp = tr_sys_file_get_std(TR_STD_SYS_FILE_ERR, nullptr);
        }

        tr_logGetTimeStr(timestr, sizeof(timestr));

        auto const out = !std::empty(name) ? tr_strvJoin("["sv, timestr, "] "sv, name, ": "sv, msg) :
                                             tr_strvJoin("["sv, timestr, "] "sv, msg);
        tr_sys_file_write_line(fp, out, nullptr);
        tr_sys_file_flush(fp, nullptr);
    }
#endif
}

void tr_logAddMessage(char const* file, int line, tr_log_level level, std::string_view name, std::string_view msg)
{
    // message logging shouldn't affect errno
    int const err = errno;

    // skip unwanted messages
    if (!tr_logLevelIsActive(level))
    {
        errno = err;
        return;
    }

    auto const lock = std::lock_guard(message_mutex_);

    // don't log the same warning ad infinitum.
    // it's not useful after some point.
    bool last_one = false;
    if (level == TR_LOG_CRITICAL || level == TR_LOG_ERROR || level == TR_LOG_WARN)
    {
        static auto constexpr MaxRepeat = size_t{ 30 };
        static auto counts = new std::map<std::pair<char const*, int>, size_t>{};

        auto& count = (*counts)[std::make_pair(file, line)];
        ++count;
        last_one = count == MaxRepeat;
        if (count > MaxRepeat)
        {
            errno = err;
            return;
        }
    }

    // log the messages
    logAddImpl(file, line, level, name, msg);
    if (last_one)
    {
        logAddImpl(file, line, level, "", _("Too many messages like this! I won't log this message anymore this session."));
    }

    errno = err;
}

void tr_logAddMessage(
    [[maybe_unused]] char const* file,
    [[maybe_unused]] int line,
    tr_log_level level,
    [[maybe_unused]] std::string_view name,
    char const* fmt,
    ...)
{
    // message logging shouldn't affect errno
    int const err = errno;

    // build the message
    auto buf = std::array<char, 2048>{};
    va_list ap;
    va_start(ap, fmt);
    int const buf_len = evutil_vsnprintf(std::data(buf), std::size(buf), fmt, ap);
    va_end(ap);
    if (buf_len <= 0)
    {
        errno = err;
        return;
    }

    tr_logAddMessage(file, line, level, name, std::string_view{ std::data(buf) });
}
