/**
 * SECTION:ufo-base-scheduler
 * @Short_description: Data transport between two UfoFilters
 * @Title: UfoBaseScheduler
 */

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif
#include <string.h>

#include "config.h"
#include "ufo-aux.h"
#include "ufo-base-scheduler.h"
#include "ufo-filter.h"
#include "ufo-filter-source.h"
#include "ufo-filter-sink.h"
#include "ufo-filter-reduce.h"
#include "ufo-relation.h"

G_DEFINE_TYPE (UfoBaseScheduler, ufo_base_scheduler, G_TYPE_OBJECT)

#define UFO_BASE_SCHEDULER_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), UFO_TYPE_BASE_SCHEDULER, UfoBaseSchedulerPrivate))

typedef struct {
    gfloat cpu_time;
    gfloat gpu_time;
} ExecutionInformation;

typedef struct {
    cl_command_queue    cmd_queue;
    cl_command_type     cmd_type;
    cl_int              cmd_status;
    cl_ulong            submitted;
    cl_ulong            queued;
    cl_ulong            started;
    cl_ulong            ended;
} cl_event_info_row;

typedef struct {
    UfoFilter          *filter;
    GList              *relations;
    guint               num_cmd_queues;
    cl_command_queue   *cmd_queues;
    guint               num_inputs;
    guint               num_outputs;
    UfoInputParameter  *input_params;
    UfoOutputParameter *output_params;
    guint             **output_dims;
    GAsyncQueue       **input_pop_queues;
    GAsyncQueue       **input_push_queues;
    GAsyncQueue       **output_pop_queues;
    GAsyncQueue       **output_push_queues;
    UfoBuffer         **work;
    UfoBuffer         **result;
    GTimer            **timers;
    GTimer             *cpu_timer;
    cl_event_info_row  *event_rows;
    guint               n_event_rows;
    guint               n_available_event_rows;
} ThreadInfo;

struct _UfoBaseSchedulerPrivate {
    UfoResourceManager  *manager;
    GHashTable          *exec_info;    /**< maps from gpointer to ExecutionInformation* */
};

/**
 * ufo_base_scheduler_new:
 * @manager: A #UfoResourceManager
 *
 * Creates a new #UfoBaseScheduler.
 *
 * Return value: A new #UfoBaseScheduler
 */
UfoBaseScheduler *
ufo_base_scheduler_new (UfoResourceManager *manager)
{
    UfoBaseScheduler *scheduler;

    scheduler = UFO_BASE_SCHEDULER (g_object_new (UFO_TYPE_BASE_SCHEDULER, NULL));
    scheduler->priv->manager = manager;
    g_object_ref (manager);
    return scheduler;
}

static void
push_poison_pill (GList *relations)
{
    g_list_foreach (relations, (GFunc) ufo_relation_push_poison_pill, NULL);
}

static void
alloc_output_buffers (UfoFilter *filter,
                      GAsyncQueue *pop_queues[],
                      guint **output_dims)
{
    UfoOutputParameter *output_params = ufo_filter_get_output_parameters (filter);
    UfoResourceManager *manager = ufo_filter_get_resource_manager (filter);
    const guint num_outputs = ufo_filter_get_num_outputs (filter);

    for (guint port = 0; port < num_outputs; port++) {
        const guint num_dims = output_params[port].n_dims;

        for (guint i = 0; i < num_outputs; i++) {
            UfoBuffer *buffer = ufo_resource_manager_request_buffer (manager, num_dims, output_dims[port], NULL, NULL);

            /*
             * For some reason, if we don't reference the buffer again,
             * it is returned by ufo_buffer_new() for other filters as
             * well, thus sharing the buffer which is not what we want.
             *
             * TODO: remove the reference in a meaningful way, or find
             * out what the problem is.
             */
            g_object_ref (buffer);

            g_async_queue_push (pop_queues[port], buffer);
        }
    }
}

