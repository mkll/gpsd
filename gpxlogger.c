/*
 * This file is Copyright (c) 2010 by the GPSD project
 * BSD terms apply: see the file COPYING in the distribution root for details.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <sys/time.h>		/* for select() */
#ifndef S_SPLINT_S
#include <unistd.h>
#endif /* S_SPLINT_S */

#include "gps.h"
#include "gpsdclient.h"
#include "revision.h"

#ifdef S_SPLINT_S
extern struct tm *gmtime_r(const time_t *, /*@out@*/ struct tm *tp);
#endif /* S_SPLINT_S */

/**************************************************************************
 *
 * Transport-layer-independent functions
 *
 **************************************************************************/

static char *author = "Amaury Jacquot, Chris Kuethe";
static char *license = "BSD";
static char *progname;

static FILE *logfile;
static bool intrack = false;
static time_t timeout = 5;	/* seconds */
static double minmove = 0;	/* meters */
#ifdef CLIENTDEBUG_ENABLE
static int debug;
#endif /* CLIENTDEBUG_ENABLE */

static void print_gpx_header(void)
{
    (void)fprintf(logfile,"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    (void)fprintf(logfile,"<gpx version=\"1.1\" creator=\"navsys logger\"\n");
    (void)fprintf(logfile,"        xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n");
    (void)fprintf(logfile,"        xmlns=\"http://www.topografix.com/GPX/1.1\"\n");
    (void)fprintf(logfile,"        xsi:schemaLocation=\"http://www.topografix.com/GPS/1/1\n");
    (void)fprintf(logfile,"        http://www.topografix.com/GPX/1/1/gpx.xsd\">\n");
    (void)fprintf(logfile," <metadata>\n");
    (void)fprintf(logfile,"  <name>NavSys GPS logger dump</name>\n");
    (void)fprintf(logfile,"  <author>%s</author>\n", author);
    (void)fprintf(logfile,"  <copyright>%s</copyright>\n", license);
    (void)fprintf(logfile," </metadata>\n");
    (void)fflush(logfile);
}

static void print_gpx_trk_end(void)
{
    (void)fprintf(logfile,"  </trkseg>\n");
    (void)fprintf(logfile," </trk>\n");
    (void)fflush(logfile);
}

static void print_gpx_footer(void)
{
    if (intrack)
	print_gpx_trk_end();
    (void)fprintf(logfile,"</gpx>\n");
    (void)fclose(logfile);
}

static void print_gpx_trk_start(void)
{
    (void)fprintf(logfile," <trk>\n");
    (void)fprintf(logfile,"  <trkseg>\n");
    (void)fflush(logfile);
}

static void print_fix(struct gps_fix_t *fix, struct tm *time)
{
    (void)fprintf(logfile,"   <trkpt lat=\"%f\" lon=\"%f\">\n",
		 fix->latitude, fix->longitude);
    (void)fprintf(logfile,"    <ele>%f</ele>\n", fix->altitude);
    (void)fprintf(logfile,"    <time>%04d-%02d-%02dT%02d:%02d:%02dZ</time>\n",
		 time->tm_year + 1900, time->tm_mon + 1, time->tm_mday,
		 time->tm_hour, time->tm_min, time->tm_sec);
    if (fix->mode == MODE_NO_FIX)
	(void)fprintf(logfile, "    <fix>none</fix>\n");
    else
	(void)fprintf(logfile, "    <fix>%dd</fix>\n", fix->mode);
#if 0
    /*
     * Can't print this more detailed report because in D-Bus mode
     * we don't necessarily have access to some of the stuff in gsdata.
     * Might mean some of this stuff should be promoted.
     */
    if ((gpsdata->status >= 2) && (gpsdata->fix.mode >= MODE_3D)) {
	/* dgps or pps */
	if (gpsdata->fix.mode == 4) {	/* military pps */
	    (void)fprintf(logfile,"        <fix>pps</fix>\n");
	} else {		/* civilian dgps or sbas */
	    (void)fprintf(logfile,"        <fix>dgps</fix>\n");
	}
    } else {			/* no dgps or pps */
	if (gpsdata->fix.mode == MODE_3D) {
	    (void)fprintf(logfile,"        <fix>3d</fix>\n");
	} else if (gpsdata->fix.mode == MODE_2D) {
	    (void)fprintf(logfile,"        <fix>2d</fix>\n");
	} else if (gpsdata->fix.mode == MODE_NOFIX) {
	    (void)fprintf(logfile,"        <fix>none</fix>\n");
	}			/* don't print anything if no fix indicator */
    }

    /* print # satellites used in fix, if reasonable to do so */
    if (gpsdata->fix.mode >= MODE_2D) {
	(void)fprintf(logfile,"        <hdop>%.1f</hdop>\n", gpsdata->hdop);
	(void)fprintf(logfile,"        <sat>%d</sat>\n", gpsdata->satellites_used);
    }
#endif

    (void)fprintf(logfile,"   </trkpt>\n");
    (void)fflush(logfile);
}

