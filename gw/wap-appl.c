/*
 * gw/wap-appl.c - wapbox application layer and push ota indication, response
 * and confirmation primitive implementation.
 *
 * This module implements indication and confirmation primitives of WAP-189-
 * PushOTA-20000217-a (hereafter called ota). 
 * In addition, WAP-200-WDP-20001212-a (wdp) is referred.
 * Wapbox application layer is not a Wapforum protocol. 
 *
 * The application layer is reads events from its event queue, fetches the 
 * corresponding URLs and feeds back events to the WSP layer (pull). 
 *
 * In addition, the layer forwards WSP events related to push to the module 
 * wap_push_ppg and wsp, implementing indications, responses  and confirma-
 * tions of ota.
 *
 * Note that push header encoding and decoding are divided two parts:
 * first decoding and encoding numeric values and then packing these values
 * into WSP format and unpacking them from WSP format. This module contains
 * encoding part.
 *
 * Lars Wirzenius
 */

#include <string.h>

#include "gwlib/gwlib.h"
#include "wmlscript/ws.h"
#include "xml_shared.h"
#include "wml_compiler.h"
#include "wap/wap.h"
#include "wap-appl.h"
#include "wap_push_ppg.h"
#include "wap/wsp_strings.h"
#include "wap/wsp_caps.h"
#include "wap/wsp.h"
#ifdef ENABLE_COOKIES
#include "wap/cookies.h"
#endif
#include "wap-error.h"

/*
 * Give the status the module:
 *
 *	limbo
 *		not running at all
 *	running
 *		operating normally
 *	terminating
 *		waiting for operations to terminate, returning to limbo
 */
static enum { limbo, running, terminating } run_status = limbo;


/*
 * The queue of incoming events.
 */
static List *queue = NULL;


/*
 * HTTP caller identifier for application layer.
 */
static HTTPCaller *caller = NULL;


/*
 * Number of currently running HTTP fetching threads.
 */
static Counter *fetches = NULL;


/*
 * Charsets supported by WML compiler, queried from wml_compiler.
 */
static List *charsets = NULL;

struct content {
    Octstr *body;
    Octstr *type;
    Octstr *charset;
    Octstr *url;
};


/*
 * A mapping from HTTP request identifiers to information about the request.
 */
struct request_data {
    long client_SDU_size;
    WAPEvent *event;
    long session_id;
    Octstr *url;
    long x_wap_tod;
    List *request_headers;
};


/*
 * WSP smart error messaging
 */
extern int wsp_smart_errors;
extern Octstr *device_home;

/*
 *
 */
static int have_ppg = 0;

/*
 * Private functions.
 */

static void main_thread(void *);
static void start_fetch(WAPEvent *);
static void return_replies_thread(void *);

static void  dev_null(const char *data, size_t len, void *context);

static Octstr *convert_wml_to_wmlc(struct content *content);
static Octstr *convert_wmlscript_to_wmlscriptc(struct content *content);
static void wsp_http_map_url(Octstr **osp);
static List *negotiate_capabilities(List *req_caps);

static struct {
    char *type;
    char *result_type;
    Octstr *(*convert)(struct content *);
} converters[] = {
    { "text/vnd.wap.wml",
      "application/vnd.wap.wmlc",
      convert_wml_to_wmlc },
    { "text/vnd.wap.wmlscript",
      "application/vnd.wap.wmlscriptc",
      convert_wmlscript_to_wmlscriptc },
};
#define NUM_CONVERTERS ((long)(sizeof(converters) / sizeof(converters[0])))

/*
 * Following functions implement indications and conformations part of Push
 * OTA protocol.
 */
static void indicate_push_connection(WAPEvent *e);
static void indicate_push_disconnect(WAPEvent *e);
static void indicate_push_suspend(WAPEvent *e);
static void indicate_push_resume(WAPEvent *e);
static void confirm_push(WAPEvent *e);
static void indicate_push_abort(WAPEvent *e);
static void split_header_list(List **headers, List **new_headers, char *name);
static void check_application_headers(List **headers, List **app_headers);
static void decode_bearer_indication(List **headers, List **bearer_headers);
static void response_push_connection(WAPEvent *e);

/***********************************************************************
 * The public interface to the application layer.
 */

void wap_appl_init(Cfg *cfg) 
{
    gw_assert(run_status == limbo);
    queue = list_create();
    fetches = counter_create();
    list_add_producer(queue);
    run_status = running;
    charsets = wml_charsets();
    caller = http_caller_create();
    gwthread_create(main_thread, NULL);
    gwthread_create(return_replies_thread, NULL);

    if (cfg != NULL)
        have_ppg = 1;
    else
        have_ppg = 0;
}


void wap_appl_shutdown(void) 
{
    gw_assert(run_status == running);
    run_status = terminating;
    
    list_remove_producer(queue);
    gwthread_join_every(main_thread);
    
    http_caller_signal_shutdown(caller);
    gwthread_join_every(return_replies_thread);
    
    http_caller_destroy(caller);
    list_destroy(queue, wap_event_destroy_item);
    list_destroy(charsets, octstr_destroy_item);
    counter_destroy(fetches);
}


void wap_appl_dispatch(WAPEvent *event) 
{
    gw_assert(run_status == running);
    list_produce(queue, event);
}


long wap_appl_get_load(void) 
{
    gw_assert(run_status == running);
    return counter_value(fetches) + list_len(queue);
}


/***********************************************************************
 * Private functions.
 */

/*
 * When we have a push event, create ota indication or confirmation and send 
 * it to ppg module. 
 * Because Accept-Application and Bearer-Indication are optional, we cannot 
 * rely on them. We must ask ppg main module do we have an open push session 
 * for this initiator. Push is identified by push id.
 * If there is no ppg configured, do not refer to ppg's sessions' list.
 */
