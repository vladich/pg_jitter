/*
 * compat/win32/netinet/in.h — Windows compatibility shim
 *
 * PostgreSQL's server headers include <netinet/in.h> when HAVE_NETINET_IN_H
 * is defined in pg_config.h.  The EDB Windows installer's pg_config.h sets
 * this flag even though the header doesn't ship with MSVC.  Provide the
 * equivalent Windows types via <winsock2.h>.
 */
#ifndef PG_JITTER_COMPAT_NETINET_IN_H
#define PG_JITTER_COMPAT_NETINET_IN_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif
