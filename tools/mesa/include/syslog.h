/* syslog.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * src/util/log.c can route Mesa's log to syslog when MESA_LOG=syslog.
 * HobbyOS has no syslog daemon; the calls fail closed in gl_stubs.c and the
 * normal stderr path is what the guest uses.  Priority values are glibc's. */
#ifndef HOBBYOS_SYSLOG_H
#define HOBBYOS_SYSLOG_H 1

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_NDELAY 0x08
#define LOG_USER   (1 << 3)

#ifdef __cplusplus
extern "C" {
#endif
void openlog(const char *ident, int option, int facility);
void syslog(int priority, const char *format, ...);
void closelog(void);
#ifdef __cplusplus
}
#endif

#endif
