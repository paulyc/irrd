
/*
 * $Id: telnet.c,v 1.23 2002/10/17 20:02:31 ljb Exp $
 * originally Id: telnet.c,v 1.59 1998/08/03 17:29:10 gerald Exp 
 */

#include <sys/types.h>
#include <sys/socket.h>
#ifndef SETPGRP_VOID
#include <termios.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <stdarg.h>
#include <fcntl.h>

#include "mrt.h"
#include "select.h"
#include "trace.h"
#include "config_file.h"
#include "config_file.h"
#include "irrd.h"

#ifdef HAVE_IPV6
#define SOCKADDR sockaddr_in6
#else
#define SOCKADDR sockaddr_in
#endif

static int irr_read_command (irr_connection_t * irr);

/* local yokel's */
void irr_write_answer  (irr_answer_t *, irr_connection_t *);
void irr_write_direct (irr_connection_t *irr, FILE *fp, int len);
#ifndef HAVE_LIBPTHREAD    
static int irr_read_command_schedule (irr_connection_t *irr);
#endif /* HAVE_LIBPTHREAD */

void *start_irr_connection (irr_connection_t * irr_connection) {
  fd_set          read_fds;
  struct timeval  tv;
  int		  ret;

#ifdef HAVE_LIBPTHREAD
  sigset_t set;

  sigemptyset (&set);
  sigaddset (&set, SIGALRM);
  sigaddset (&set, SIGHUP);
  pthread_sigmask (SIG_BLOCK, &set, NULL);
#endif /* HAVE_LIBPTHREAD */

  irr_connection->cp = irr_connection->buffer;
  irr_connection->end = irr_connection->buffer;
  irr_connection->end[0] = '\0';

  trace (NORM, default_trace, "connection from %s (fd %d)\n",
	 prefix_toa (irr_connection->from), irr_connection->sockfd);

#ifndef HAVE_LIBPTHREAD    
  select_add_fd (irr_connection->sockfd, SELECT_READ,
		 (void *) irr_read_command_schedule, irr_connection);
  return NULL;
#endif /* HAVE_LIBPTHREAD */

  FD_ZERO(&read_fds);
  FD_SET(irr_connection->sockfd, &read_fds);

  while (1) {
    /* set connection timeout */
    tv.tv_sec = irr_connection->timeout;
    tv.tv_usec = 0;

    ret = select (irr_connection->sockfd + 1, &read_fds, 0, 0, &tv);
    if (ret <= 0) {
      if (ret == 0) 
	trace (NORM, default_trace, "select timeout on read\n");
      else
        trace (ERROR, default_trace,
	     "select error on read -- error (%s)\n", strerror (errno));
      irr_destroy_connection (irr_connection);
      return NULL;
    }

    if (irr_connection->scheduled_for_deletion == 1) {
     trace (ERROR, default_trace, "Unexpected scheduled for deletion\n");
      irr_destroy_connection (irr_connection);
      return NULL;
    }

    ret = irr_read_command (irr_connection);

    if (ret < 0) {
	trace (NORM, default_trace, "Connection Aborted\n");
	return NULL;
    }

    if (((ret == 1) && (irr_connection->stay_open == 0)) ||
	(irr_connection->scheduled_for_deletion == 1)) {
      /*trace (NORM, default_trace,
	"Closing connection.  stay_open set to close.\n");*/
      irr_destroy_connection (irr_connection);
      return NULL;
    }
  }
}

int irr_accept_connection (int fd) {
  int sockfd;
  socklen_t len;
  int family;
  int too_many_flag = 0;
  prefix_t *prefix;
  struct SOCKADDR addr;
  irr_connection_t *irr_connection;
  u_int one = 1;
  char *ascii_prefix;
  char tmp[BUFSIZE];
  irr_database_t *database; 
  connection_hash_t *connection_hash_item;

  len = sizeof (addr);
  memset ((struct SOCKADDR *) &addr, 0, len);
  
  if ((sockfd = accept (fd, (struct sockaddr *) &addr, &len)) < 0) {
    trace (ERROR, default_trace, "Accept failed (%s)\n",
	   strerror (errno));
    select_enable_fd (fd);
    return (-1);
  }
  select_enable_fd (fd);

  if (setsockopt (sockfd, IPPROTO_TCP, TCP_NODELAY, (char *) &one,
		  sizeof (one)) < 0) {
    trace (ERROR, default_trace, "setsockoptfailed\n");
    close (sockfd);
    return (-1);
  }
 
#ifdef HAVE_IPV6 
  if ((family = addr.sin6_family) == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *) &addr;
    prefix = New_Prefix (AF_INET, &sin->sin_addr, 32);
  } else if (family == AF_INET6) {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &addr;
    if (IN6_IS_ADDR_V4MAPPED (&sin6->sin6_addr))
      prefix = New_Prefix (AF_INET, ((char *) &sin6->sin6_addr) + 12, 32);
    else
      prefix = New_Prefix (AF_INET6, &sin6->sin6_addr, 128);
  }
#else
  if ((family = addr.sin_family) == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
    prefix = New_Prefix (AF_INET, &sin->sin_addr, 32);
  }  