static void
get_input_queues (GList *relations, UfoFilter *filter, GAsyncQueue ***input_push_queues, GAsyncQueue ***input_pop_queues)
{
    const guint num_inputs = ufo_filter_get_num_inputs (filter);
    GAsyncQueue **pop_queues = g_malloc0 (sizeof(GAsyncQueue *) * num_inputs);
    GAsyncQueue **push_queues = g_malloc0 (sizeof(GAsyncQueue *) * num_inputs);

    for (GList *it = g_list_first (relations); it != NULL; it = g_list_next (it)) {
        UfoRelation *relation = UFO_RELATION(it->data);

        if (ufo_relation_has_consumer (relation, filter)) {
            guint port = ufo_relation_get_consumer_port (relation, filter);
            ufo_relation_get_consumer_queues (relation, filter, &push_queues[port], &pop_queues[port]);
        }
    }

    *input_push_queues = push_queues;
    *input_pop_queues = pop_queues;
}

static void
get_output_queues (GList *relations, UfoFilter *filter, GAsyncQueue ***output_push_queues, GAsyncQueue ***output_pop_queues)
{
    const guint num_outputs = ufo_filter_get_num_outputs (filter);
    GAsyncQueue **pop_queues = g_malloc0 (sizeof (GAsyncQueue *) * num_outputs);
    GAsyncQueue **push_queues = g_malloc0 (sizeof (GAsyncQueue *) * num_outputs);

    for (GList *it = g_list_first (relations); it != NULL; it = g_list_next (it)) {
        UfoRelation *relation = UFO_RELATION (it->data);

        if (filter == ufo_relation_get_producer (relation)) {
            guint port = ufo_relation_get_producer_port (relation);
            ufo_relation_get_producer_queues (relation, &push_queues[port], &pop_queues[port]);
        }
    }

    *output_push_queues = push_queues;
    *output_pop_queues = pop_queues;
}

static void
print_prefixed (const gchar *text, ThreadInfo *info)
{
    /* g_debug ("%s-%p:%s", ufo_filter_get_plugin_name (info->filter), (gpointer) info->filter, text); */
}

static gboolean
fetch_work (ThreadInfo *info)
{
    const gpointer POISON_PILL = GINT_TO_POINTER (1);
    gboolean success = TRUE;
    print_prefixed ("fetch:work", info);

    for (guint i = 0; i < info->num_inputs; i++) {
        if ((info->input_params[i].n_expected_items == UFO_FILTER_INFINITE_INPUT) ||
            (info->input_params[i].n_fetched_items < info->input_params[i].n_expected_items))
        {
            info->work[i] = g_async_queue_pop (info->input_pop_queues[i]);
            info->input_params[i].n_fetched_items++;
        }

        if (info->work[i] == POISON_PILL) {
            g_async_queue_push (info->input_push_queues[i], POISON_PILL);
            success = FALSE;
        }
    }

    print_prefixed ("fetch:done", info);
    return success;
}

static void
cleanup_fetched (ThreadInfo *info)
{
    for (guint i = 0; i < info->num_inputs; i++) {
        if ((info->input_params[i].n_fetched_items == info->input_params[i].n_expected_items))
            g_async_queue_push (info->input_push_queues[i], info->work[i]);
    }
}

static void
push_work (ThreadInfo *info)
{
    print_prefixed ("release:work", info);

    for (guint i = 0; i < info->num_inputs; i++) {
        if ((info->input_params[i].n_expected_items == UFO_FILTER_INFINITE_INPUT) ||
            (info->input_params[i].n_fetched_items < info->input_params[i].n_expected_items))
        {
            g_async_queue_push (info->input_push_queues[i], info->work[i]);
        }
    }

    print_prefixed ("release:done", info);
}

static void
fetch_result (ThreadInfo *info)
{
    print_prefixed ("fetch:result", info);

    for (guint port = 0; port < info->num_outputs; port++)
        info->result[port] = g_async_queue_pop (info->output_pop_queues[port]);

    print_prefixed ("fetch:done", info);
}

