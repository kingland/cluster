#include <unix_internal.h>

static table file_attributes = 0;

typedef struct file *file;

struct file {
    // inno, etc?
    uuid u;
    estring name;
    // stability
    table children;
    file parent;
};

typedef struct filebag {
    struct bag b;
    heap h;
    file root;
    table idmap;
} *filebag;

#ifndef MAXNAMLEN
#define MAXNAMLEN NAME_MAX
#endif // foo

static void name_file(heap h, file f, estring name)
{
    // xxx should allow renames, links, deletia
    f->name = name;
    if (f->parent) {
        if (!f->parent->children)
            f->parent->children = create_value_table(h);

        table_set(f->parent->children, name, f);
    }
}

static file allocate_file(filebag fb, file parent, uuid u)
{
    // should allow reparenting using rename()
    // and link()
    file f = allocate(fb->h, sizeof(struct file));
    f->u = u;
    f->name = 0;
    f->parent = parent;
    f->children = 0;
    table_set(fb->idmap, f->u, f);
    return f;
}

#define path_of_file(__f)\
    ({\
    int count = 0;\
    for (file __x = __f; __x && __x->name && (count += __x->name->length + (count?1:0)) ;  __x = __x->parent); \
    char *result = alloca(count);\
    result[count]=0;\
    for (file __x = __f;\
         __x && memcpy(result + (count -= __x->name->length), __x->name->body, __x->name->length), (count? result[--count]='/':0) ; \
         __x = __x->parent);\
    result;\
    })

static void fill_children(filebag fb, file f)
{
    foreach_file(path_of_file(f), name, len) {
        if (name[0] != '.') {
            file child;
            estring cname = intern_string((unsigned char *)name, len);
            if ((!f->children) || !(child = table_find(f->children, cname)))
                name_file(fb->h, allocate_file(fb, f, generate_uuid()), cname);
        }
    }
}

static boolean filebag_eav_check(filebag fb, file f, struct stat *s, listener out, value e, value a, value v)
{
    if ((a == sym(length)) && ((u64)*(double *)v) == s->st_size) return true;
    if ((a == sym(name)) && (f->name ==v)) return true;
    if (a == sym(child)) {
        fill_children(fb, f);
        if(f->children && table_find(f->children, v)) return true;
    }
    //    if (a == sym(contents))
    //    if (a == sym(owner)) {
    return false;
}

static void filebag_ea_scan(filebag fb, file f, struct stat *s, listener out, value e, value a)
{
    if (a == sym(length)) {
        apply(out, e, a, box_float(s->st_size), 0);
        return;
    }
    if (a == sym(name)) {
        apply(out, e, a, f->name, 0);
        return;
    }
    if (a == sym(child)) {

        if (f->children) {
            fill_children(fb, f);
            table_foreach(f->children, _, c) {
                apply(out, e, a, ((file)c)->u, 0);
            }
        }
    }
    if (a == sym(contents)) {
        buffer x = read_file(fb->h, path_of_file(f));
        if (x) apply(out, e, a, intern_buffer(x), 0);
        return;
    }
    // also struct tiespec st_mtimespec
    if (a == sym(owner)) {
        struct passwd *p = getpwuid(s->st_uid);
        if (p)
            apply(out, e, a, intern_cstring(p->pw_name), 0);
    }
}


static void filebag_e_scan(filebag fb, file f, listener out, value e, value a)
{
    struct stat st;
    if (stat(path_of_file(f), &st) == 0) {
        table_foreach(file_attributes, k, _)
            filebag_ea_scan(fb, f, &st, out, e, a);
    }
}


// xxx - we're not scanning forward here X child Y
static void dump_tree(file f, listener out)
{
    table_foreach(f->children, _, c) {
        file child = c;
        apply(out, f->u, sym(child), child->u, 0);
        // xxx - cycles
        if (child->children) dump_tree(c, out);
    }
}

static CONTINUATION_1_5(filebag_scan, filebag, int, listener, value, value, value);
static void filebag_scan(filebag fb, int sig, listener out, value e, value a, value v)
{
    if (sig & e_sig) {
        file f = table_find(fb->idmap, e);
        if (f) {
            struct stat st;
            int res = stat(path_of_file(f), &st);
            if (sig & a_sig) {
                if (sig & v_sig) {
                    if (((a == sym(tag)) && (v == sym(root)) && (e == fb->root->u)) ||
                        filebag_eav_check(fb, f, &st, out, e, a, v)) {
                        apply(out, e, a, v, 0);
                    }
                } else {
                    if (S_ISDIR(st.st_mode)) fill_children(fb, f);
                    filebag_ea_scan(fb, f, &st, out, e, a);
                }
            } else {
                struct stat st;
                stat(path_of_file(f), &st);
                table_foreach(file_attributes, a, _)
                    filebag_ea_scan(fb, f, &st, out, e, a);
            }
        }
    } else {
        if ((sig == s_eAV) && (a == sym(tag)) && (v == sym(root))) {
            apply(out, fb->root->u, a, v, 0);
        }
        if ((sig == s_eAv) && (a ==sym(child))) {
            fill_children(fb, fb->root);
            dump_tree(fb->root, out);
        }
    }
    // silently drop all inquries about free entities...we can filter on attribute, but value..man..
}


static CONTINUATION_1_4(filebag_prepare, filebag, edb, edb, ticks, commit_handler);
static void filebag_prepare(filebag fb, edb s, edb remove, ticks t, commit_handler c)
{
    edb_foreach_ev(s, e, sym(child), v) {
        file parent;
        if ((parent = table_find(fb->idmap, e))){
            allocate_file(fb, parent, v);
        }
    }

    edb_foreach_ev(s, e, sym(name), v) {
        file f;
        if ((f = table_find(fb->idmap, e))) {
            name_file(fb->h, f, v);
        }
    }

    edb_foreach_ev(s, e, sym(contents), v) {
        file f;
        estring contents = v;
        // xxx ordering
        if ((f = table_find(fb->idmap, e)) && (f->name)){
            descriptor fd = open(path_of_file(f), O_WRONLY|O_CREAT|O_TRUNC, 0666);
            int res = write(fd, contents->body, contents->length);
            if (res != contents->length) {
                prf("file write failed\n");
            }
            close(fd);
        }
    }
}

bag filebag_init(buffer root_pathname)
{
    if (!file_attributes) {
        file_attributes = create_value_table(init);
        table_set(file_attributes, sym(length), (void *)1);
        table_set(file_attributes, sym(owner), (void *)1);
        table_set(file_attributes, sym(name), (void *)1);
    }
    heap h = allocate_rolling(init, sstring("filebag"));
    filebag fb = allocate(h, sizeof(struct filebag));
    fb->h = h;
    fb->b.scan = cont(h, filebag_scan, fb);
    //    fb->b.u = generate_uuid();
    fb->idmap = create_value_table(h);
    fb->root = allocate_file(fb, 0, generate_uuid());
    fb->root->name = intern_buffer(root_pathname);
    fb->b.listeners = allocate_table(h, key_from_pointer, compare_pointer);
    fb->b.prepare = cont(h, filebag_prepare, fb);
    return (bag)fb;
}