#endif
  else {
    trace (ERROR, default_trace, "unknown connection family = %d\n",
	   family);
    close (sockfd);
    return (-1);
  }  

  /* check load */
  if (IRR.connections > IRR.max_connections) {
    trace (INFO, default_trace, "Too many connections -- REJECTING %s\n",
	   prefix_toa (prefix));
    Deref_Prefix (prefix);
    close (sockfd);
    return (-1);
  }

  /* Apply access list (if one exists) */
  if (IRR.irr_port_access > 0) {
    if (!apply_access_list (IRR.irr_port_access, prefix)) {
      trace (NORM, default_trace, "connection DENIED from %s\n",
	     prefix_toa (prefix));
      Deref_Prefix (prefix);
      close (sockfd);
      return (-1);
    }
  }

  /* check per host connnection limit */
  ascii_prefix = prefix_toa(prefix);

  if (pthread_mutex_lock (&IRR.connections_mutex_lock) != 0)
    trace (ERROR, default_trace, "locking -- connection_mutex_lock--: %s\n",
	   strerror (errno));

  connection_hash_item = g_hash_table_lookup(IRR.connections_hash, ascii_prefix);
  if (connection_hash_item == NULL) {
    connection_hash_item = irrd_malloc(sizeof(connection_hash_t));
    connection_hash_item->key = strdup(ascii_prefix);
    connection_hash_item->num = 1;
    g_hash_table_insert(IRR.connections_hash, connection_hash_item->key, connection_hash_item);
  } else {
    if (connection_hash_item->num >= MAX_PER_IP_CONNECTIONS) {
      too_many_flag = 1;
    } else {
      connection_hash_item->num++;
    }
  }

  if (pthread_mutex_unlock (&IRR.connections_mutex_lock) != 0)
    trace (ERROR, default_trace, "unlocking -- connection_mutex_lock--: %s\n",
	   strerror (errno));

  if (too_many_flag) {
    trace (INFO, default_trace, "Too many host connections (%d) -- REJECTING %s\n",
	   connection_hash_item->num, ascii_prefix);
    Deref_Prefix (prefix);
    close (sockfd);
    return (-1);
  }

  trace (TRACE, default_trace, "accepting connection from %s\n",
	 ascii_prefix);

  irr_connection = irrd_malloc(sizeof(irr_connection_t));
#ifndef HAVE_LIBPTHREAD    
  irr_connection->schedule = New_Schedule ("irr_connection", default_trace);
#else
  irr_connection->schedule = NULL;
#endif
  irr_connection->sockfd = sockfd;
  irr_connection->from = prefix;
  irr_connection->ll_database = LL_Create (0);
  irr_connection->timeout = 60; /*  default timeout in seconds */
  irr_connection->full_obj = 1;
  irr_connection->end = irr_connection->buffer;
  irr_connection->start = time (NULL);

  /* by default, use all databases in order appear IRRd config file */
  LL_Iterate (IRR.ll_database, database) {

    /* Apply access list (if one exists) */
    if ((database->access_list > 0) &&
	(!apply_access_list (database->access_list, prefix))) {
      trace (NORM, default_trace, "Access to %s denied for %s\n",
	     database->name, ascii_prefix);
    }
    else {
      if (! (database->flags & IRR_NODEFAULT))
	LL_Add (irr_connection->ll_database, database);
    }
  }

  if (pthread_mutex_lock (&IRR.connections_mutex_lock) != 0)
    trace (ERROR, default_trace, "locking -- connection_mutex_lock--: %s\n",
	   strerror (errno));

  LL_Add (IRR.ll_connections, irr_connection);
  IRR.connections++;

  if (pthread_mutex_unlock (&IRR.connections_mutex_lock) != 0)
    trace (ERROR, default_trace, "unlocking -- connection_mutex_lock--: %s\n",
	   strerror (errno));

  sprintf (tmp, "IRR %s", ascii_prefix);
  mrt_thread_create (tmp, irr_connection->schedule,
		     (thread_fn_t) start_irr_connection, irr_connection);
  return (1);
}

/*
 * begin listening for connections on a well known port
 */