static void
push_result (ThreadInfo *info)
{
    print_prefixed ("release:result", info);

    for (guint port = 0; port < info->num_outputs; port++)
        g_async_queue_push (info->output_push_queues[port], info->result[port]);

    print_prefixed ("release:done", info);
}

static void
log_cl_event (cl_event *event_location, ThreadInfo *info)
{
    cl_event event;
    cl_event_info_row *row;

    event = *event_location;

    if (event == NULL)
        return;

    row = &info->event_rows[info->n_event_rows];
    CHECK_OPENCL_ERROR (clGetEventInfo (event, CL_EVENT_COMMAND_QUEUE, sizeof (cl_command_queue), &row->cmd_queue, NULL));
    CHECK_OPENCL_ERROR (clGetEventInfo (event, CL_EVENT_COMMAND_TYPE, sizeof (cl_command_type), &row->cmd_type, NULL));
    CHECK_OPENCL_ERROR (clGetEventInfo (event, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof (cl_int), &row->cmd_status, NULL));

    if (row->cmd_status == CL_COMPLETE) {
        CHECK_OPENCL_ERROR (clGetEventProfilingInfo (event, CL_PROFILING_COMMAND_QUEUED, sizeof (cl_ulong), &row->queued, NULL));
        CHECK_OPENCL_ERROR (clGetEventProfilingInfo (event, CL_PROFILING_COMMAND_SUBMIT, sizeof (cl_ulong), &row->submitted, NULL));
        CHECK_OPENCL_ERROR (clGetEventProfilingInfo (event, CL_PROFILING_COMMAND_START, sizeof (cl_ulong), &row->started, NULL));
        CHECK_OPENCL_ERROR (clGetEventProfilingInfo (event, CL_PROFILING_COMMAND_END, sizeof (cl_ulong), &row->ended, NULL));
    }

    info->n_event_rows++;

    if (info->n_event_rows == info->n_available_event_rows) {
        info->n_available_event_rows *= 2;
        info->event_rows = g_realloc_n (info->event_rows, info->n_available_event_rows, sizeof (cl_event_info_row));
    }
}

static GError *
process_source_filter (ThreadInfo *info)
{
    UfoFilter   *filter = UFO_FILTER (info->filter);
    GError      *error = NULL;
    gboolean     cont = TRUE;

    ufo_filter_source_initialize (UFO_FILTER_SOURCE (filter), info->output_dims, &error);

    if (error != NULL)
        return error;

    alloc_output_buffers (filter,
                          info->output_pop_queues,
                          info->output_dims);

    while (cont) {
        fetch_result (info);

        g_timer_continue (info->cpu_timer);
        cont = ufo_filter_source_generate (UFO_FILTER_SOURCE (filter), info->result, info->cmd_queues[0], &error);
        g_timer_stop (info->cpu_timer);

        if (error != NULL)
            return error;

        if (cont)
            push_result (info);
    }

    return NULL;
}

static GError *
process_synchronous_filter (ThreadInfo *info)
{
    UfoFilter *filter = UFO_FILTER (info->filter);
    UfoFilterClass *filter_class = UFO_FILTER_GET_CLASS (filter);
    GError *error = NULL;
    gboolean cont = TRUE;

    /*
     * Initialize
     */
    if (fetch_work (info)) {
        print_prefixed ("init", info);
        ufo_filter_initialize (filter, info->work, info->output_dims, &error);
        print_prefixed ("init:done", info);

        if (error != NULL) {
            g_error ("%s", error->message);
            return error;
        }

        alloc_output_buffers (filter,
                              info->output_pop_queues,
                              info->output_dims);
    }
    else {
        return NULL;
    }

    fetch_result (info);

    while (cont) {
        if (filter_class->process_gpu != NULL) {
            UfoEventList *events;
            GList *list;

            g_timer_continue (info->cpu_timer);
            events = ufo_filter_process_gpu (filter, info->work, info->result, info->cmd_queues[0], &error);
            g_timer_stop (info->cpu_timer);

            if (events != NULL) {
                list = ufo_event_list_get_list (events);
                g_list_foreach (list, (GFunc) log_cl_event, info);
                ufo_event_list_free (events);
            }
        }
        else {
            g_timer_continue (info->cpu_timer);
            ufo_filter_process_cpu (filter, info->work, info->result, info->cmd_queues[0], &error);
            g_timer_stop (info->cpu_timer);
        }

        if (error != NULL)
            return error;

        push_work (info);
        push_result (info);

        fetch_result (info);
        cont = fetch_work (info);
    }

    /*
     * In case this buffer keeps some of its inputs (if n_expected_items <
     * UFO_FILTER_INPUT_INFINITE), then we need to return those buffers to the
     * preceding filter. Otherwise we get stuck.
     */
    cleanup_fetched (info);

    return NULL;
}

