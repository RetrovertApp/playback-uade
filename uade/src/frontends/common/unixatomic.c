#include <uade/unixatomic.h>
#include <uade/sysincludes.h>

#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>

/* These operate on real file descriptors only; the uade IPC path uses
   uade_ipc_read()/uade_ipc_write() in uadechannel.c. */
#ifdef _WIN32
#include <io.h>
#define UREAD(fd, target, len) _read(fd, target, (unsigned int)(len))
#define UWRITE(fd, target, len) _write(fd, target, (unsigned int)(len))
#define UCLOSE(fd) _close(fd)
#else
#include <sys/types.h>
#include <unistd.h>
#define UREAD(fd, target, len) read(fd, target, len)
#define UWRITE(fd, target, len) write(fd, target, len)
#define UCLOSE(fd) close(fd)
#endif

int uade_atomic_close(int fd)
{
  while (1) {
    if (UCLOSE(fd) < 0) {
      if (errno == EINTR)
	continue;
      return -1;
    }
    break;
  }
  return 0;
}

int uade_atomic_dup2(int oldfd, int newfd)
{
#ifdef _WIN32
  (void)oldfd;
  (void)newfd;
  return -1;  /* Not supported on Windows */
#else
  while (1) {
    if (dup2(oldfd, newfd) < 0) {
      if (errno == EINTR)
	continue;
      return -1;
    }
    break;
  }
  return newfd;
#endif
}

#if !defined(__AMIGA__) && !defined(__AROS__)

ssize_t uade_atomic_read(int fd, const void *buf, size_t count)
{
  char *b = (char *) buf;
  ssize_t bytes_read = 0;
  ssize_t ret;
  while (bytes_read < (ssize_t)count) {
    ret = (ssize_t)UREAD(fd, &b[bytes_read], count - bytes_read);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    } else if (ret == 0) {
      return 0;
    }
    bytes_read += ret;
  }
  return bytes_read;
}

ssize_t uade_atomic_write(int fd, const void *buf, size_t count)
{
  char *b = (char *) buf;
  ssize_t bytes_written = 0;
  ssize_t ret;
  while (bytes_written < (ssize_t)count) {
    ret = (ssize_t)UWRITE(fd, &b[bytes_written], count - bytes_written);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    bytes_written += ret;
  }
  return bytes_written;
}

#endif