int 
listen_telnet (u_short port) {
  socklen_t len;
  int optval;
  int sockfd;
  int family;
  struct sockaddr *sa;
  struct sockaddr_in serv_addr;
#ifdef HAVE_IPV6 
  struct sockaddr_in6 serv_addr6;

  memset(&serv_addr6, 0, sizeof serv_addr6);
  memcpy(&serv_addr6.sin6_addr, &in6addr_any, sizeof (in6addr_any));
  serv_addr6.sin6_family = AF_INET6;
  serv_addr6.sin6_port = htons(port);
#endif

  memset(&serv_addr, 0, sizeof serv_addr);
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons (port);

  /* this port has not been configured */
  if (port <= 0) return (0);

#ifdef HAVE_IPV6
  family = AF_INET6;
  sa = (struct sockaddr *) &serv_addr6;
  len = sizeof (serv_addr6);
#else
  family = AF_INET;
  sa = (struct sockaddr *) &serv_addr;
  len = sizeof (serv_addr);
#endif  /* HAVE_IPV6 */

  if ((sockfd = socket (family, SOCK_STREAM, IPPROTO_TCP)) < 0) {
#ifdef HAVE_IPV6
    /* retry with IPV4 */
    trace (NORM, default_trace, "Unable to get IPV6 socket (%s), retrying with IPV4\n", strerror(errno));
    family = AF_INET;
    sa = (struct sockaddr *) &serv_addr;
    len = sizeof (serv_addr);
    if ((sockfd = socket (family, SOCK_STREAM, IPPROTO_TCP)) < 0) {
#endif
      trace (ERROR, default_trace, "Could not get socket (%s)\n",
	   strerror (errno));
      return (-1);
#ifdef HAVE_IPV6
    }
#endif  /* HAVE_IPV6 */
  }

#ifdef IPV6_V6ONLY
  /* want to listen for both IPv4 and IPv6 connections on same socket */
  optval = 0;
  if (setsockopt (sockfd, IPPROTO_IPV6, IPV6_V6ONLY,
	 (const char *) &optval, sizeof (optval)) < 0) {
    trace (ERROR, default_trace, "Could not clear IPV6_V6ONLY (%s)\n",
		strerror (errno));
  }
#endif

  optval = 1;
  if (setsockopt (sockfd, SOL_SOCKET, SO_REUSEADDR,
		  (const char *) &optval, sizeof (optval)) < 0) {
    trace (ERROR, default_trace, "Could not set SO_RESUSEADDR (%s)\n",
	   strerror (errno));
  }
  
  if (bind (sockfd, sa, len) < 0) {
    trace (ERROR, default_trace, 
	   "Could not bind to port %d (%s)\n",
	   port, strerror(errno));
    return (-1);
  }

  listen (sockfd, 128);

  trace (NORM, default_trace, "listening for connections on port %d (fd %d)\n",
	 port, sockfd);
                   
  select_add_fd (sockfd, 1, (void_fn_t) irr_accept_connection, (void*)(intptr_t)sockfd);

  return (sockfd);
}

#ifndef HAVE_LIBPTHREAD
static int irr_read_command_schedule (irr_connection_t *irr) {
  schedule_event (irr->schedule, (void *) irr_read_command, 1, irr);
	return (1);
}
#endif /* HAVE_LIBPTHREAD */

/* 
 * irr_read_command
 * A misnamed routine -- actually read command input into buffer and
 * and process the buffer. We may, or may not have a command...
 * If we have not processed a command, return 0 (1 otherwise)
 */
static int irr_read_command (irr_connection_t * irr) {
  int n, i, state;
  char *cp, *newline, *tmp_ptr;
  int command_found = 0;

  if ((n = read (irr->sockfd, irr->end, BUFSIZE - (irr->end - irr->buffer) - 2)) <= 0) {
    trace (NORM, default_trace, "read failed %d (errno - %d)\n", n, errno);
    irr_destroy_connection (irr);
    return (-1);
  }

  irr->end += n;
  *(irr->end) = '\0'; /* necessary for IRR_MODE_LOAD_UPDATE case */
  state = irr->state;
  /* we should probably have this in a loop... */
  while ((newline = strchr (irr->buffer, '\r')) != NULL) {
    memmove (newline, newline + 1, irr->end - newline);
    irr->end--;
    n--;
  }

  while ((newline = strchr (irr->buffer, '\n')) != NULL ||
	 state == IRR_MODE_LOAD_UPDATE) {

    if (newline != NULL) {
      if (state == IRR_MODE_LOAD_UPDATE) {
	i = newline - irr->buffer + 1;
	memcpy (irr->tmp, irr->buffer, i);
	*(irr->tmp + i) = '\0';
      }
      else
	*newline = '\0';
    }
    else
      strcpy (irr->tmp, irr->buffer);

    command_found = 1;
    
    cp = irr->tmp;
    if (state != IRR_MODE_LOAD_UPDATE) {
      strcpy (cp, irr->buffer);

      /* remove any trailing spaces */
      tmp_ptr = cp + strlen (cp) - 1;
      while (tmp_ptr >= (cp) && isspace ((unsigned char) *tmp_ptr)) {
	*tmp_ptr = '\0';
	tmp_ptr--;
      }
      
      /* remove leading spaces */
      while (*cp && isspace ((unsigned char) *cp)) {
	cp++;
      }
    }
    irr->cp = cp;

    irr_process_command (irr); 

    /* user has quit or we've unexpetdly terminated */
    if (irr->stay_open == 0) {
#ifndef HAVE_LIBPTHREAD
      irr_destroy_connection (irr);
#endif /* HAVE_LIBPTHREAD */
      return (1);
    }

    /* IRR_MODE_LOAD_UPDATE case:
     * there may not be a newline, so
     * process all 'n' chars in buffer
     */
    if (state == IRR_MODE_LOAD_UPDATE) {
      n -= strlen (irr->tmp);
      if (n <= 0) {
	irr->end = irr->buffer;
	break;
      }
    }

    newline++;
    if ((irr->end - newline) > 0) 
      memmove (irr->buffer, newline, irr->end - newline);
    irr->end = irr->buffer + (irr->end - newline);
    *(irr->end) = '\0';
  }

#ifndef HAVE_LIBPTHREAD
  select_enable_fd (irr->sockfd);
#endif /* HAVE_LIBPTHREAD */
  
  return (command_found);
}

