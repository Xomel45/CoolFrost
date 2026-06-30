#ifndef ERRNO_H
#define ERRNO_H

/* ── POSIX-subset error codes ───────────────────────────────────────────── *
 * Functions return negative errno: return -ENOMEM, check ret < 0.          */

#define EPERM        1   /* Operation not permitted     */
#define ENOENT       2   /* No such file or directory   */
#define ESRCH        3   /* No such process             */
#define EINTR        4   /* Interrupted system call     */
#define EIO          5   /* I/O error                   */
#define ENXIO        6   /* No such device or address   */
#define ENOEXEC      8   /* Exec format error           */
#define EBADF        9   /* Bad file descriptor         */
#define ENOMEM      12   /* Out of memory               */
#define EACCES      13   /* Permission denied           */
#define EFAULT      14   /* Bad address                 */
#define EBUSY       16   /* Device or resource busy     */
#define EEXIST      17   /* File already exists         */
#define ENODEV      19   /* No such device              */
#define ENOTDIR     20   /* Not a directory             */
#define EISDIR      21   /* Is a directory              */
#define EINVAL      22   /* Invalid argument            */
#define ENFILE      23   /* File table overflow         */
#define EMFILE      24   /* Too many open files         */
#define EFBIG       27   /* File too large              */
#define ENOSPC      28   /* No space left on device     */
#define EROFS       30   /* Read-only file system       */
#define ERANGE      34   /* Result too large            */
#define ENOSYS      38   /* Function not implemented    */
#define ENOTEMPTY   39   /* Directory not empty         */
#define ELOOP       40   /* Too many symbolic links     */
#define ENOTSUP     95   /* Operation not supported     */
#define ETIMEDOUT  110   /* Connection timed out        */

/* ── Helpers ────────────────────────────────────────────────────────────── */

/* True if return value r represents an error */
#define IS_ERR(r)      ((r) < 0)

/* True if a pointer-encoded error (Linux-style) */
#define IS_ERR_PTR(p)  ((unsigned long)(p) >= (unsigned long)(-4096))

/* Error name string (for logging) */
static inline const char *errno_name(int e) {
    if (e < 0) e = -e;
    switch (e) {
    case EPERM:     return "EPERM";
    case ENOENT:    return "ENOENT";
    case EIO:       return "EIO";
    case ENOMEM:    return "ENOMEM";
    case EACCES:    return "EACCES";
    case EBUSY:     return "EBUSY";
    case ENODEV:    return "ENODEV";
    case EINVAL:    return "EINVAL";
    case ENOSPC:    return "ENOSPC";
    case ERANGE:    return "ERANGE";
    case ENOSYS:    return "ENOSYS";
    case ETIMEDOUT: return "ETIMEDOUT";
    case ENOTSUP:   return "ENOTSUP";
    default:        return "EUNKNOWN";
    }
}

#endif