static void conditionally_log_fix(struct gps_fix_t *gpsfix)
{
    static time_t int_time, old_int_time;
    static double old_lat, old_lon;
    static bool first = true;

    int_time = (time_t) floor(gpsfix->time);
    if ((int_time == old_int_time) || gpsfix->mode < MODE_2D)
	return;

    /* may not be worth logging if we've moved only a very short distance */ 
    if (minmove>0 && !first && earth_distance(gpsfix->latitude, gpsfix->longitude,
					    old_lat, old_lon) < minmove)
	return;

    /* 
     * Make new track if the jump in time is above
     * timeout.  Handle jumps both forward and
     * backwards in time.  The clock sometimes jumps
     * backward when gpsd is submitting junk on the
     * dbus.
     */
    /*@-type@*/
    if (fabs(int_time - old_int_time) > timeout && !first) {
	print_gpx_trk_end();
	intrack = false;
    }
    /*@+type@*/

    if (!intrack) {
	print_gpx_trk_start();
	intrack = true;
	if (first)
	    first = false;
    }

    old_int_time = int_time;
    if (minmove > 0) {
	old_lat = gpsfix->latitude;
	old_lon = gpsfix->longitude;
    }
    print_fix(gpsfix, gmtime(&int_time));
}

static void quit_handler(int signum)
{
    /* don't clutter the logs on Ctrl-C */
    if (signum != SIGINT)
	syslog(LOG_INFO, "exiting, signal %d received", signum);
    print_gpx_footer();
    exit(0);
}

static struct gps_fix_t gpsfix;

#ifdef DBUS_ENABLE
/**************************************************************************
 *
 * Doing it with D-Bus
 *
 **************************************************************************/

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>

#include <glib/gprintf.h>

#define EMIX(x, y)	(((x) > (y)) ? (x) : (y))

DBusConnection *connection;

static char gpsd_devname[BUFSIZ];