int irr_destroy_connection (irr_connection_t * connection) {
  connection_hash_t *connection_hash_item;
  char *ascii_prefix;

  if (pthread_mutex_lock (&IRR.connections_mutex_lock) != 0)
    trace (ERROR, default_trace, "connection_mutex_lock--: %s\n",
	strerror (errno));

  LL_Remove (IRR.ll_connections, connection);
  ascii_prefix = prefix_toa(connection->from);
  connection_hash_item = g_hash_table_lookup(IRR.connections_hash, ascii_prefix);
  if (connection_hash_item == NULL) {
    trace (ERROR, default_trace, "error locating hash entry for %s\n", ascii_prefix);
  } else {
    connection_hash_item->num--;
    if (connection_hash_item->num < 1) {
      g_hash_table_remove(IRR.connections_hash, connection_hash_item->key);
      free(connection_hash_item->key);
      irrd_free(connection_hash_item);
    }
  }

  IRR.connections--;

  if (pthread_mutex_unlock (&IRR.connections_mutex_lock) != 0)
    trace (ERROR, default_trace, "connection_mutex_lock--: %s\n",
	strerror (errno));

  trace (NORM, default_trace, 
	"Closing connection from %s (fd %d, %d connections)\n", 
	ascii_prefix,
	connection->sockfd,
	IRR.connections);

#ifndef HAVE_LIBPTHREAD
  select_delete_fd (connection->sockfd);
#else
  close (connection->sockfd);
#endif /* HAVE_LIBPTHREAD */

  /*LL_RemoveFn (IRR.ll_irr_connections, connection, 0);*/
  Deref_Prefix (connection->from);
#ifndef HAVE_LIBPTHREAD    
  if(connection->schedule->is_running > 0)
    /* if we running as a scheduled event, can't destroy schedule yet */
    delete_schedule (connection->schedule);
  else
    /* it's safe to destroy the schedule */
    destroy_schedule (connection->schedule);
#endif
  LL_Destroy (connection->ll_database);

  if (connection->answer != NULL)
    irrd_free(connection->answer);

  irrd_free(connection);

  mrt_thread_exit ();
  /* NOTREACHED */
  return(-1);
}

int irr_add_answer (irr_connection_t *irr, char *format, ...) {
  va_list args;
  int len;
  char buffer[BUFSIZE];

  if (irr->answer == NULL) {
    /* If this is first time, create a linked list and malloc a buffer */
    irr->ll_answer = LL_Create (LL_DestroyFunction, free, 0);
    irr->answer = malloc (IRR_OUTPUT_BUFFER_SIZE);
    irr->answer_len = 0;
  }

  if (irr->answer_len >= (IRR_OUTPUT_BUFFER_SIZE - BUFSIZE)) {
    /* buffer too big, add answer to linked list and malloc more space */
    irr_build_memory_answer(irr, irr->answer_len, irr->answer);
    irr->answer = malloc (IRR_OUTPUT_BUFFER_SIZE);
    irr->answer_len = 0;
  }

  va_start (args, format);
  vsprintf (buffer, format, args);  

  len = strlen (buffer);
  memcpy(irr->answer + irr->answer_len, buffer, len);
  irr->answer_len += len;

  return (1);
}

/* 
 * Send answer built up by irr_add_answer
 *
 */
void irr_send_answer (irr_connection_t * irr) {
  irr_answer_t *irr_answer;
  char *cp;

  if (irr->answer == NULL) {	/* short-cut if no answer */
    irr_write_nobuffer (irr, "D\n");
    trace (NORM, default_trace, "No entries found\n");
    return;
  }

  /* add a terminating newline if not already present */
  cp = irr->answer + irr->answer_len - 1;
  if (*cp++ != '\n') {
    *cp = '\n';
    irr->answer_len++;
  }
  /* Add buffered data to the linked list */ 
  irr_build_memory_answer(irr, irr->answer_len, irr->answer);

  send_dbobjs_answer (irr, MEM_INDEX, RAWHOISD_MODE);
  LL_ContIterate (irr->ll_answer, irr_answer) {
    free (irr_answer->blob);	/* free our malloc'ed memory */
  }
  irr_write_buffer_flush (irr);
  LL_Destroy(irr->ll_answer);
  irr->answer = NULL;
}

/* irr_write_buffer_flush
 * Called after we're done itterating through the database building up an answer.
 * This routine actually writes out to the socket, feeding it final_answer
 * structures built during irr_write
 */