static void main_thread(void *arg) 
{
    WAPEvent *ind, *res;
    long sid;
    WAPAddrTuple *tuple;
    
    while (run_status == running && (ind = list_consume(queue)) != NULL) {
	switch (ind->type) {
	case S_MethodInvoke_Ind:
	    res = wap_event_create(S_MethodInvoke_Res);
	    res->u.S_MethodInvoke_Res.server_transaction_id =
	    ind->u.S_MethodInvoke_Ind.server_transaction_id;
	    res->u.S_MethodInvoke_Res.session_id =
	    ind->u.S_MethodInvoke_Ind.session_id;
	    wsp_session_dispatch_event(res);
	    start_fetch(ind);
	    break;
	
	case S_Unit_MethodInvoke_Ind:
	    start_fetch(ind);
	    break;

	case S_Connect_Ind:
            tuple  = ind->u.S_Connect_Ind.addr_tuple;

            if (have_ppg && wap_push_ppg_have_push_session_for(tuple)) {
	        indicate_push_connection(ind);
            } else {
	        res = wap_event_create(S_Connect_Res);
	    /* FIXME: Not yet used by WSP layer */
	       res->u.S_Connect_Res.server_headers = NULL;
	       res->u.S_Connect_Res.negotiated_capabilities =
	           negotiate_capabilities(
	               ind->u.S_Connect_Ind.requested_capabilities);
	       res->u.S_Connect_Res.session_id = 
                   ind->u.S_Connect_Ind.session_id;
	       wsp_session_dispatch_event(res);
            }

            wap_event_destroy(ind);
            break;
	
	case S_Disconnect_Ind:
	    sid = ind->u.S_Disconnect_Ind.session_handle;

            if (have_ppg && wap_push_ppg_have_push_session_for_sid(sid)) 
                indicate_push_disconnect(ind);
	    wap_event_destroy(ind);
	    break;

	case S_Suspend_Ind:
	    sid = ind->u.S_Suspend_Ind.session_id;

            if (wap_push_ppg_have_push_session_for_sid(sid)) 
                indicate_push_suspend(ind);
	    wap_event_destroy(ind);
	    break;

	case S_Resume_Ind:
	    sid = ind->u.S_Resume_Ind.session_id;

            if (have_ppg && wap_push_ppg_have_push_session_for_sid(sid)) 
                indicate_push_resume(ind);
            else {
	        res = wap_event_create(S_Resume_Res);
	        res->u.S_Resume_Res.server_headers = NULL;
	        res->u.S_Resume_Res.session_id = 
                    ind->u.S_Resume_Ind.session_id;
	        wsp_session_dispatch_event(res);
	        
            }
            wap_event_destroy(ind);
	    break;
	
	case S_MethodResult_Cnf:
	    wap_event_destroy(ind);
	    break;

        case S_ConfirmedPush_Cnf:
            confirm_push(ind);
            wap_event_destroy(ind);
	    break;
	
	case S_MethodAbort_Ind:
	    /* XXX Interrupt the fetch thread somehow */
	    wap_event_destroy(ind);
	    break;

        case S_PushAbort_Ind:
            indicate_push_abort(ind);
            wap_event_destroy(ind);
	    break;

        case Pom_Connect_Res:
	    response_push_connection(ind);
	    wap_event_destroy(ind);
	    break;
	
	default:
	    panic(0, "APPL: Can't handle %s event", 
	    	  wap_event_name(ind->type));
	    break;
	}
    }
}


static int convert_content(struct content *content) 
{
    Octstr *new_body;
    int failed = 0;
    int i;
    
    for (i = 0; i < NUM_CONVERTERS; i++) {
	if (octstr_str_compare(content->type, converters[i].type) == 0) {
	    new_body = converters[i].convert(content);
	    if (new_body != NULL) {
		octstr_destroy(content->body);
		content->body = new_body;
		octstr_destroy(content->type);
		content->type = octstr_create(
		converters[i].result_type);
		return 1;
	    }
	    failed = 1;
	}
    }
    
    if (failed)
	return -1;
    return 0;
}


/* Add a header identifying our gateway version */
static void add_kannel_version(List *headers) 
{
    http_header_add(headers, "X-WAP-Gateway", GW_NAME "/" VERSION);
}


/* Add Accept-Charset: headers for stuff the WML compiler can
 * convert to UTF-8. */
/* XXX This is not really correct, since we will not be able
 * to handle those charsets for all content types, just WML. */
static void add_charset_headers(List *headers) 
{
    long i, len;
    
    gw_assert(charsets != NULL);
    len = list_len(charsets);
    for (i = 0; i < len; i++) {
	unsigned char *charset = octstr_get_cstr(list_get(charsets, i));
	if (!http_charset_accepted(headers, charset))
	    http_header_add(headers, "Accept-Charset", charset);
    }
}


/* Add Accept: headers for stuff we can convert for the phone */
static void add_accept_headers(List *headers) 
{
    int i;
    
    for (i = 0; i < NUM_CONVERTERS; i++) {
	if (http_type_accepted(headers, converters[i].result_type)
	    && !http_type_accepted(headers, converters[i].type)) {
	    http_header_add(headers, "Accept", converters[i].type);
	}
    }
}


static void add_network_info(List *headers, WAPAddrTuple *addr_tuple) 
{
    if (octstr_len(addr_tuple->remote->address) > 0) {
	http_header_add(headers, "X_Network_Info", 
			octstr_get_cstr(addr_tuple->remote->address));
    }
}