static DBusHandlerResult handle_gps_fix(DBusMessage * message)
{
    DBusError error;
    /* this packet format was designed before we split eph */
    double eph = EMIX(gpsfix.epx, gpsfix.epy);

    dbus_error_init(&error);

    dbus_message_get_args(message,
			  &error,
			  DBUS_TYPE_DOUBLE, &gpsfix.time,
			  DBUS_TYPE_INT32, &gpsfix.mode,
			  DBUS_TYPE_DOUBLE, &gpsfix.ept,
			  DBUS_TYPE_DOUBLE, &gpsfix.latitude,
			  DBUS_TYPE_DOUBLE, &gpsfix.longitude,
			  DBUS_TYPE_DOUBLE, &eph,
			  DBUS_TYPE_DOUBLE, &gpsfix.altitude,
			  DBUS_TYPE_DOUBLE, &gpsfix.epv,
			  DBUS_TYPE_DOUBLE, &gpsfix.track,
			  DBUS_TYPE_DOUBLE, &gpsfix.epd,
			  DBUS_TYPE_DOUBLE, &gpsfix.speed,
			  DBUS_TYPE_DOUBLE, &gpsfix.eps,
			  DBUS_TYPE_DOUBLE, &gpsfix.climb,
			  DBUS_TYPE_DOUBLE, &gpsfix.epc,
			  DBUS_TYPE_STRING, &gpsd_devname, DBUS_TYPE_INVALID);

    conditionally_log_fix(&gpsfix);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/*
 * Message dispatching function
 *
 */
static DBusHandlerResult signal_handler(DBusConnection * connection,
					DBusMessage * message)
{
    /* dummy, need to use the variable for some reason */
    connection = NULL;

    if (dbus_message_is_signal(message, "org.gpsd", "fix"))
	return handle_gps_fix(message);
    /*
     * ignore all other messages
     */

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int dbus_mainloop(void)
{
    GMainLoop *mainloop;
    DBusError error;

    mainloop = g_main_loop_new(NULL, FALSE);

    dbus_error_init(&error);
    connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set(&error)) {
	syslog(LOG_CRIT, "%s: %s", error.name, error.message);
	return 3;
    }

    dbus_bus_add_match(connection, "type='signal'", &error);
    if (dbus_error_is_set(&error)) {
	syslog(LOG_CRIT, "unable to add match for signals %s: %s", error.name,
	       error.message);
	return 4;
    }

    if (!dbus_connection_add_filter
	(connection, (DBusHandleMessageFunction) signal_handler, NULL,
	 NULL)) {
	syslog(LOG_CRIT, "unable to register filter with the connection");
	return 5;
    }

    dbus_connection_setup_with_g_main(connection, NULL);

    g_main_loop_run(mainloop);
    return 0;
}

#endif /* DBUS_ENABLE */

/**************************************************************************
 *
 * Doing it with sockets
 *
 **************************************************************************/

static struct fixsource_t source;

static void process(struct gps_data_t *gpsdata)
{
    /* this is where we implement source-device filtering */
    if (gpsdata->dev.path[0] != '\0' && source.device != NULL
	&& strcmp(source.device, gpsdata->dev.path) != 0)
	return;

    conditionally_log_fix(&gpsdata->fix);
}

/*@-mustfreefresh -compdestroy@*/
static int socket_mainloop(void)
{
    fd_set fds;
    struct gps_data_t gpsdata;

    if (gps_open(source.server, source.port, &gpsdata) != 0) {
	(void)fprintf(stderr,
		      "%s: no gpsd running or network error: %d, %s\n",
		      progname, errno, gps_errstr(errno));
	exit(1);
    }

    (void)gps_stream(&gpsdata, WATCH_ENABLE, NULL);

    for (;;) {
	int data;
	struct timeval tv;

	FD_ZERO(&fds);
	FD_SET(gpsdata.gps_fd, &fds);

	tv.tv_usec = 250000;
	tv.tv_sec = 0;
	data = select(gpsdata.gps_fd + 1, &fds, NULL, NULL, &tv);

	if (data == -1) {
	    (void)fprintf(stderr, "%s\n", strerror(errno));
	    break;
	} else if (data) {
	    (void)gps_read(&gpsdata);
	    process(&gpsdata);
	}
    }
    (void)gps_close(&gpsdata);
    return 0;
}
/*@+mustfreefresh +compdestroy@*/

/**************************************************************************
 *
 * Main sequence
 *
 **************************************************************************/

static void usage(void)
{
    fprintf(stderr,
	    "Usage: %s [-V] [-h] [-d] [-i timeout] [-j casoc] [-f filename] [-m minmove] [server[:port:[device]]]\n",
	    progname);
    fprintf(stderr,
	    "\tdefaults to '%s -i 5 -j 0 localhost:2947'\n", progname);
    exit(1);
}

/*@-mustfreefresh -globstate@*/
int main(int argc, char **argv)
{
    int ch;
    bool daemonize = false;

    progname = argv[0];

    logfile = stdout;
    while ((ch = getopt(argc, argv, "dD:f:hi:m:V")) != -1) {
	switch (ch) {
	case 'd':
	    openlog(basename(progname), LOG_PID | LOG_PERROR, LOG_DAEMON);
	    daemonize = true;
	    break;
#ifdef CLIENTDEBUG_ENABLE
	case 'D':
	    debug = atoi(optarg);
	    gps_enable_debug(debug, logfile);
	    break;
#endif /* CLIENTDEBUG_ENABLE */
       case 'f':       /* Output file name. */
            {
                char    fname[PATH_MAX];
                time_t  t;
                size_t  s;

                t = time(NULL);
                s = strftime(fname, sizeof(fname), optarg, localtime(&t));
                if (s == 0) {
                        syslog(LOG_ERR,
                            "Bad template \"%s\", logging to stdout.", optarg);
                        break;
                }
                logfile = fopen(fname, "w");
                if (logfile == NULL)
                        syslog(LOG_ERR,
                            "Failed to open %s: %s, logging to stdout.",
                            fname, strerror(errno));
                break;
            }
	case 'i':		/* set polling interfal */
	    timeout = (time_t) atoi(optarg);
	    if (timeout < 1)
		timeout = 1;
	    if (timeout >= 3600)
		fprintf(stderr,
			"WARNING: track timeout is an hour or more!\n");
	    break;
        case 'm':
	    minmove = (double )atoi(optarg);
	    break;
	case 'V':
	    (void)fprintf(stderr, "gpxlogger revision " REVISION "\n");
	    exit(0);
	default:
	    usage();
	    /* NOTREACHED */
	}
    }

    if (daemonize && logfile == stdout) {
	syslog(LOG_ERR, "Daemon mode with no valid logfile name - exiting.");
	exit(1);
    }

    if (optind < argc) {
	gpsd_source_spec(argv[optind], &source);
    } else
	gpsd_source_spec(NULL, &source);
#if 0
    (void)fprintf(logfile,"<!-- server: %s port: %s  device: %s -->\n",
		 source.server, source.port, source.device);
#endif

    /* initializes the gpsfix data structure */
    gps_clear_fix(&gpsfix);

    /* catch all interesting signals */
    (void)signal(SIGTERM, quit_handler);
    (void)signal(SIGQUIT, quit_handler);
    (void)signal(SIGINT, quit_handler);

    /*@-unrecog@*/
    /* might be time to daemonize */
    if (daemonize) {
	/* not SuS/POSIX portable, but we have our own fallback version */
	if (daemon(0, 0) != 0)
	    (void) fprintf(stderr,"demonization failed: %s\n", strerror(errno));
    }
    /*@+unrecog@*/

    //syslog (LOG_INFO, "---------- STARTED ----------");

    print_gpx_header();

#ifdef DBUS_ENABLE
    /* To force socket use in the default way just give a 'localhost' arg */
    if (optind < argc)
	return socket_mainloop();
    else
	return dbus_mainloop();
#else
    return socket_mainloop();
#endif
}
/*@+mustfreefresh +globstate@*/