void irr_write_buffer_flush (irr_connection_t *irr) {
  int n, ret;
  int fd = irr->sockfd;
  u_char *ptr;
  fd_set          write_fds;
  struct timeval  tv;
  final_answer_t *final_answer;

  /* something happened to the socket at some point -- delete before read */
  if (irr->scheduled_for_deletion)
    return;

  FD_ZERO(&write_fds);
  FD_SET(fd, &write_fds);

  if (irr->ll_final_answer == NULL) return;

  /* iterate through all of our linked answers */
  LL_Iterate (irr->ll_final_answer, final_answer) {
    ptr = final_answer->buf;

    while (ptr < final_answer->ptr) {
      tv.tv_sec = 60; /* 60 second timeout on trying to write to socket */
      tv.tv_usec = 0;

      ret = select (fd + 1, 0, &write_fds, 0, &tv);
      if (ret <= 0) {
        if (ret == 0) 
  	  trace (NORM, default_trace, "select timeout on buffered write\n");
        else
          trace (ERROR, default_trace,
	       "select error on buffered write -- error (%s)\n", strerror (errno));
	irr->scheduled_for_deletion = 1;
	LL_Destroy (irr->ll_final_answer);
	irr->ll_final_answer = NULL;
	return;
      }

      if ((n = write (fd, ptr, final_answer->ptr - ptr)) < 0) {
	trace (ERROR, default_trace, "buffered write error (%s)\n", strerror (errno));
	irr->scheduled_for_deletion = 1;
	LL_Destroy (irr->ll_final_answer);
	irr->ll_final_answer = NULL;
	return;
      }
      ptr += n;
    }
  }

  /* free ll_final_answer structs!!!! Need to write a destroy routine */
  LL_Destroy (irr->ll_final_answer);
  irr->ll_final_answer = NULL;
  return;
}

/* write a null terminated string directly to a connection */
void irr_write_nobuffer (irr_connection_t *irr, char *buf) {
  int n, ret, len;
  int fd = irr->sockfd;
  char *ptr;
  fd_set          write_fds;
  struct timeval  tv;

  len = strlen(buf);
  ptr = buf;

  /* something happened to the socket at some point -- delete before read */
  if (irr->scheduled_for_deletion)
    return;

  FD_ZERO(&write_fds);
  FD_SET(fd, &write_fds);

  while ((ptr - buf) < len) {
    tv.tv_sec = 60; /* 60 second timeout on trying to write to socket */
    tv.tv_usec = 0;

    ret = select (fd + 1, 0, &write_fds, 0, &tv);
    if (ret <= 0) {
      if (ret == 0) 
	trace (NORM, default_trace, "select timeout on unbuffered write\n");
      else
	trace (ERROR, default_trace,
	       "select error on unbuffered write -- error (%s)\n", strerror (errno));
      irr->scheduled_for_deletion = 1;
      return;
    }

    if ((n = write (fd, buf, len - (ptr-buf))) < 0) {
      irr->scheduled_for_deletion = 1;
      trace (ERROR, default_trace, "unbuffered write error (%s)\n", strerror (errno));
      return;
    }
    ptr += n;
  }
  return;
}

void delete_final_answer (final_answer_t *tmp) {
  irrd_free(tmp->buf);
  irrd_free(tmp);
}

/* irr_write_direct
 * copy direct from disk to memory buffers in a linked_list hung off the  
 * irr_connection structure.
 * We later call irr_write_buffer_flush after we finish gathering answer
 * and releasing all the locks
 */
void irr_write_direct (irr_connection_t *irr, FILE *fp, int len) {
  int bytes, n = 0, read = 0;
  final_answer_t *final_answer;

  while (read < len) {
    if (irr->ll_final_answer == NULL) { /* check if first time */
      irr->ll_final_answer = LL_Create (LL_DestroyFunction, delete_final_answer, 0);
      final_answer = NULL;
    } else {
      final_answer = LL_GetTail (irr->ll_final_answer);
      if (final_answer != NULL)
        n = 4096 - (final_answer->ptr - final_answer->buf);
    }

    /* no room, we need to add another one */
    if (final_answer == NULL || n == 0) {
      final_answer = irrd_malloc(sizeof(final_answer_t));
      final_answer->buf = final_answer->ptr = malloc (4096); 
      LL_Add (irr->ll_final_answer, final_answer);
      n = 4096;
    }

    /* write either all thats left, or as much room as left in buffer */
    if (n < (len - read)) 
      bytes = n;
    else
      bytes = len - read;

    (void)fread(final_answer->ptr, 1, bytes, fp);
    read += bytes;
    final_answer->ptr += bytes;
#if OPT_POSTGRES
    /* Add geoidx hook; someday this will be a proper database and joins will do this */
    /* Someday (sooner, I hope) this will support > 1 geoidx */
    gchar *geoidx;
    int idx;
    char query[250];
    char buf[100] = {'\0'};
    if ( NULL != (geoidx = g_strstr_len(final_answer->ptr - bytes, bytes, "geoidx"))) {
      geoidx += strlen("geoidx");
      while (*++geoidx == ' '); /* first ++ gets rid of ':' token */
      idx = atoi(geoidx);
#include <postgresql/libpq-fe.h>
      /* What?? #inlcude here? Disgusting! But, this code won't be around long anyways
	 because we're replacing everything with postgresql soon, right? Riiigghttt?? */
      PGconn *conn;
      PGresult *res;
      conn = PQconnectdb("dbname=geo_test user=ppannuto");
      if (PQstatus(conn) == CONNECTION_BAD) {
	      fprintf(stderr, "Unable to connect to database: %s", PQerrorMessage(conn));
	      return;
      }
      snprintf(query, 250, "SELECT cntry_name,fips_cntry FROM wb WHERE gid = %d", idx);
      res = PQexec(conn, query);
      if (PQresultStatus(res) != PGRES_TUPLES_OK) {
	      fprintf(stderr, "Did not receive data from query?\n");
	      return;
      }

      if (1 != PQntuples(res)) {
	      fprintf(stderr, "Wrong number of tuples? (%d)\n", PQntuples(res));
	      return;
      }

      int i;
      for (i = 0; i < PQnfields(res); i++) {
	      /* FIXME: 100's enough, right? Bad! */
	      strcat(buf, PQfname(res, i));
	      strcat(buf, ":\t");
	      strcat(buf, PQgetvalue(res, 0, i));
	      strcat(buf, "\n");
	      fprintf(stderr, "%s\n", buf);
      }

      PQclear(res);

      bytes = final_answer->ptr - final_answer->buf;
      if (strlen(buf) + 1 > bytes)
	      fprintf(stderr, "WARNING: extra geoidx info truncated\n");
      bytes = snprintf(final_answer->ptr, bytes, "%s", buf);
      final_answer->ptr += bytes;
    }
#endif
  }
  return;
}

