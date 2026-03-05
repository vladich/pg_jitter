/*
 * compat/win32/arpa/inet.h — Windows compatibility shim
 *
 * PostgreSQL headers may include <arpa/inet.h> which doesn't exist on
 * Windows.  The needed declarations come from <ws2tcpip.h>.
 */
#ifndef PG_JITTER_COMPAT_ARPA_INET_H
#define PG_JITTER_COMPAT_ARPA_INET_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif
