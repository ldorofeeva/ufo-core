/*
 * Copyright (C) 2011-2013 Karlsruhe Institute of Technology
 *
 * This file is part of ufod.
 *
 * ufod is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * ufod is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with ufod.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ufo/ufo.h>


static UfoDaemon *global_daemon = NULL;
static gboolean done = FALSE;

typedef struct {
    gchar *addr;
} Options;

static Options *
opts_parse (gint *argc, gchar ***argv)
{
    GOptionContext *context;
    Options *opts;
    gboolean show_version = FALSE;
    GError *error = NULL;

    opts = g_new0 (Options, 1);

    GOptionEntry entries[] = {
        { "listen", 'l', 0, G_OPTION_ARG_STRING, &opts->addr, "Address to listen on (see http://api.zeromq.org/3-2:zmq-tcp)", NULL },
        { "version",  0, 0, G_OPTION_ARG_NONE, &show_version, "Show version information", NULL },
        { NULL }
    };

    context = g_option_context_new (NULL);
    g_option_context_add_main_entries (context, entries, NULL);

    if (!g_option_context_parse (context, argc, argv, &error)) {
        g_print ("Option parsing failed: %s\n", error->message);
        g_free (opts);
        return NULL;
    }

    if (show_version) {
        g_print ("%s version " UFO_VERSION "\n", argv[0][0]);
        exit (EXIT_SUCCESS);
    }

    if (opts->addr == NULL)
        opts->addr = g_strdup ("tcp://*:5555");

    g_option_context_free (context);

    return opts;
}

static void
opts_free (Options *opts)
{
    g_free (opts->addr);
    g_free (opts);
}

static void
terminate (int signum)
{
    if (signum == SIGTERM)
        g_print ("Received SIGTERM, exiting...\n");

    if (signum == SIGINT)
        g_print ("Received SIGINT, exiting...\n");

    done = TRUE;
}

int
main (int argc, char * argv[])
{
    Options *opts;
    GError *error = NULL;

#if !(GLIB_CHECK_VERSION (2, 36, 0))
    g_type_init ();
#endif

#if !(GLIB_CHECK_VERSION (2, 32, 0))
    g_thread_init (NULL);
#endif

    if ((opts = opts_parse (&argc, &argv)) == NULL)
        return 1;

    /* Now, install SIGTERM/SIGINT handler */
    (void) signal (SIGTERM, terminate);
    (void) signal (SIGINT, terminate);

    global_daemon = ufo_daemon_new (opts->addr, &error);

    if (error != NULL)
        goto error;

    ufo_daemon_start (global_daemon, &error);

    if (error != NULL)
        goto error;

    g_print ("ufod %s - waiting for requests on %s ...\n", UFO_VERSION, opts->addr);

    while (!done)
        g_usleep (G_USEC_PER_SEC);

    ufo_daemon_stop (global_daemon, &error);

error:
    if (error != NULL)
        g_printerr ("Error: %s\n", error->message);

    g_object_unref (global_daemon);
    opts_free (opts);

    return error != NULL ? 1 : 0;
}
