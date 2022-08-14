#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "state.h"

char *default_dir;

// Create state dir if it does not exist
int state_create_dir(char *dir) {
  struct stat st;
  int err;

  err = stat(dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      // Check if this is a broken symbolic link
      err = lstat(dir, &st);
      if (err == 0 && S_ISLNK(st.st_mode)) {
        fprintf(stderr, "error: ./%s is a broken symbolic link\n", dir);
        return -1;
      }

      // create directory
      if (mkdir(dir, 0755) == 0) { // success
        fprintf(stderr, "created state dir: ./%s\n", dir);
      } else { // error
        fprintf(stderr, "error creating state dir (./%s): %s\n",
            dir, strerror(errno));
        return -1;
      }
    } else {
      perror("stat state dir");
      return -1;
    }
  } else {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "error: state dir (./%s) is not a directory. remove it or replace it with a directory.\n",
          dir);
      return -1;
    }
  }

  if (access(dir, R_OK) != 0) {
    fprintf(stderr, "Can't access state dir (./%s): %s\n",
        dir, strerror(errno));
    return -1;
  }

  return 0;
}

void state_default_dir(const char *dir) {
  default_dir = (char *)dir;
}

void state_set(const char *dir, const char *name, const char *value) {
  FILE *fp;
  char *path;
  int path_len;
  struct stat st;
  int err;

  if (dir == NULL) {
    dir = default_dir;
  }

  err = stat(dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      fprintf(stderr, "Error: %s directory does not exist\n", dir);
    } else {
      perror("stat error");
    }
    exit(EXIT_FAILURE);
  } else {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "Error: %s is not a directory\n", dir);
      exit(EXIT_FAILURE);
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

void state_get(const char *dir, const char *name, char **buf) {
  FILE *fp;
  char *path;
  int path_len;
  int size;
  struct stat st;
  int err;

  if (dir == NULL) {
    dir = default_dir;
  }

  err = stat(dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      fprintf(stderr, "Error: %s directory does not exist\n", dir);
    } else {
      perror("stat error");
    }
    exit(EXIT_FAILURE);
  } else {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "Error: %s is not a directory\n", dir);
      exit(EXIT_FAILURE);
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