/* irr_write
 * Just copy buf answer to memory buffers in a linked_list hung off the  
 * irr_connection structure.
 * We later call irr_write_buffer_flush after we finish gathering answer
 * and releasing all the locks
 */
void irr_write (irr_connection_t *irr, char *buf, int len) {
  int bytes, n = 0;
  char *ptr;
  final_answer_t *final_answer;

  ptr = buf;
  
  while ((ptr - buf) < len) {
    if (irr->ll_final_answer == NULL) { /* check if ll_final_answer initialized */
      irr->ll_final_answer = LL_Create (LL_DestroyFunction, delete_final_answer, 0);
      final_answer = NULL;
    } else {
      final_answer = LL_GetTail (irr->ll_final_answer);
      if (final_answer != NULL)
        n = 4096 - (final_answer->ptr - final_answer->buf);
    }

    /* no room, we need to add another one */
    if (final_answer == NULL || n == 0) {
      final_answer = irrd_malloc(sizeof(final_answer_t));
      final_answer->buf = final_answer->ptr = malloc (4096); 
      LL_Add (irr->ll_final_answer, final_answer);
      n = 4096;
    }

    /* write either all thats left, or as much room as left in buffer */
    if (n < (len - (ptr-buf))) 
      bytes = n;
    else
      bytes = len - (ptr-buf);

    memcpy (final_answer->ptr, ptr, bytes);
    ptr += bytes;
    final_answer->ptr += bytes;
  }
  return;
}

void irr_send_okay (irr_connection_t * irr) {
  irr_write_nobuffer (irr, "C\n");
}

void irr_mode_send_error (irr_connection_t * irr, int mode, char *msg) {
  char tmp[1024];

  if (mode == RAWHOISD_MODE)
    irr_send_error(irr, msg);
  else {
    sprintf (tmp, "\n%%ERROR: %s\n\n\n", msg);
    irr_write_nobuffer (irr, tmp);
    trace (NORM, default_trace, "Returned error: %s\n", msg);
  }
}

void irr_send_error (irr_connection_t * irr, char *msg) {
  char tmp[1024];

  if (msg != NULL)
    sprintf (tmp, "F %s\n", msg);
  else
    strcpy (tmp, "F\n");

  irr_write_nobuffer (irr, tmp);
  trace (NORM, default_trace, "Returned error: %s\n", tmp);
}

void send_dbobjs_answer (irr_connection_t *irr, enum INDEX_T index, int mode) {
  char buffer[BUFSIZE];
  irr_answer_t *irr_answer;
  u_long answer_size = 0;
  char *disc_str;
  int first = 1;

  /* compute answer size */ 
  LL_ContIterate (irr->ll_answer, irr_answer) {
    if (first != 1 && index == DISK_INDEX) 
      answer_size += 1; /* need to add '\n' between disk obj's for blank line */
    answer_size += (u_long) irr_answer->len;
    first = 0;
  }

  /* return "D" return code or no entries message */
  if (answer_size == 0) {
    if (mode == RAWHOISD_MODE)
      strcpy (buffer, "D\n");
    else { /* later read this string from configs, ie make it customizable */
      strcpy (buffer, "%  No entries found for the selected source(s).\n");
      if (irr->stay_open) /* emulate RIPE server for persistent connections */
	strcat(buffer, "\n\n");	/* RIPE server provides to empty lines */
    }
    irr_write (irr, buffer, strlen (buffer));
    trace (NORM, default_trace, "No entries found\n");
    return;
  }

  if (mode == RAWHOISD_MODE) {
    /* # of bytes in answer */
    sprintf (buffer, "A%d\n", (int) answer_size);
    irr_write (irr, buffer, strlen(buffer));
  } else if ( (irr->ripe_flags & ROA_STATUS) && IRR.ll_roa_disclaimer) {
    buffer[0] = '\0';
    LL_Iterate (IRR.ll_roa_disclaimer, disc_str) {
      strcat (buffer, disc_str);
      strcat (buffer, "\n");
    }
    irr_write (irr, buffer, strlen(buffer));
  }

  if (index == DISK_INDEX) {
    first = 1;
    LL_ContIterate (irr->ll_answer, irr_answer) {
      if (first != 1) {
        irr_write (irr, "\n", 1);  /* need to add a newline between objs */
      }
      irr_write_answer (irr_answer, irr); 
      first = 0;
    }
  } else { /* MEM_INDEX */
    LL_ContIterate (irr->ll_answer, irr_answer) {
      irr_write (irr, irr_answer->blob, irr_answer->len);
    }
  }
    
  /* add "C" return code */
  if (mode == RAWHOISD_MODE)
    irr_write (irr, "C\n", 2);
  else if (irr->stay_open == 1) /* irrtoolset wants two null lines after RIPE-style queries */
    irr_write (irr, "\n\n", 2);

  trace (NORM, default_trace, "Sent %d bytes\n", answer_size);  

} /* end send_dbobjs_answer() */

