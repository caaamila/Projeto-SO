#define _XOPEN_SOURCE 700

#include "filesystem.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include "constants.h"

int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}

// o src vem do <DIR_INPUT> do comando D e o dst é o caminho /tmp/CloudIST/<ID-reserva>/<ID-VM>
int copy_directory_recursive(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) return 1;

    // cria o "/tmp/CloudIST/%s/%s", reservation->id, new_vm->id 
    mkdir(dst, 0755);

    DIR *dir = opendir(src);
    if (!dir) return 1;

    struct dirent *entry;
    char src_path[MAX_PATH_SIZE];
    char dst_path[MAX_PATH_SIZE];

    while ((entry = readdir(dir)) != NULL) {
      //.  /tmp/v1-inputs   (ela própria)
      //.. /tmp             (a pasta acima)
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

        // Obter metadados da entrada atual
        struct stat entry_st;
        if (stat(src_path, &entry_st) != 0) continue;

        if (S_ISDIR(entry_st.st_mode)) {
            if(copy_directory_recursive(src_path, dst_path)!=0){
              closedir(dir);
              return 1;
            }

        } else if (S_ISREG(entry_st.st_mode)) {
            int in_fd = open(src_path, O_RDONLY);
            if (in_fd < 0){
              closedir(dir);
              return 1;
            }

            int out_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out_fd < 0) {
              close(in_fd);
              closedir(dir);
              return 1;
            }

            char buf[4096];
            ssize_t bytes;
            while ((bytes = read(in_fd, buf, sizeof(buf))) > 0) {

                ssize_t bytes_escritos=write(out_fd, buf, (size_t)bytes);

                if(bytes_escritos!=bytes){
                  fprintf(stderr, "Nao foi escrito tudo oq era preciso");
                  close(in_fd);
                  close(out_fd);
                  closedir(dir);
                  return 1;
                }
            }

            if(bytes<0){
              fprintf(stderr, "Erro ao ler");
              close(in_fd);
              close(out_fd);
              closedir(dir);
              return 1;
            }

            close(in_fd);
            close(out_fd);
        }
    }
    closedir(dir);
    return 0;
}