static void add_session_id(List *headers, long session_id) 
{
    if (session_id != -1) {
	char buf[40];
	sprintf(buf, "%ld", session_id);
	http_header_add(headers, "X-WAP-Session-ID", buf);
    }
}


static void add_client_sdu_size(List *headers, long sdu_size) 
{
    if (sdu_size > 0) {
	Octstr *buf;
	
	buf = octstr_format("%ld", sdu_size);
	http_header_add(headers, "X-WAP-Client-SDU-Size", 
	    	    	octstr_get_cstr(buf));
	octstr_destroy(buf);
    }
}


static void add_via(List *headers) 
{
    Octstr *os;
    
    os = octstr_format("WAP/1.1 %S (" GW_NAME "/%s)", get_official_name(),
		       VERSION);
    http_header_add(headers, "Via", octstr_get_cstr(os));
    octstr_destroy(os);
}


/*
 * Add an X-WAP.TOD header to the response headers.  It is defined in
 * the "WAP Caching Model" specification.
 * We generate it in textual form and let WSP header packing convert it
 * to binary form.
 */
static void add_x_wap_tod(List *headers) 
{
    Octstr *gateway_time;
    
    gateway_time = date_format_http(time(NULL));
    if (gateway_time == NULL) {
	warning(0, "Could not add X-WAP.TOD response header.");
	return;
    }
    
    http_header_add(headers, "X-WAP.TOD", octstr_get_cstr(gateway_time));
		    octstr_destroy(gateway_time);
}


static void add_referer_url(List *headers, Octstr *url) 
{
    if (octstr_len(url) > 0) {
	   http_header_add(headers, "Referer", octstr_get_cstr(url));
    }
}


static void set_referer_url(Octstr *url, WSPMachine *sm)
{
	gw_assert(url != NULL);
	gw_assert(sm != NULL);

    sm->referer_url = octstr_duplicate(url);
}


static Octstr *get_referer_url(const WSPMachine *sm)
{
    return sm ? sm->referer_url : NULL;
}


/*
 * Return the reply from an HTTP request to the phone via a WSP session.
 */
static void return_session_reply(long server_transaction_id, long status,
    	    	    	    	 List *headers, Octstr *body, 
				 long session_id)
{
    WAPEvent *e;
    
    e = wap_event_create(S_MethodResult_Req);
    e->u.S_MethodResult_Req.server_transaction_id = server_transaction_id;
    e->u.S_MethodResult_Req.status = status;
    e->u.S_MethodResult_Req.response_headers = headers;
    e->u.S_MethodResult_Req.response_body = body;
    e->u.S_MethodResult_Req.session_id = session_id;
    wsp_session_dispatch_event(e);
}


/*
 * Return the reply from an HTTP request to the phone via connectionless
 * WSP.
 */
static void return_unit_reply(WAPAddrTuple *tuple, long transaction_id,
    	    	    	      long status, List *headers, Octstr *body)
{
    WAPEvent *e;

    e = wap_event_create(S_Unit_MethodResult_Req);
    e->u.S_Unit_MethodResult_Req.addr_tuple = 
    	wap_addr_tuple_duplicate(tuple);
    e->u.S_Unit_MethodResult_Req.transaction_id = transaction_id;
    e->u.S_Unit_MethodResult_Req.status = status;
    e->u.S_Unit_MethodResult_Req.response_headers = headers;
    e->u.S_Unit_MethodResult_Req.response_body = body;
    wsp_unit_dispatch_event(e);
}


/*
 * Return an HTTP reply back to the phone.
 */
