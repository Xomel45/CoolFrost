#ifndef E1000_H
#define E1000_H

#include <stdint.h>

/* Returns 0 if a supported NIC was found and initialised, -1 otherwise */
int e1000_init(void);

/* Send `len` bytes from `buf` as a raw Ethernet frame.  Returns 0 on success. */
int e1000_send(const void *buf, uint16_t len);

/* Copy the next received frame into `buf` (max `max_len` bytes).
 * Returns the frame length, or 0 if the Rx ring is empty. */
int e1000_recv(void *buf, uint16_t max_len);

#endif
