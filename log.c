#include <stdio.h>
#include <stdint.h>
#include <syslog.h>

#include "log.h"

static int log_level = LOG_LEVEL_DEBUG;
static FILE *out_stream = NULL;

static int using_syslog = 0;

void log_enable_syslog(char *ident) {
  if (!using_syslog) {
    using_syslog = 1;
    openlog(ident, LOG_PERROR | LOG_CONS | LOG_PID, LOG_USER);
  }
}

void log_disable_syslog() {
  if (using_syslog) {
    using_syslog = 0;
    closelog();
  }
}

void log_set_level(int level) {
  log_level = level;
}

int log_get_level() {
  return log_level;
}

void log_set_stream(FILE *stream) {
  out_stream = stream;
}

void log_hex(int msg_log_level, uint8_t *data, int len) {
  int i;

  if (msg_log_level < log_level) {
    return;
  }

  for (i = 0; i < len; i++) {
    log_msg_level(msg_log_level, "%02x", data[i]);
  }
}

void log_msg(int msg_log_level, const char *format, const va_list args) {
  if (out_stream == NULL) {
    out_stream = stdout;
  }

  if (msg_log_level >= log_level) {
    if (using_syslog) {
      switch (msg_log_level) {
        case LOG_LEVEL_DEBUG:
          vsyslog(LOG_DEBUG, format, args);
          break;
        case LOG_LEVEL_INFO:
          vsyslog(LOG_INFO, format, args);
          break;
        case LOG_LEVEL_WARN:
          vsyslog(LOG_WARNING, format, args);
          break;
        case LOG_LEVEL_ERROR:
          vsyslog(LOG_ERR, format, args);
          break;
        case LOG_LEVEL_FATAL:
          vsyslog(LOG_CRIT, format, args);
          break;
        default:
          syslog(LOG_ALERT, "unknown log level: %d\n--- message begin ---\n", msg_log_level);
          vsyslog(LOG_ALERT, format, args);
          syslog(LOG_ALERT, "--- message end ---\n");
          break;
      }
    } else {
      vfprintf(out_stream, format, args);
    }
  }
}

void log_msg_level(int msg_log_level, const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_msg(msg_log_level, format, args);
  va_end(args);
}

void log_debug(const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_msg(LOG_LEVEL_DEBUG, format, args);
  va_end(args);
}

void log_info(const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_msg(LOG_LEVEL_INFO, format, args);
  va_end(args);
}

void log_warn(const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_msg(LOG_LEVEL_WARN, format, args);
  va_end(args);
}

void log_error(const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_msg(LOG_LEVEL_ERROR, format, args);
  va_end(args);
}

void log_fatal(const char *format, ...) {
  va_list args;
  va_start(args, format);
  log_msg(LOG_LEVEL_FATAL, format, args);
  va_end(args);
}
