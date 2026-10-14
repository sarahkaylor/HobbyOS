/* ar.h - ar(1) archive format (POSIX / SysV layout as on Linux).
 *
 * HobbyOS shim header for the GNU make port: the sysroot ships no <ar.h>,
 * and make's arscan.c includes it unconditionally on non-Android/BeOS/zOS
 * platforms (arscan.c:338-340).  This is the classic System V definition
 * (matching glibc/linux/ar.h field-for-field); make only reads ar_name,
 * ar_date and ar_size and checks ARFMAG.  Keep byte-identical with the
 * platform convention so archives made by the host's ar/arscan match.
 *
 * This is an OS-surface shim, not a make source: like glob.h/fnmatch.h it
 * exists to satisfy the upstream transcription, which stays byte-identical.
 */
#ifndef HOBBYOS_AR_H
#define HOBBYOS_AR_H

#define ARMAG "!<arch>\n" /* magic for the start of an archive          */
#define SARMAG 8          /* length of ARMAG                            */
#define ARFMAG "`\n"      /* magic in ar_fmag at end of each member hdr */

struct ar_hdr {
  char ar_name[16]; /* member file name, sometimes / terminated */
  char ar_date[12]; /* file date, decimal seconds since Epoch  */
  char ar_uid[6];   /* user ID, in ASCII decimal               */
  char ar_gid[6];   /* group ID, in ASCII decimal              */
  char ar_mode[8];  /* file mode, in ASCII octal               */
  char ar_size[10]; /* file size, in ASCII decimal             */
  char ar_fmag[2];  /* always contains ARFMAG                  */
};

#endif /* HOBBYOS_AR_H */
