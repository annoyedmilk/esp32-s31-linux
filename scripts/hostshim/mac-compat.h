/* Compatibility shim so Linux kernel host tools build on macOS.
 * Author: Marco Müller <hello@annoyedmilk.ch> */
#ifndef _S31_MAC_COMPAT_H
#define _S31_MAC_COMPAT_H
#ifdef __APPLE__
#include <unistd.h>
#include <fcntl.h>
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
static inline ssize_t copy_file_range(int fd_in, off_t *off_in, int fd_out,
				      off_t *off_out, size_t len,
				      unsigned int flags)
{
	char buf[65536];
	ssize_t total = 0;
	(void)off_in; (void)off_out; (void)flags;
	while (len > 0) {
		size_t chunk = len > sizeof(buf) ? sizeof(buf) : len;
		ssize_t r = read(fd_in, buf, chunk);
		ssize_t w = 0;
		if (r < 0)
			return total ? total : -1;
		if (r == 0)
			break;
		while (w < r) {
			ssize_t k = write(fd_out, buf + w, r - w);
			if (k < 0)
				return -1;
			w += k;
		}
		total += r;
		len -= (size_t)r;
	}
	return total;
}
#endif /* __APPLE__ */
#endif
