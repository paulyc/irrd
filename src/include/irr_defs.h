/* 
 * $Id
 */

#ifndef _IRRDDEFS_H
#define _IRRDDEFS_H

#include <config.h>

/* default cache dir */
#define DEF_DBCACHE "/var/spool/database"

#ifndef HAVE_SNPRINTF
extern int snprintf(char *, size_t, const char *, /*args*/ ...);
#endif

/* Solaris 2.5.1 is missing prototype for mkstemp() */
#if defined(HAVE_DECL_MKSTEMP) && ! HAVE_DECL_MKSTEMP
extern int mkstemp(char *template);
#endif

#if defined(HAVE_DECL_MKDTEMP) && ! HAVE_DECL_MKDTEMP
extern char *mkdtemp(char *template);
#endif

#define IRRDCACHER_CMD ""
#define WGET_CMD       "/usr/bin/wget"

#endif /* _IRRDDEFS_H */