static void return_reply(int status, Octstr *content_body, List *headers,
    	    	    	 long sdu_size, WAPEvent *orig_event,
                         long session_id, Octstr *url, int x_wap_tod,
                         List *request_headers)
{
    struct content content;
    int converted;
    WSPMachine *sm;

    content.url = url;
    content.body = content_body;

    if (status < 0) {
        error(0, "WSP: http lookup failed, oops.");
        content.charset = octstr_create("");
        /* smart WSP error messaging?! */
        if (wsp_smart_errors) {
            Octstr *referer_url;
            status = HTTP_OK;
            content.type = octstr_create("text/vnd.wap.wml");
            /*
             * check if a referer for this URL exists and 
             * get back to the previous page in this case
             */
            if ((referer_url = get_referer_url(find_session_machine_by_id(session_id)))) {
                content.body = error_requesting_back(url, referer_url);
                debug("wap.wsp",0,"WSP: returning smart error WML deck for referer URL");
            } 
            /*
             * if there is no referer to retun to, check if we have a
             * device-home defined and return to that, otherwise simply
             * drop an error wml deck.
             */
            else if (device_home != NULL) {
                content.body = error_requesting_back(url, device_home);
                debug("wap.wsp",0,"WSP: returning smart error WML deck for device-home URL");
            } else {
                content.body = error_requesting(url);
                debug("wap.wsp",0,"WSP: returning smart error WML deck");
            }

            /* 
             * if we did not connect at all there is no content in 
             * the headers list, so create for the upcoming transformation
             */
            if (headers == NULL)
                headers = http_create_empty_headers();

            converted = convert_content(&content);
            if (converted == 1)
                http_header_mark_transformation(headers, content.body, content.type);

        } else {
            status = HTTP_BAD_GATEWAY;
            content.type = octstr_create("text/plain");
            content.charset = octstr_create("");
            content.body = octstr_create("");
        }

    } else {

        http_header_get_content_type(headers, &content.type, &content.charset);
        alog("<%s> (%s, charset='%s') %d", 
             octstr_get_cstr(url), octstr_get_cstr(content.type),
             octstr_get_cstr(content.charset), status);

#ifdef ENABLE_COOKIES
        if (session_id != -1)
            if (get_cookies(headers, find_session_machine_by_id(session_id)) == -1)
                error(0, "WSP: Failed to extract cookies");
#endif

        converted = convert_content(&content);
        if (converted < 0) {
            warning(0, "WSP: All converters for `%s' failed.",
                    octstr_get_cstr(content.type));
            /* Don't change status; just send the client what we did get. */
        }
        if (converted == 1) {
            http_header_mark_transformation(headers, content.body, content.type);

            /* 
             * set referer URL to WSPMachine, but only if this was a converted
             * content-type, like .wml
             */
            if (session_id != -1) {
                debug("wap.wsp.http",0,"WSP: Setting Referer URL to <%s>", 
                      octstr_get_cstr(url));
                if ((sm = find_session_machine_by_id(session_id)) != NULL) {
                    set_referer_url(url, sm);
                } else {
                    error(0,"WSP: Failed to find session machine for ID %ld",
                          session_id);
                }
            }
        }
    }

    if (headers == NULL)
        headers = http_create_empty_headers();
    http_remove_hop_headers(headers);
    http_header_remove_all(headers, "X-WAP.TOD");
    if (x_wap_tod)
        add_x_wap_tod(headers);

    if (content.body == NULL)
        content.body = octstr_create("");

    /*
     * Deal with otherwise wap-aware servers that return text/html error
     * messages if they report an error.
     * (Normally we leave the content type alone even if the client doesn't
     * claim to accept it, because the server might know better than the
     * gateway.)
     */
    if (http_status_class(status) != HTTP_STATUS_SUCCESSFUL &&
        !http_type_accepted(request_headers, octstr_get_cstr(content.type))) {
        warning(0, "WSP: Content type <%s> not supported by client,"
                   " deleting body.", octstr_get_cstr(content.type));
        octstr_destroy(content.body);
        content.body = octstr_create("");
        octstr_destroy(content.type);
        content.type = octstr_create("text/plain");
        http_header_mark_transformation(headers, content.body, content.type);
    }

    /*
     * If the response is too large to be sent to the client,
     * suppress it and inform the client.
     */
    if (octstr_len(content.body) > sdu_size && sdu_size > 0) {
        /*
         * Only change the status if it indicated success.
         * If it indicated an error, then that information is
         * more useful to the client than our "Bad Gateway" would be.
         * The too-large body is probably an error page in html.
         */
        if (http_status_class(status) == HTTP_STATUS_SUCCESSFUL)
            status = HTTP_BAD_GATEWAY;
        warning(0, "WSP: Entity at %s too large (size %ld B, limit %lu B)",
                octstr_get_cstr(url), octstr_len(content.body), sdu_size);
        octstr_destroy(content.body);
        content.body = octstr_create("");
        http_header_mark_transformation(headers, content.body, content.type);
    }

    if (orig_event->type == S_MethodInvoke_Ind) {
        return_session_reply(orig_event->u.S_MethodInvoke_Ind.server_transaction_id,
                             status, headers, content.body, session_id);
    } else {
        return_unit_reply(orig_event->u.S_Unit_MethodInvoke_Ind.addr_tuple,
                          orig_event->u.S_Unit_MethodInvoke_Ind.transaction_id,
                          status, headers, content.body);
    }

    octstr_destroy(content.type); /* body was re-used above */
    octstr_destroy(content.charset);
    octstr_destroy(url);          /* same as content.url */

    counter_decrease(fetches);
}


/*
 * This thread receives replies from HTTP layer and sends them back to
 * the phone.
 */
static void return_replies_thread(void *arg)
{
    Octstr *body;
    struct request_data *p;
    int status;
    Octstr *final_url;
    List *headers;

    while (run_status == running) {

        p = http_receive_result(caller, &status, &final_url, &headers, &body);
        if (p == NULL)
            break;

        return_reply(status, body, headers, p->client_SDU_size,
                     p->event, p->session_id, p->url, p->x_wap_tod,
                     p->request_headers);
        wap_event_destroy(p->event);
        http_destroy_headers(p->request_headers);
        gw_free(p);
        octstr_destroy(final_url);
    }
}


/*
 * This WML deck is returned when the user asks for the URL "kannel:alive".
 */
#define HEALTH_DECK \
    "<?xml version=\"1.0\"?>" \
    "<!DOCTYPE wml PUBLIC \"-//WAPFORUM//DTD 1.1//EN\" " \
    "\"http://www.wapforum.org/DTD/wml_1.1.xml\">" \
    "<wml><card id=\"health\"><p>Ok</p></card></wml>"