static GError *
process_sink_filter (ThreadInfo *info)
{
    UfoFilter *filter = UFO_FILTER (info->filter);
    UfoFilterSink *sink_filter = UFO_FILTER_SINK (filter);
    GError *error = NULL;
    gboolean cont = TRUE;

    if (fetch_work (info)) {
        ufo_filter_sink_initialize (sink_filter, info->work, &error);

        if (error != NULL)
            return error;
    }
    else
        return NULL;

    while (cont) {
        g_timer_continue (info->cpu_timer);
        ufo_filter_sink_consume (sink_filter, info->work, info->cmd_queues[0], &error);
        g_timer_stop (info->cpu_timer);

        if (error != NULL)
            return error;

        push_work (info);
        cont = fetch_work (info);
    }

    return NULL;
}

static GError *
process_reduce_filter (ThreadInfo *info)
{
    UfoFilter *filter = UFO_FILTER (info->filter);
    GError *error = NULL;
    gboolean cont;
    gfloat default_value = 0.0f;

    if (fetch_work (info)) {
        ufo_filter_reduce_initialize (UFO_FILTER_REDUCE (filter), info->work, info->output_dims, &default_value, &error);

        if (error != NULL)
            return error;

        alloc_output_buffers (filter,
                              info->output_pop_queues,
                              info->output_dims);
    }
    else {
        return NULL;
    }

    /*
     * Fetch the first result buffers and initialize them with the requested
     * default value. These buffer will be used throughout the collection phase.
     */
    fetch_result (info);

    for (guint i = 0; i < info->num_outputs; i++)
        ufo_buffer_fill_with_value (info->result[i], default_value);

    /*
     * Collect until no more data is available for consumption. The filter is
     * given the same result buffers, so that results can be accumulated.
     */
    cont = TRUE;

    while (cont) {
        g_timer_continue (info->cpu_timer);
        ufo_filter_reduce_collect (UFO_FILTER_REDUCE (filter), info->work, info->result, info->cmd_queues[0], &error);
        g_timer_stop (info->cpu_timer);

        if (error != NULL)
            return error;

        push_work (info);
        cont = fetch_work (info);
    }

    /*
     * Calculate reduction results until the filter finishes.
     */
    cont = TRUE;

    while (cont) {
        g_timer_continue (info->cpu_timer);
        cont = ufo_filter_reduce_reduce (UFO_FILTER_REDUCE (filter),
                                         info->result,
                                         info->cmd_queues[0],
                                         &error);
        g_timer_stop (info->cpu_timer);

        if (error != NULL)
            return error;

        if (cont) {
            push_result (info);
            fetch_result (info);
        }
    }

    return error;
}

static void
alloc_info_data (ThreadInfo *info)
{
    info->work = g_malloc (info->num_inputs * sizeof (UfoBuffer *));
    info->result = g_malloc (info->num_outputs * sizeof (UfoBuffer *));
    info->timers = g_malloc (info->num_inputs * sizeof (GTimer *));
    info->output_dims = g_malloc0(info->num_outputs * sizeof (guint *));
}

