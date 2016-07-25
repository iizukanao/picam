#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>
#include <stdarg.h>

enum {
  LOG_LEVEL_DEBUG,
  LOG_LEVEL_INFO,
  LOG_LEVEL_WARN,
  LOG_LEVEL_ERROR,
  LOG_LEVEL_FATAL,
  LOG_LEVEL_OFF,
};

void log_set_level(int level);
int  log_get_level();
void log_set_stream(FILE *stream);
void log_hex(int msg_log_level, uint8_t *data, int len);
void log_msg(int level, const char *format, const va_list args);
void log_msg_level(int msg_log_level, const char *format, ...);
void log_debug(const char *format, ...);
void log_info(const char *format, ...);
void log_warn(const char *format, ...);
void log_error(const char *format, ...);
void log_fatal(const char *format, ...);

#endif