void irr_write_answer (irr_answer_t *irr_answer, irr_connection_t *irr) {
  int show_keyfields_only = irr->ripe_flags & KEYFIELDS_ONLY;
  int gen_roa_status = irr->ripe_flags & ROA_STATUS;
  int hide_cryptpw = 0, roamaxlen = -1;
  u_long len;
  char buf[BUFSIZE];
  char outbuf[BUFSIZE];

  if (gen_roa_status && irr_answer->roa_obj) {
    char *cp;
    enum STATES state  = BLANK_LINE, save_state;
    int curr_f = NO_FIELD;
    int loop_exit = 0;

    if (fseek (IRR.roa_database->db_fp, irr_answer->roa_obj->offset, SEEK_SET) < 0) {
      trace (ERROR, default_trace, "fseek failed in roa_database\n");
      return;
    }
    do {
      cp = fgets(buf, BUFSIZE, IRR.roa_database->db_fp);
      len = strlen(buf);
      strcpy(outbuf,buf);	/* need a copy because get_state may
				   modify our string */
      state = get_state (cp, len, state, &save_state);
      switch (state) {
        case START_F:
          curr_f = get_curr_f (buf);
	case LINE_CONT:
          if (curr_f == ROASTATUS_ATTR) {
            if (get_roamaxlen(outbuf,&roamaxlen) < 0) {
	      irr_write_nobuffer(irr, "%% Internal error. ROA maxlen.\n");
    	      trace (ERROR, default_trace, "error getting maxlen\n");
	      return;
	     } else
	      loop_exit = 1;	/* we've got our maxlen field */
	  }
	  break;
	case BLANK_LINE:
	case DB_EOF:
	  loop_exit = 1;
	  break;
        default:
          break;
      }
    } while (!loop_exit);
  }

  if (fseek (irr_answer->db->db_fp, irr_answer->offset, SEEK_SET) < 0) {
    trace (ERROR, default_trace, "fseek failed in irr_write_answer\n");
    irr_write_nobuffer(irr, "%% Internal error. fseek\n");
    return;
  }

  if  (irr_answer->type == MNTNER && irr_answer->db->cryptpw_access_list != 0 &&
    !apply_access_list(irr_answer->db->cryptpw_access_list, irr->from) )
    hide_cryptpw = 1;

  if (show_keyfields_only || hide_cryptpw || gen_roa_status) {
    char *cp;
    enum STATES state  = BLANK_LINE, save_state;
    int curr_f = NO_FIELD;
    int loop_exit = 0;

    if (irr_answer->len == 0)
      return;  /* Shouldn't happen, but check anyway */

    do {
      cp = fgets(buf, BUFSIZE, irr_answer->db->db_fp);
      len = strlen(buf);
      strcpy(outbuf,buf);	/* need a copy because get_state may
				   modify our string */

      state = get_state (cp, len, state, &save_state);
      switch (state) {
        case START_F:
          curr_f = get_curr_f (buf);
	case LINE_CONT:
          if (show_keyfields_only) {
            if (key_info[curr_f].f_type & KEY_F)
              irr_write (irr, outbuf, len);      
          } else {
            if (hide_cryptpw && curr_f == AUTH) {
              scrub_cryptpw(outbuf);
              scrub_md5pw(outbuf);
            }
            irr_write (irr, outbuf, len);
	  }
          if (gen_roa_status && curr_f == ORIGIN) {
  	    enum OBJ_ROASTATUS roastatus = ROA_INVALID;

	    if (roamaxlen == -1) { /* no ROA max length specified */
	      if (!irr_answer->roa_obj) { /* no ROA found at all */
		roastatus = ROA_UNKNOWN;
	      } else if (irr_answer->prefix_bitlen == irr_answer->roa_bitlen && irr_answer->prefix_obj->origin == irr_answer->roa_obj->origin) {
		roastatus = ROA_VALID;
	      } else
		roastatus = ROA_INVALID;
	    } else {	/* ROA max length field preset, must check */
	      if (irr_answer->prefix_obj->origin == irr_answer->roa_obj->origin
		  && irr_answer->prefix_bitlen >= irr_answer->roa_bitlen
		  && irr_answer->prefix_bitlen <= roamaxlen) {
		roastatus = ROA_VALID;
	      } else
		roastatus = ROA_INVALID;
	    }
	    switch (roastatus) {
	      case ROA_VALID:
		if (roamaxlen != -1) {
		  sprintf(outbuf, "roa-status: v=1; s=valid; m=%d; ",roamaxlen);
		} else {
		  strcpy (outbuf, "roa-status: v=1; s=valid; ");
		}
		break;
	      case ROA_INVALID:
		strcpy (outbuf, "roa-status: v=1; s=invalid; ");
		break;
              default:
		strcpy (outbuf, "roa-status: v=1; s=unknown; ");
		break;
	    }
	    strcat (outbuf, IRR.roa_timebuffer);
	    irr_write(irr, outbuf, strlen(outbuf));
	    if (irr->ripe_flags & ROA_URI && (roastatus == ROA_VALID || roastatus == ROA_INVALID)) {
	      do {
	        cp = fgets(outbuf, BUFSIZE, IRR.roa_database->db_fp);
	        if (cp == NULL)
		  break;
	        if (*cp == ' ' || *cp == '\t' || *cp == '+' )
	          irr_write(irr, outbuf, strlen(outbuf));
		else
		  break;
	      } while (TRUE);
	    }
	  }
	  break;
	case BLANK_LINE:
	case DB_EOF:
	  loop_exit = 1;
	  break;
        default:
 	  if (!show_keyfields_only)
            irr_write (irr, outbuf, len);
          break;
      }
    } while (!loop_exit);
  } else {
    irr_write_direct (irr, irr_answer->db->db_fp, irr_answer->len);      
  }
}