static gpointer
process_thread (gpointer data)
{
    GError *error = NULL;
    ThreadInfo *info = (ThreadInfo *) data;
    UfoFilter *filter = info->filter;

    g_object_ref (filter);

    UfoOutputParameter *output_params = ufo_filter_get_output_parameters (filter);
    GList *producing_relations = NULL;

    info->num_inputs = ufo_filter_get_num_inputs (filter);
    info->num_outputs = ufo_filter_get_num_outputs (filter);
    info->input_params = ufo_filter_get_input_parameters (filter);
    info->output_params = ufo_filter_get_output_parameters (filter);

    for (guint i = 0; i < info->num_inputs; i++)
        info->input_params[i].n_fetched_items = 0;

    get_input_queues (info->relations, filter, &info->input_push_queues, &info->input_pop_queues);
    get_output_queues (info->relations, filter, &info->output_push_queues, &info->output_pop_queues);

    alloc_info_data (info);

    for (guint i = 0; i < info->num_outputs; i++)
        info->output_dims[i] = g_malloc0 (output_params[i].n_dims * sizeof(guint));

    /*
     * Find out, in which relations we are involved with this filter. This is
     * rather ugly and should be refactored at a later time.
     */
    for (GList *it = g_list_first (info->relations); it != NULL; it = g_list_next (it)) {
        UfoRelation *relation = UFO_RELATION (it->data);

        if (filter == ufo_relation_get_producer (relation))
            producing_relations = g_list_append (producing_relations, relation);
    }

    if (UFO_IS_FILTER_SOURCE (filter))
        error = process_source_filter (info);
    else if (UFO_IS_FILTER_SINK (filter))
        error = process_sink_filter (info);
    else if (UFO_IS_FILTER_REDUCE (filter))
        error = process_reduce_filter (info);
    else
        error = process_synchronous_filter (info);

    /*
     * In case of an error something really bad happened and data is corrupted
     * anyway, so don't bother with freeing resources. We need to show the error
     * ASAP...
     */
    if (error != NULL)
        return error;

    g_message ("UfoBaseScheduler: %s-%p finished", ufo_filter_get_plugin_name (filter), filter);

    push_poison_pill (producing_relations);

    g_free (info->output_dims);
    g_free (info->work);
    g_free (info->result);
    g_free (info->timers);
    g_free (info->input_pop_queues);
    g_free (info->input_push_queues);
    g_free (info->output_pop_queues);
    g_free (info->output_push_queues);
    g_list_free (producing_relations);
    g_object_unref (filter);

    return error;
}

/**
 * ufo_base_scheduler_run:
 * @scheduler: A #UfoBaseScheduler object
 * @relations: (in) (element-type UfoRelation): A list of all relations that should be scheduled
 * @error: return location for a GError with error codes from
 * #UfoPluginManagerError or %NULL
 *
 * Start executing all filters from the @relations list in their own threads.
 */
