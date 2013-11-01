#pragma once

/* Header files */
#cmakedefine HAVE_ARPA_INET_H ${HAVE_ARPA_INET_H}
#cmakedefine HAVE_ATOMIC_H ${HAVE_ATOMIC_H}
#cmakedefine HAVE_MACH_MACH_TIME_H ${HAVE_MACH_MACH_TIME_H}
#cmakedefine HAVE_MEMORY ${HAVE_MEMORY}
#cmakedefine HAVE_NETDB_H ${HAVE_NETDB_H}
#cmakedefine HAVE_NETINET_IN_H ${HAVE_NETINET_IN_H}
#cmakedefine HAVE_NETINET_IN_H ${HAVE_NETINET_IN_H}
#cmakedefine HAVE_NETINET_TCP_H ${HAVE_NETINET_TCP_H}
#cmakedefine HAVE_POLL_H ${HAVE_POLL_H}
#cmakedefine HAVE_SYSEXITS_H ${HAVE_SYSEXITS_H}
#cmakedefine HAVE_SYS_SOCKET_H ${HAVE_SYS_SOCKET_H}
#cmakedefine HAVE_SYS_TIME_H ${HAVE_SYS_TIME_H}
#cmakedefine HAVE_SCHED_H ${HAVE_SCHED_H}
#cmakedefine HAVE_TR1_MEMORY ${HAVE_TR1_MEMORY}
#cmakedefine HAVE_TR1_UNORDERED_MAP ${HAVE_TR1_UNORDERED_MAP}
#cmakedefine HAVE_UNISTD_H ${HAVE_UNISTD_H}
#cmakedefine HAVE_UNISTD_H ${HAVE_UNISTD_H}
#cmakedefine HAVE_UNORDERED_MAP ${HAVE_UNORDERED_MAP}
#cmakedefine HAVE_WINSOCK2_H ${HAVE_WINSOCK2_H}
#cmakedefine HAVE_WS2TCPIP_H ${HAVE_WS2TCPIP_H}

/* Functions */
#cmakedefine HAVE_CLOCK_GETTIME ${HAVE_CLOCK_GETTIME}
#cmakedefine HAVE_MACH_ABSOLUTE_TIME ${HAVE_MACH_ABSOLUTE_TIME}
#cmakedefine HAVE_GETTIMEOFDAY ${GETTIMEOFDAY}
#cmakedefine HAVE_GETOPT_LONG ${HAVE_GETOPT_LONG}

/* various */
#define VERSION "x.x.x"

#ifdef __GNUC__
#define HAVE_GCC_ATOMICS 1
#endif

#include "config_static.h"
