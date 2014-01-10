#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#include "state.h"

void state_set(char *dir, char *name, char *value) {
  FILE *fp;
  char *path;
  int path_len;
  struct stat st;
  int err;

  err = stat(dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      fprintf(stderr, "Error: %s directory does not exist\n", dir);
    } else {
      perror("stat error");
    }
    exit(1);
  } else {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "Error: %s is not a directory\n", dir);
      exit(1);
    }
  }

  path_len = strlen(dir) + strlen(name) + 2;
  path = malloc(path_len);
  if (path == NULL) {
    perror("malloc path");
    return;
  }
  snprintf(path, path_len, "%s/%s", dir, name);
  fp = fopen(path, "w");
  if (fp == NULL) {
    perror("State file open failed");
    return;
  }
  fwrite(value, 1, strlen(value), fp);
  fclose(fp);
  free(path);
}

void state_get(char *dir, char *name, char **buf) {
  FILE *fp;
  char *path;
  int path_len;
  int size;
  struct stat st;
  int err;

  err = stat(dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      fprintf(stderr, "Error: %s directory does not exist\n", dir);
    } else {
      perror("stat error");
    }
    exit(1);
  } else {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "Error: %s is not a directory\n", dir);
      exit(1);
    }
  }

  path_len = strlen(dir) + strlen(name) + 2;
  path = malloc(path_len);
  if (path == NULL) {
    perror("malloc path");
    return;
  }
  snprintf(path, path_len, "%s/%s", dir, name);
  fp = fopen(path, "r");
  if (fp == NULL) {
    perror("State file open failed");
    return;
  }
  fseek(fp, 0, SEEK_END);
  size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  *buf = malloc(size);
  if (*buf == NULL) {
    perror("Can't malloc for buffer");
    return;
  }
  fread(*buf, 1, size, fp);
  fclose(fp);
  free(path);
}