void ufo_base_scheduler_run (UfoBaseScheduler *scheduler, GList *relations, GError **error)
{
    cl_command_queue *cmd_queues;
    GTimer      *timer;
    GList       *filters;
    GThread    **threads;
    GError      *tmp_error = NULL;
    ThreadInfo  *thread_info;
    guint        n_queues;
    guint        n_threads;
    guint        i = 0;

    g_return_if_fail (UFO_IS_BASE_SCHEDULER (scheduler));

    ufo_resource_manager_get_command_queues (scheduler->priv->manager, (void **) &cmd_queues, &n_queues);

    /*
     * Unfortunately, there is no set data type in GLib. Thus we have to write
     * this here.
     */
    GHashTable *filter_set = g_hash_table_new (g_direct_hash, g_direct_equal);

    for (GList *it = g_list_first (relations); it != NULL; it = g_list_next (it)) {
        UfoRelation *relation   = UFO_RELATION (it->data);
        UfoFilter   *producer   = ufo_relation_get_producer (relation);
        GList       *consumers  = ufo_relation_get_consumers (relation);

        g_hash_table_insert (filter_set, producer, NULL);

        for (GList *jt = g_list_first (consumers); jt != NULL; jt = g_list_next (jt))
            g_hash_table_insert (filter_set, jt->data, NULL);
    }

    filters     = g_hash_table_get_keys (filter_set);
    n_threads   = g_list_length (filters);
    threads     = g_new0 (GThread *, n_threads);
    thread_info = g_new0 (ThreadInfo, n_threads);

    g_thread_init (NULL);
    timer = g_timer_new ();

    /*
     * Start each filter in its own thread
     */
    for (GList *it = filters; it != NULL; it = g_list_next (it), i++) {
        UfoFilter *filter = UFO_FILTER (it->data);

        thread_info[i].filter = filter;
        thread_info[i].relations = relations;
        thread_info[i].num_cmd_queues = n_queues;
        thread_info[i].cmd_queues = cmd_queues;
        thread_info[i].n_available_event_rows = 256;
        thread_info[i].event_rows = g_new0 (cl_event_info_row, thread_info[i].n_available_event_rows);
        thread_info[i].n_event_rows = 0;

        thread_info[i].cpu_timer = g_timer_new ();
        g_timer_stop (thread_info[i].cpu_timer);

        threads[i] = g_thread_create (process_thread, &thread_info[i], TRUE, &tmp_error);

        if (tmp_error != NULL) {
            /* FIXME: kill already started threads */
            g_propagate_error (error, tmp_error);
            return;
        }
    }

    /*
     * Wait for them to finish
     */
    for (i = 0; i < n_threads; i++) {
        /* UfoFilter *filter; */
        /* ThreadInfo *info; */

        tmp_error = (GError *) g_thread_join (threads[i]);

        if (tmp_error != NULL) {
            g_propagate_error (error, tmp_error);
            return;
        }

        /* info = &thread_info[i]; */
        /* filter = info->filter; */
        /* g_print ("%s-%p\n", ufo_filter_get_plugin_name (filter), (gpointer) filter); */

        /* for (guint row = 0; row < info->n_event_rows; row++) { */
        /*     cl_event_info_row *event_row = &info->event_rows[row]; */
        /*     g_print ("%lu %lu %lu %lu\n", event_row->queued, event_row->submitted, event_row->started, event_row->ended); */
        /* } */
    }

    /* TODO: free the cpu timers */

    g_free (thread_info);
    g_free (threads);
    g_list_free (filters);
    g_hash_table_destroy (filter_set);

    g_timer_stop (timer);
    g_message ("Processing finished after %3.5f seconds", g_timer_elapsed (timer, NULL));
    g_timer_destroy (timer);
}

static void ufo_base_scheduler_dispose (GObject *object)
{
    UfoBaseSchedulerPrivate *priv = UFO_BASE_SCHEDULER_GET_PRIVATE (object);
    g_object_unref (priv->manager);
    g_hash_table_destroy (priv->exec_info);
    G_OBJECT_CLASS (ufo_base_scheduler_parent_class)->dispose (object);
    g_message ("UfoBaseScheduler: disposed");
}

static void ufo_base_scheduler_finalize (GObject *object)
{
    G_OBJECT_CLASS (ufo_base_scheduler_parent_class)->finalize (object);
    g_message ("UfoBaseScheduler: finalized");
}

static void ufo_base_scheduler_class_init (UfoBaseSchedulerClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    gobject_class->dispose = ufo_base_scheduler_dispose;
    gobject_class->finalize = ufo_base_scheduler_finalize;
    g_type_class_add_private (klass, sizeof (UfoBaseSchedulerPrivate));
}

static void ufo_base_scheduler_init (UfoBaseScheduler *base_scheduler)
{
    UfoBaseSchedulerPrivate *priv;
    base_scheduler->priv = priv = UFO_BASE_SCHEDULER_GET_PRIVATE (base_scheduler);
    priv->exec_info = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
}