/* build an in-memory query answer */
void irr_build_memory_answer (irr_connection_t *irr, u_long len, char *blob) {
  irr_answer_t *irr_answer;

  irr_answer = irrd_malloc(sizeof(irr_answer_t));
  irr_answer->len = len;
  irr_answer->blob = blob;
  LL_Add (irr->ll_answer, irr_answer);
}

/* build a query answer referencing on-disk prefix type objects */
void irr_build_prefix_answer (irr_connection_t *irr, irr_database_t *database, irr_prefix_object_t *prefix_object) {
  irr_answer_t *irr_answer;

  irr_answer = irrd_malloc(sizeof(irr_answer_t));
  irr_answer->db = database;
  irr_answer->len = prefix_object->len;
  irr_answer->offset = prefix_object->offset;
  irr_answer->prefix_obj = prefix_object;
  LL_Add (irr->ll_answer, irr_answer);
} /* end irr_build_prefix_answer() */

/* build a query answer referencing on-disk prefix type objects */
void irr_build_roa_answer (irr_connection_t *irr, irr_database_t *database, irr_prefix_object_t *prefix_object, u_short bitlen, radix_node_t *roa_node) {
  irr_answer_t *irr_answer;

  irr_answer = irrd_malloc(sizeof(irr_answer_t));
  irr_answer->db = database;
  irr_answer->len = prefix_object->len;
  irr_answer->offset = prefix_object->offset;
  irr_answer->prefix_obj = prefix_object;
  irr_answer->roa_obj = roa_node->data;
  irr_answer->prefix_bitlen = bitlen;
  irr_answer->roa_bitlen = roa_node->prefix->bitlen;
  LL_Add (irr->ll_answer, irr_answer);
} /* end irr_build_roa_answer() */

/* build a query answer referencing on-disk objects */
void irr_build_answer (irr_connection_t *irr, irr_database_t *database, enum IRR_OBJECTS type, u_long offset, u_long len) {
  irr_answer_t *irr_answer;

  irr_answer = irrd_malloc(sizeof(irr_answer_t));
  irr_answer->db = database;
  irr_answer->type = type;
  irr_answer->offset = offset;
  irr_answer->len = len;
  LL_Add (irr->ll_answer, irr_answer);
} /* end irr_build_answer() */

/* show_connections
 * List current RAWhoisd TCP connections to UII 
 */
void show_connections (uii_connection_t *uii) {
  irr_connection_t *connection;
  int i = 1;

  uii_add_bulk_output (uii, "Currently %d connection(s) [MAX %d]\r\n\r\n",
		       IRR.connections, IRR.max_connections);

  if (pthread_mutex_lock (&IRR.connections_mutex_lock) != 0)
    trace (ERROR, default_trace, "Error locking -- connection_mutex_lock--: %s\n",
	   strerror (errno));

  LL_Iterate (IRR.ll_connections, connection) {
    uii_add_bulk_output (uii, "  %d  %s (fd=%d)  Age=%d\r\n", 
			 i++, prefix_toa (connection->from),
			 connection->sockfd,
			 time (NULL) - connection->start);
  }

  if (pthread_mutex_unlock (&IRR.connections_mutex_lock) != 0)
    trace (ERROR, default_trace, "Error locking -- connection_mutex_lock--: %s\n",
	   strerror (errno));

  uii_send_bulk_data (uii);
  return;
}