static void start_fetch(WAPEvent *event) 
{
    int ret;
    long client_SDU_size; /* 0 means no limit */
    Octstr *url;
    Octstr *referer_url;
    List *session_headers;
    List *request_headers;
    List *actual_headers;
    List *resp_headers;
    WAPAddrTuple *addr_tuple;
    long session_id;
    Octstr *content_body;
    Octstr *method;	    /* type of request, normally a get or a post */
    Octstr *request_body;
    int x_wap_tod;          /* X-WAP.TOD header was present in request */
    Octstr *magic_url;
    struct request_data *p;
    
    counter_increase(fetches);
    
    if (event->type == S_MethodInvoke_Ind) {
        struct S_MethodInvoke_Ind *p;
    
        p = &event->u.S_MethodInvoke_Ind;
        session_headers = p->session_headers;
        request_headers = p->request_headers;
        url = octstr_duplicate(p->request_uri);
        addr_tuple = p->addr_tuple;
        session_id = p->session_id;
        client_SDU_size = p->client_SDU_size;
        request_body = octstr_duplicate(p->request_body);
        method = p->method;
    } else {
        struct S_Unit_MethodInvoke_Ind *p;
	
        p = &event->u.S_Unit_MethodInvoke_Ind;
        session_headers = NULL;
        request_headers = p->request_headers;
        url = octstr_duplicate(p->request_uri);
        addr_tuple = p->addr_tuple;
        session_id = -1;
        client_SDU_size = 0; /* No limit */
        request_body = octstr_duplicate(p->request_body);
        method = p->method;
    }
    
    wsp_http_map_url(&url);
    
    actual_headers = list_create();
    
    if (session_headers != NULL)
        http_header_combine(actual_headers, session_headers);
    if (request_headers != NULL)
        http_header_combine(actual_headers, request_headers);
    
    http_remove_hop_headers(actual_headers);
    x_wap_tod = http_header_remove_all(actual_headers, "X-WAP.TOD");
    add_accept_headers(actual_headers);
    add_charset_headers(actual_headers);
    add_network_info(actual_headers, addr_tuple);
    add_client_sdu_size(actual_headers, client_SDU_size);
    add_via(actual_headers);
    
#ifdef ENABLE_COOKIES
    if ((session_id != -1) && 
        (set_cookies(actual_headers, find_session_machine_by_id(session_id)) == -1)) 
        error(0, "WSP: Failed to add cookies");
#endif

    /* set referer URL to HTTP header from WSPMachine */
    if (session_id != -1) {
        if ((referer_url = get_referer_url(find_session_machine_by_id(session_id))) != NULL) {
            add_referer_url(actual_headers, referer_url);
        }
    }
    
    add_kannel_version(actual_headers);
    add_session_id(actual_headers, session_id);
    
    http_header_pack(actual_headers);
    
    magic_url = octstr_imm("kannel:alive");
    if (octstr_str_compare(method, "GET")  == 0 &&
        octstr_compare(url, magic_url) == 0) {
        ret = HTTP_OK;
        resp_headers = list_create();
        http_header_add(resp_headers, "Content-Type", "text/vnd.wap.wml");
        content_body = octstr_create(HEALTH_DECK);
        octstr_destroy(request_body);
        return_reply(ret, content_body, resp_headers, client_SDU_size,
                     event, session_id, url, x_wap_tod, actual_headers);
        wap_event_destroy(event);
        http_destroy_headers(actual_headers);
    } else if (octstr_str_compare(method, "GET") == 0 ||
               octstr_str_compare(method, "POST") == 0 ||
               octstr_str_compare(method, "HEAD") == 0) {
        if (request_body != NULL && (octstr_str_compare(method, "GET") == 0 ||
                                     octstr_str_compare(method, "HEAD") == 0)) {
            octstr_destroy(request_body);
            request_body = NULL;
        }

        p = gw_malloc(sizeof(*p));
        p->client_SDU_size = client_SDU_size;
        p->event = event;
        p->session_id = session_id;
        p->url = url;
        p->x_wap_tod = x_wap_tod;
        p->request_headers = actual_headers;
        http_start_request(caller, http_name2method(method), url, actual_headers, 
                           request_body, 0, p, NULL);
        octstr_destroy(request_body);
    } else {
        error(0, "WSP: Method %s not supported.", octstr_get_cstr(method));
        content_body = octstr_create("");
        resp_headers = http_create_empty_headers();
        ret = HTTP_NOT_IMPLEMENTED;
        octstr_destroy(request_body);
        return_reply(ret, content_body, resp_headers, client_SDU_size,
                     event, session_id, url, x_wap_tod, actual_headers);
        wap_event_destroy(event);
        http_destroy_headers(actual_headers);
    }
}



/* Shut up WMLScript compiler status/trace messages. */
static void dev_null(const char *data, size_t len, void *context) 
{
    /* nothing */
}


static Octstr *convert_wml_to_wmlc(struct content *content) 
{
    Octstr *wmlc;
    int ret;
    
    ret = wml_compile(content->body, content->charset, &wmlc);
    if (ret == 0)
	return wmlc;
    warning(0, "WSP: WML compilation failed.");
    return NULL;
}


static Octstr *convert_wmlscript_to_wmlscriptc(struct content *content) 
{
    WsCompilerParams params;
    WsCompilerPtr compiler;
    WsResult result;
    unsigned char *result_data;
    size_t result_size;
    Octstr *wmlscriptc;
    
    memset(&params, 0, sizeof(params));
    params.use_latin1_strings = 0;
    params.print_symbolic_assembler = 0;
    params.print_assembler = 0;
    params.meta_name_cb = NULL;
    params.meta_name_cb_context = NULL;
    params.meta_http_equiv_cb = NULL;
    params.meta_http_equiv_cb_context = NULL;
    params.stdout_cb = dev_null;
    params.stderr_cb = dev_null;
    
    compiler = ws_create(&params);
    if (compiler == NULL) {
	panic(0, "WSP: could not create WMLScript compiler");
	exit(1);
    }
    
    result = ws_compile_data(compiler, 
			     octstr_get_cstr(content->url),
			     octstr_get_cstr(content->body),
			     octstr_len(content->body),
			     &result_data,
			     &result_size);
    if (result != WS_OK) {
	warning(0, "WSP: WMLScript compilation failed: %s",
		ws_result_to_string(result));
	wmlscriptc = NULL;
    } else {
	wmlscriptc = octstr_create_from_data(result_data, result_size);
    }
    
    return wmlscriptc;
}


