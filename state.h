#ifndef _CLIB_STATE_H_
#define _CLIB_STATE_H_

#if defined(__cplusplus)
extern "C" {
#endif

void state_set(char *dir, char *name, char *value);
void state_get(char *dir, char *name, char **buf);

#if defined(__cplusplus)
}
#endif

#endif
