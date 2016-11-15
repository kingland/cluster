#include <unix_internal.h>


typedef struct commitlog {
    struct bag b;
    bag wrapped;
    buffer stage, writing;
    thunk complete;
    descriptor file;
} *commitlog;

static void swap(commitlog log)
{
    buffer x = log->writing;
    log->writing = log->stage;
    log->stage = x;
}

static void commit_complete(commitlog log)
{
    buffer_clear(log->writing);
    // xxx  - throw on the finalizing log record into the next buffer
    if (buffer_length(log->stage)) {
        swap(log);
        asynch_write(log->file, log->writing, log->complete);
    }
}

static CONTINUATION_1_4(log_prepare, commitlog, edb, edb, ticks, commit_handler);
static void log_prepare(commitlog log, edb source, edb remove, ticks t, commit_handler x)
{
    boolean start = !buffer_length(log->stage) && !buffer_length(log->writing);

    serialize_edb(log->stage, source);

    if (start) {
        swap(log);
        asynch_write(log->file, log->writing, log->complete);
    }
}

static CONTINUATION_3_1(fill, edb, value*, value *, value);
static void fill(edb target, value *e, value *a, value v)
{
    if (!*e) {*e = v; return;}
    if (!*a) {*a = v; return;}
    edb_insert(target, *e, *a, v, 0);
}

// maybe descriptor?
bag start_log(bag base, char *filename)
{
    heap h = allocate_rolling(pages, sstring("log working"));
    commitlog log = allocate(h, sizeof(struct commitlog));
    log->b.prepare = cont(h, log_prepare, log);
    log->b.scan = base->scan;
    log->b.listeners = base->listeners;

    // xxx - move this into a unix specific async file handler
    int fd = open(filename, O_CREAT | O_APPEND | O_RDWR, 0666);
    buffer stage;
    value *e = allocate(h, sizeof(value));
    value *a = allocate(h, sizeof(value));
    *e = *a = (void **)0;
    buffer_handler des = allocate_deserialize(h, cont(h, fill, (edb)base, e, a));

    do {
        // xxx - can use this as an output buffer, des needs two, but not more than two
        stage = allocate_buffer(h, 2048);
        stage->end = read(fd, bref(stage, 0), stage->length);
        // ideally this would be the same buffer, but deserialize
        // can sometimes hang on to them..a guess a freelist is the samea
        apply(des, stage, ignore);
    } while(buffer_length(stage));
    return (bag)log;
}