/* The interface for capability negotiation is a bit different from
 * the negotiation at WSP level, to make it easier to program.
 * The application layer gets a list of requested capabilities,
 * basically a straight decoding of the WSP level capabilities.
 * It replies with a list of all capabilities it wants to set or
 * refuse.  (Refuse by setting cap->data to NULL).  Any capabilities
 * it leaves out are considered "unknown; don't care".  The WSP layer
 * will either process those itself, or refuse them.
 *
 * At the WSP level, not sending a reply to a capability means accepting
 * what the client proposed.  If the application layer wants this to 
 * happen, it should set cap->data to NULL and cap->accept to 1.
 * (The WSP layer does not try to guess what kind of reply would be 
 * identical to what the client proposed, because the format of the
 * reply is often different from the format of the request, and this
 * is likely to be true for unknown capabilities too.)
 */
static List *negotiate_capabilities(List *req_caps) 
{
    /* Currently we don't know or care about any capabilities,
     * though it is likely that "Extended Methods" will be
     * the first. */
    return list_create();
}


/***********************************************************************
 * The following code implements the map-url mechanism
 */

struct wsp_http_map {
	struct wsp_http_map *next;
	unsigned flags;
#define WSP_HTTP_MAP_INPREFIX	0x0001	/* prefix-match incoming string */
#define WSP_HTTP_MAP_OUTPREFIX	0x0002	/* prefix-replace outgoing string */
#define WSP_HTTP_MAP_INOUTPREFIX 0x0003	/* combine the two for masking */
	char *in;
	int in_len;
	char *out;
	int out_len;
};

static struct wsp_http_map *wsp_http_map = 0;
static struct wsp_http_map *wsp_http_map_last = 0;

/*
 * Add mapping for src URL to dst URL.
 */
static void wsp_http_map_url_do_config(char *src, char *dst)
{
    struct wsp_http_map *new_map;
    int in_len = src ? strlen(src) : 0;
    int out_len = dst ? strlen(dst) : 0;
    
    if (!in_len) {
	warning(0, "wsp_http_map_url_do_config: empty incoming string");
	return;
    }
    gw_assert(in_len > 0);
    
    new_map = gw_malloc(sizeof(*new_map));
    new_map->next = NULL;
    new_map->flags = 0;
    
    /* incoming string
     * later, the incoming URL will be prefix-compared to new_map->in,
     * using exactly new_map->in_len characters for comparison.
     */
    new_map->in = gw_strdup(src);
    if (src[in_len-1] == '*') {
	new_map->flags |= WSP_HTTP_MAP_INPREFIX;
	in_len--;		/* do not include `*' in comparison */
    } else {
	in_len++;		/* include \0 in comparisons */
    }
    new_map->in_len = in_len;
    
    /* replacement string
     * later, when an incoming URL matches, it will be replaced
     * or modified according to this string. If the replacement
     * string ends with an asterisk, and the match string indicates
     * a prefix match (also ends with an asterisk), the trailing
     * part of the matching URL will be appended to the replacement
     * string, i.e. we do a prefix replacement.
     */
    new_map->out = gw_strdup(dst);
    if (dst[out_len-1] == '*') {
	new_map->flags |= WSP_HTTP_MAP_OUTPREFIX;
	out_len--;			/* exclude `*' */
    }
    new_map->out_len = out_len;
    
    /* insert at tail of existing list */
    if (wsp_http_map == NULL) {
	wsp_http_map = wsp_http_map_last = new_map;
    } else {
	wsp_http_map_last->next = new_map;
	wsp_http_map_last = new_map;
    }
}

/* Called during configuration read, once for each "map-url" statement.
 * Interprets parameter value as a space-separated two-tuple of src and dst.
 */
void wsp_http_map_url_config(char *s)
{
    char *in, *out;
    
    s = gw_strdup(s);
    in = strtok(s, " \t");
    if (!in) 
    	return;
    out = strtok(NULL, " \t");
    if (!out) 
    	return;
    wsp_http_map_url_do_config(in, out);
    gw_free(s);
}

/* Called during configuration read, this adds a mapping for the source URL
 * "DEVICE:home", to the given destination. The mapping is configured
 * as an in/out prefix mapping.
 */
void wsp_http_map_url_config_device_home(char *to)
{
    int len;
    char *newto = 0;
    
    if (!to)
	return;
    len = strlen(to);
    if (to[len] != '*') {
	newto = gw_malloc(len+2);
	strcpy(newto, to);
	newto[len] = '*';
	newto[len+1] = '\0';
	to = newto;
    }
    wsp_http_map_url_do_config("DEVICE:home*", to);
    if (newto)
	gw_free(newto);
}

/* show mapping list at info level, after configuration is done. */
void wsp_http_map_url_config_info(void)
{
    struct wsp_http_map *run;
    
    for (run = wsp_http_map; run; run = run->next) {
	char *s1 = (run->flags & WSP_HTTP_MAP_INPREFIX)  ? "*" : "";
	char *s2 = (run->flags & WSP_HTTP_MAP_OUTPREFIX) ? "*" : "";
	info(0, "map-url %.*s%s %.*s%s",
	     run->in_len, run->in, s1,
	     run->out_len, run->out, s2);
    }
}

/* Search list of mappings for the given URL, returning the map structure. */
static struct wsp_http_map *wsp_http_map_find(char *s)
{
    struct wsp_http_map *run;
    
    for (run = wsp_http_map; run; run = run->next)
    if (0 == strncasecmp(s, run->in, run->in_len))
	break;
    if (run) {
	debug("wap.wsp.http", 0, "WSP: found mapping for url <%s>", s);
    }
    return run;
}

/* 
 * Maybe rewrite URL, if there is a mapping. This is where the runtime
 * lookup comes in (called from further down this file, wsp_http.c)
 */
