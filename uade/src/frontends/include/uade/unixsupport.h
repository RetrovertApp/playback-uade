#ifndef _UADE_UNIXSUPPORT_H_
#define _UADE_UNIXSUPPORT_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <uade/uadeipc.h>

#ifdef _WIN32
#define pid_t int
#endif

#define uade_debug(state, ...)                           \
    do {                                                 \
        if ((state) == NULL || uade_is_verbose(state)) { \
            fprintf(stderr, __VA_ARGS__);                \
        }                                                \
    } while (0)
#define uade_die(...)                                  \
    do {                                               \
        fprintf(stderr, "uade: ");                     \
        fprintf(stderr, __VA_ARGS__);                  \
        exit(1);                                       \
    } while (0)
#define uade_die_error(...)                             \
    do {                                                \
        fprintf(stderr, "uade: ");                      \
        fprintf(stderr, __VA_ARGS__);                   \
        fprintf(stderr, ": %s\n", strerror(errno));     \
        exit(1);                                        \
    } while (0)
#define uade_error(...)                                                        \
    do {                                                                       \
        fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, __func__);          \
        fprintf(stderr, __VA_ARGS__);                                           \
        abort();                                                               \
    } while (0)
#define uade_info(...)                               \
    do {                                             \
        fprintf(stderr, "uade info: ");              \
        fprintf(stderr, __VA_ARGS__);                \
    } while (0)
#define uade_warning(...)                              \
    do {                                               \
        fprintf(stderr, "uade warning: ");             \
        fprintf(stderr, __VA_ARGS__);                  \
    } while (0)

char* uade_dirname(char* dst, char* src, size_t maxlen);
int uade_find_amiga_file(char* realname, size_t maxlen, const char* aname, const char* playerdir,
                         const char* moduledir);

/* Threading-based spawn - user_data is passed to thread callbacks */
void uade_arch_kill_and_wait_uadecore(struct uade_ipc* ipc, pid_t* uadepid, void* user_data);
int uade_arch_spawn(struct uade_ipc* ipc, pid_t* uadepid, const char* uadename, void* user_data);

int uade_filesize(size_t* size, const char* pathname);

#endif
