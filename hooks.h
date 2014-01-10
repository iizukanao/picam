#ifndef _CLIB_HOOKS_H_
#define _CLIB_HOOKS_H_

#if defined(__cplusplus)
extern "C" {
#endif

int clear_hooks(char *dirname);
void start_watching_hooks(pthread_t *thread, char *dir, void (*callback)(char *, char *), int read_content);
void stop_watching_hooks();

#if defined(__cplusplus)
}
#endif

#endif