static void wsp_http_map_url(Octstr **osp)
{
    struct wsp_http_map *map;
    Octstr *old = *osp;
    char *oldstr = octstr_get_cstr(old);
    
    map = wsp_http_map_find(oldstr);
    if (!map)
        return;

    *osp = octstr_create_from_data(map->out, map->out_len);

    /* 
     * If both prefix flags are set, append tail of incoming URL
     * to outgoing URL.
     */
    if (WSP_HTTP_MAP_INOUTPREFIX == (map->flags & WSP_HTTP_MAP_INOUTPREFIX))
        octstr_append_cstr(*osp, oldstr + map->in_len);
    debug("wap.wsp.http", 0, "WSP: url <%s> mapped to <%s>",
          oldstr, octstr_get_cstr(*osp));
    octstr_destroy(old);
}


void wsp_http_map_destroy(void) 
{
    struct wsp_http_map *p, *q;
    p = wsp_http_map;
    
    while (p != NULL) {
	q = p;
	if (q -> in) 
	    gw_free (q -> in);
	if (q -> out) 
	    gw_free (q -> out);
	p = q -> next;
	gw_free (q);
    }
}

/*
 * Ota submodule implements indications, responses and confirmations part of 
 * ota.
 */

/*
 * If Accept-Application is empty, add header indicating default application 
 * wml ua (see ota 6.4.1). Otherwise decode application id (see http://www.
 * wapforum.org/wina/push-app-id.htm). FIXME: capability negotiation (no-
 * thing means default, if so negotiated).
 * Function does not allocate memory neither for headers nor application_
 * headers.
 * Returns encoded application headers and input header list without them.
 */
static void check_application_headers(List **headers, 
                                      List **application_headers)
{
    List *inh;
    int i;
    Octstr *appid_name, *coded_octstr;
    char *appid_value, *coded_value;

    split_header_list(headers, &inh, "Accept-Application");
    
    if (*headers == NULL || list_len(inh) == 0) {
        http_header_add(*application_headers, "Accept-Application", "wml ua");
        debug("wap.appl.push", 0, "APPL: No push application, assuming wml"
              " ua");
        if (*headers != NULL)
            http_destroy_headers(inh);
        return;
    }

    i = 0;
    coded_value = NULL;
    appid_value = NULL;

    while (list_len(inh) > 0) {
        http_header_get(inh, i, &appid_name, &coded_octstr);

        /* Greatest value reserved by WINA is 0xFF00 0000*/
        coded_value = octstr_get_cstr(coded_octstr);
        if (coded_value != NULL)
	   appid_value = wsp_application_id_to_cstr((long) coded_value);

        if (appid_value != NULL && coded_value != NULL)
            http_header_add(*application_headers, "Accept-Application", 
                            appid_value);
        else {
	    error(0, "OTA: Unknown application is, skipping: ");
            octstr_dump(coded_octstr, 0);
        }

        i++;  
    }
   
    debug("wap.appl.push", 0, "application headers were");
    http_header_dump(*application_headers);

    http_destroy_headers(inh);
    octstr_destroy(appid_name);
    octstr_destroy(coded_octstr);
}

/*
 * Bearer-Indication field is defined in ota 6.4.1. 
 * Skip the header, if it is malformed or if there is more than one bearer 
 * indication.
 * Function does not allocate memory neither for headers nor bearer_headers.
 * Return encoded bearer indication header and input header list without it.
 */
static void decode_bearer_indication(List **headers, List **bearer_headers)
{
    List *inb;
    Octstr *name, *coded_octstr;
    char *value;
    unsigned char coded_value;

    if (*headers == NULL) {
        debug("wap.appl", 0, "APPL: no client headers, continuing");
        return;
    }

    split_header_list(headers, &inb, "Bearer-Indication");

    if (list_len(inb) == 0) {
        debug("wap.appl.push", 0, "APPL: No bearer indication headers,"
              " continuing");
        http_destroy_headers(inb);
        return;  
    }

    if (list_len(inb) > 1) {
        error(0, "APPL: To many bearer indication header(s), skipping"
              " them");
        http_destroy_headers(inb);
        return;
    }

    http_header_get(inb, 0, &name, &coded_octstr);
    http_destroy_headers(inb);

  /* Greatest assigned number for a bearer type is 0xff, see wdp, appendix C */
    coded_value = octstr_get_char(coded_octstr, 0);
    value = wsp_bearer_indication_to_cstr(coded_value);

    if (value != NULL && coded_value != 0) {
       http_header_add(*bearer_headers, "Bearer-Indication", value);
       debug("wap.appl.push", 0, "bearer indication header was");
       http_header_dump(*bearer_headers);
       return;
    } else {
       error(0, "APPL: Illegal bearer indication value, skipping");
       octstr_dump(coded_octstr, 0);
       http_destroy_headers(*bearer_headers);
       return;
    }
}


/*
 * Separate headers into two lists, one having all headers named "name" and
 * the other rest of them.
 */
static void split_header_list(List **headers, List **new_headers, char *name)
{
    if (*headers == NULL)
        return;

    *new_headers = http_header_find_all(*headers, name);
    http_header_remove_all(*headers, name);  
}

/*
 * Find headers Accept-Application and Bearer-Indication amongst push headers,
 * decode them and add them to their proper field. 
 */
