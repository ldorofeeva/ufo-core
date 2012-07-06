/**
 * SECTION:ufo-filter
 * @Short_description: Single unit of computation
 * @Title: UfoFilterSink
 */

#include <glib.h>
#include <gmodule.h>
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#include "config.h"
#include "ufo-filter-sink.h"

G_DEFINE_TYPE (UfoFilterSink, ufo_filter_sink, UFO_TYPE_FILTER)

#define UFO_FILTER_SINK_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), UFO_TYPE_FILTER_SINK, UfoFilterSinkPrivate))

void
ufo_filter_sink_initialize (UfoFilterSink *filter, UfoBuffer *work[], GError **error)
{
    g_return_if_fail (UFO_IS_FILTER_SINK (filter));
    UFO_FILTER_SINK_GET_CLASS (filter)->initialize (filter, work, error);
}

void
ufo_filter_sink_consume (UfoFilterSink  *filter, UfoBuffer *work[], gpointer cmd_queue, GError **error)
{
    g_return_if_fail (UFO_IS_FILTER_SINK (filter));
    UFO_FILTER_SINK_GET_CLASS (filter)->consume (filter, work, cmd_queue, error);
}

static void
ufo_filter_sink_initialize_real (UfoFilterSink *filter, UfoBuffer *work[], GError **error)
{
    g_debug ("%s->initialize not implemented", ufo_filter_get_plugin_name (UFO_FILTER (filter)));
}

static void
ufo_filter_sink_consume_real (UfoFilterSink *filter, UfoBuffer *work[], gpointer cmd_queue, GError **error)
{
    g_set_error (error, UFO_FILTER_ERROR, UFO_FILTER_ERROR_METHOD_NOT_IMPLEMENTED,
            "Virtual method `consume` of %s is not implemented",
            ufo_filter_get_plugin_name (UFO_FILTER (filter)));
}

static void
ufo_filter_sink_class_init(UfoFilterSinkClass *klass)
{
    klass->initialize = ufo_filter_sink_initialize_real;
    klass->consume = ufo_filter_sink_consume_real;
}

static void
ufo_filter_sink_init(UfoFilterSink *self)
{
    self->priv = UFO_FILTER_SINK_GET_PRIVATE (self);
}
