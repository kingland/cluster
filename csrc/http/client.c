#include <runtime.h>
#include <http/http.h>


struct client {
    heap h;
    edb b;
    uuid request;
    http_handler response;
    vector queued;
};


static CONTINUATION_1_4(response_body, client, bag, uuid, buffer, register_read);
static void response_body(client c, bag b, uuid n, buffer in, register_read r)
{
    // xxx - should -  allow plumbing for simple bodies to just dump
    // into the response body
    //    prf("response body %b\n", edb_dump(init, (edb)b));
}


static CONTINUATION_1_1(client_connected, client, endpoint);
static void client_connected(client c, endpoint e)
{
    http_send_request(e->w, c->b, c->request);
    //    cont(c->h, response_body, c)));
    apply(e->r, response_header_parser(c->h, c->response));
}

client open_http_client(heap h, edb b, uuid request, http_handler response)
{
    station a;
    estring host = lookupv((edb)b, request, sym(host));
    client c = allocate(h, sizeof(struct client));
    c->h =h;
    c->b = b;
    c->request = request;
    c->response = response;
#if 0
    estring eurl = lookupv(b, request, sym(url));
    string url = alloca_wrap_buffer(eurl->body, eurl->length);
    // extract the host from the head of the url if not specified
    // fix fako split
    string location = allocate_string(h);

    string_foreach(url, i) {
        if (i == '/') break;
        string_insert(location, i);
    }
#endif
    tcp_create_client (h,
                       station_from_string(h, alloca_wrap_buffer(host->body, host->length)),
                       cont(h, client_connected, c));
    return c;
}