static void indicate_push_connection(WAPEvent *e)
{
    WAPEvent *ppg_event;
    List *push_headers,
         *application_headers,
         *bearer_headers;

    push_headers = http_header_duplicate(e->u.S_Connect_Ind.client_headers);
    application_headers = http_create_empty_headers();
    bearer_headers = http_create_empty_headers();
    
    ppg_event = wap_event_create(Pom_Connect_Ind);
    ppg_event->u.Pom_Connect_Ind.addr_tuple = 
        wap_addr_tuple_duplicate(e->u.S_Connect_Ind.addr_tuple);
    ppg_event->u.Pom_Connect_Ind.requested_capabilities = 
        wsp_cap_duplicate_list(e->u.S_Connect_Ind.requested_capabilities);

    check_application_headers(&push_headers, &application_headers);
    ppg_event->u.Pom_Connect_Ind.accept_application = application_headers;

    decode_bearer_indication(&push_headers, &bearer_headers);

    if (list_len(bearer_headers) == 0) {
        http_destroy_headers(bearer_headers);
        ppg_event->u.Pom_Connect_Ind.bearer_indication = NULL;
    } else
        ppg_event->u.Pom_Connect_Ind.bearer_indication = bearer_headers;

    ppg_event->u.Pom_Connect_Ind.push_headers = push_headers;
    ppg_event->u.Pom_Connect_Ind.session_id = e->u.S_Connect_Ind.session_id;
    debug("wap.appl", 0, "APPL: making OTA connection indication to PPG");

    wap_push_ppg_dispatch_event(ppg_event);
}

static void indicate_push_disconnect(WAPEvent *e)
{
    WAPEvent *ppg_event;

    ppg_event = wap_event_create(Pom_Disconnect_Ind);
    ppg_event->u.Pom_Disconnect_Ind.reason_code = 
        e->u.S_Disconnect_Ind.reason_code;
    ppg_event->u.Pom_Disconnect_Ind.error_headers =
        octstr_duplicate(e->u.S_Disconnect_Ind.error_headers);
    ppg_event->u.Pom_Disconnect_Ind.error_body =
        octstr_duplicate(e->u.S_Disconnect_Ind.error_body);
    ppg_event->u.Pom_Disconnect_Ind.session_handle =
        e->u.S_Disconnect_Ind.session_handle;

    wap_push_ppg_dispatch_event(ppg_event);
}

/*
 * We do not implement acknowledgement headers
 */
static void confirm_push(WAPEvent *e)
{
    WAPEvent *ppg_event;

    ppg_event = wap_event_create(Po_ConfirmedPush_Cnf);
    ppg_event->u.Po_ConfirmedPush_Cnf.server_push_id = 
        e->u.S_ConfirmedPush_Cnf.server_push_id;
    ppg_event->u.Po_ConfirmedPush_Cnf.session_handle = 
         e->u.S_ConfirmedPush_Cnf.session_id;

    debug("wap.appl", 0, "OTA: confirming push for ppg");
    wap_push_ppg_dispatch_event(ppg_event);
}

static void indicate_push_abort(WAPEvent *e)
{
    WAPEvent *ppg_event;

    ppg_event = wap_event_create(Po_PushAbort_Ind);
    ppg_event->u.Po_PushAbort_Ind.push_id = e->u.S_PushAbort_Ind.push_id;
    ppg_event->u.Po_PushAbort_Ind.reason = e->u.S_PushAbort_Ind.reason;
    ppg_event->u.Po_PushAbort_Ind.session_handle = 
        e->u.S_PushAbort_Ind.session_id;

    debug("wap.push.ota", 0, "OTA: making push abort indication for ppg");
    wap_push_ppg_dispatch_event(ppg_event);
}

static void indicate_push_suspend(WAPEvent *e)
{
    WAPEvent *ppg_event;

    ppg_event = wap_event_create(Pom_Suspend_Ind);
    ppg_event->u.Pom_Suspend_Ind.reason = e->u.S_Suspend_Ind.reason;
    ppg_event->u.Pom_Suspend_Ind.session_id =  e->u.S_Suspend_Ind.session_id;

    wap_push_ppg_dispatch_event(ppg_event);
}

/*
 * Find Bearer-Indication amongst client headers, decode it and assign it to
 * a separate field in the event structure.
 */
static void indicate_push_resume(WAPEvent *e)
{
    WAPEvent *ppg_event;
    List *push_headers,
         *bearer_headers;

    push_headers = http_header_duplicate(e->u.S_Resume_Ind.client_headers);
    bearer_headers = http_create_empty_headers();
    
    ppg_event = wap_event_create(Pom_Resume_Ind);
    ppg_event->u.Pom_Resume_Ind.addr_tuple = wap_addr_tuple_duplicate(
        e->u.S_Resume_Ind.addr_tuple);
   
    decode_bearer_indication(&push_headers, &bearer_headers);

    if (list_len(bearer_headers) == 0) {
        http_destroy_headers(bearer_headers);
        ppg_event->u.Pom_Resume_Ind.bearer_indication = NULL;
    } else 
        ppg_event->u.Pom_Resume_Ind.bearer_indication = bearer_headers;

    ppg_event->u.Pom_Resume_Ind.client_headers = push_headers;
    ppg_event->u.Pom_Resume_Ind.session_id = e->u.S_Resume_Ind.session_id;

    wap_push_ppg_dispatch_event(ppg_event);
}

/*
 * Server headers are mentioned in table in ota 6.4.1, but none of the primit-
 * ives use them. They are optional in S_Connect_Res, so we do not use them.
 */
static void response_push_connection(WAPEvent *e)
{
    WAPEvent *wsp_event;

    gw_assert(e->type = Pom_Connect_Res);

    wsp_event = wap_event_create(S_Connect_Res);
    wsp_event->u.S_Connect_Res.session_id = e->u.Pom_Connect_Res.session_id;
    wsp_event->u.S_Connect_Res.negotiated_capabilities =
        wsp_cap_duplicate_list(e->u.Pom_Connect_Res.negotiated_capabilities);
    debug("wap.appl", 0, "APPL: making push connect response");

    wsp_session_dispatch_event(wsp_event);
}





