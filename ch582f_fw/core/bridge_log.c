#include "bridge_log.h"
#include "bridge_port.h"

#if BRIDGE_LOG_ENABLE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_level = BRIDGE_LOG_LEVEL;

static const char *level_name(uint8_t lv)
{
    switch (lv) {
        case BRIDGE_LOG_ERROR: return "ERR ";
        case BRIDGE_LOG_WARN:  return "WARN";
        case BRIDGE_LOG_INFO:  return "INFO";
        case BRIDGE_LOG_DEBUG: return "DBG ";
        default:               return "?   ";
    }
}
#endif /* BRIDGE_LOG_ENABLE */

void bridge_log_init(uint32_t baudrate)
{
    (void)baudrate; /* UART0 的实际初始化由 port 层在首次写入时完成 */
#if BRIDGE_LOG_ENABLE
    s_level = BRIDGE_LOG_LEVEL;
    LOG_I("log ", "enabled, level=%u", (unsigned)s_level);
#endif
}

void bridge_log_set_level(uint8_t level)
{
#if BRIDGE_LOG_ENABLE
    if (level > BRIDGE_LOG_DEBUG) {
        level = BRIDGE_LOG_DEBUG;
    }
    s_level = level;
#else
    (void)level;
#endif
}

uint8_t bridge_log_get_level(void)
{
#if BRIDGE_LOG_ENABLE
    return s_level;
#else
    return BRIDGE_LOG_OFF;
#endif
}

void bridge_log_printf(uint8_t level, const char *module, const char *fmt, ...)
{
#if BRIDGE_LOG_ENABLE
    if (level == BRIDGE_LOG_OFF || level > s_level) {
        return;
    }

    char buf[BRIDGE_LOG_BUF_SIZE];
    int  n = snprintf(buf, sizeof(buf), "[%08lu][%s][%s] ",
                      (unsigned long)port_millis(), level_name(level),
                      (module != 0) ? module : "?");
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)(sizeof(buf) - 1u);
    }

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }

    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof(buf) - 3u) {
        len = sizeof(buf) - 3u;
    }
    buf[len++] = '\r';
    buf[len++] = '\n';

    port_log_write(buf, (uint16_t)len);
#else
    (void)level;
    (void)module;
    (void)fmt;
#endif
}
