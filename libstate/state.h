#ifndef _CLIB_STATE_H_
#define _CLIB_STATE_H_

#if defined(__cplusplus)
extern "C" {
#endif

int state_create_dir(char *dir);
void state_default_dir(const char *dir);
void state_set(const char *dir, const char *name, const char *value);
void state_get(const char *dir, const char *name, char **buf);

#if defined(__cplusplus)
}
#endif

#endif
