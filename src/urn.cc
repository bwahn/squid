
/*
 * DEBUG: section 51    URN Parsing
 * AUTHOR: Kostas Anagnostakis
 *
 * SQUID Internet Object Cache  http://squid.nlanr.net/Squid/
 * --------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from the
 *  Internet community.  Development is led by Duane Wessels of the
 *  National Laboratory for Applied Network Research and funded by
 *  the National Science Foundation.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *  
 */

#include "squid.h"

static STCB urnHandleReply;
static wordlist *urn_parsebuffer(const char *inbuf);
static const char *const crlf = "\r\n";

typedef struct {
	StoreEntry *entry;
	StoreEntry *urlres_e;
	request_t *request;
} UrnState;

wordlist *
urnFindMinRtt(wordlist * urls, method_t m, int *rtt_ret)
{
    int min_rtt = 0;
    request_t *r;
    int rtt;
    wordlist *w;
    wordlist *min_w = NULL;
    debug(50, 3) ("urnFindMinRtt\n");
    assert(urls != NULL);
    for (w = urls; w; w = w->next) {
	r = urlParse(m, w->key);
	if (r == NULL)
	    continue;
	debug(50, 3) ("Parsed %s\n", w->key);
	rtt = netdbHostRtt(r->host);
	if (rtt == 0) {
	    debug(50, 3) ("Pinging %s\n", r->host);
	    netdbPingSite(r->host);
	    put_free_request_t(r);
	    continue;
	}
	debug(50, 3) ("%s rtt=%d\n", r->host, rtt);
	if (rtt == 0)
		continue;
	if (rtt > min_rtt && min_rtt != 0)
		continue;
	min_rtt = rtt;
	min_w = w;
	put_free_request_t(r);
    }
    if (rtt_ret)
	*rtt_ret = min_rtt;
    debug(50, 1) ("urnFindMinRtt: Returning '%s' RTT %d\n",
	min_w ? min_w->key : "NONE",
	min_rtt);
    return min_w;
}

void
urnStart(request_t *r, StoreEntry *e)
{
    LOCAL_ARRAY(char, urlres, 4096);
    request_t *urlres_r = NULL;
    const cache_key *k;
    char *t;
    UrnState *urnState;
    StoreEntry *urlres_e;
    debug(50, 3) ("urnStart: '%s'\n", storeUrl(e));
    t = strchr(r->urlpath, ':');
    if (t == NULL) {
        ErrorState *err = errorCon(ERR_URN_RESOLVE, HTTP_NOT_FOUND);
	err->request = requestLink(r);
	err->url = xstrdup(storeUrl(e));
	errorAppendEntry(e, err);
	return;
    }
    *t = '\0';
    urnState = xcalloc(1, sizeof(UrnState));
    urnState->entry = e;
    urnState->request = requestLink(r);
    cbdataAdd(urnState);
    snprintf(urlres, 4096, "http://%s/uri-res/N2L?%s", r->urlpath, t+1);
    k = storeKeyPublic(urlres, METHOD_GET);
    urlres_r = urlParse(METHOD_GET, urlres);
    urlres_r->headers = xstrdup("Accept: */*\r\n\r\n");
    urlres_r->headers_sz = strlen(urlres_r->headers);
    if ((urlres_e = storeGet(k)) == NULL) {
	urlres_e = storeCreateEntry(urlres, urlres, 0, METHOD_GET);
	storeClientListAdd(urlres_e, urnState);
	protoDispatch(0, urlres_e, urlres_r);
    } else {
        storeClientListAdd(urlres_e, urnState);
    }
    urnState->urlres_e = urlres_e;
    storeLockObject(urlres_e);
    storeClientCopy(urlres_e,
	0,
	0,
	4096,
	get_free_4k_page(),
	urnHandleReply,
	urnState);
}

static void
urnHandleReply(void *data, char *buf, ssize_t size)
{
    LOCAL_ARRAY(char, line, 4096);
    UrnState *urnState = data;
    StoreEntry *e = urnState->entry;
    StoreEntry *urlres_e = urnState->urlres_e;
    char *s = NULL;
    char *hdr;
    wordlist *w;
    wordlist *urls;
    wordlist *min_w;
    int l;
    String *S;
    ErrorState *err;

    debug(50, 3) ("urnHandleReply: Called with size=%d.\n", size);
    if (urlres_e->store_status == STORE_ABORTED) {
	put_free_4k_page(buf);
	return;
    }
    if (size == 0) {
	put_free_4k_page(buf);
	return;
    } else if (size < 0) {
	put_free_4k_page(buf);
	return;
    }
    if (urlres_e->store_status == STORE_PENDING) {
	storeClientCopy(urlres_e,
	    size,
	    0,
	    SM_PAGE_SIZE,
	    buf,
	    urnHandleReply,
	    urnState);
	return;
    }
    /* we know its STORE_OK */
    s = mime_headers_end(buf);
    if (s == NULL) {
	debug(50, 1) ("urnHandleReply: didn't find end-of-headers for %s\n",
	    storeUrl(e));
	return;
    }
    assert(urlres_e->mem_obj->reply);
    httpParseReplyHeaders(buf, urlres_e->mem_obj->reply);
    debug(50, 3) ("mem->reply exists, code=%d.\n",
	urlres_e->mem_obj->reply->code);
    if (urlres_e->mem_obj->reply->code != HTTP_OK) {
	debug(50, 3) ("urnHandleReply: failed.\n");
	err = errorCon(ERR_URN_RESOLVE, HTTP_NOT_FOUND);
	err->request = requestLink(urnState->request);
	err->url = xstrdup(storeUrl(e));
	errorAppendEntry(e, err);
	return;
    }
    while (isspace(*s))
	s++;
    urls = urn_parsebuffer(s);
    if (urls == NULL) {	/* unkown URN error */
	debug(50, 3) ("urnTranslateDone: unknown URN %s\n", storeUrl(e));
	err = errorCon(ERR_URN_RESOLVE, HTTP_NOT_FOUND);
	err->request = requestLink(urnState->request);
	err->url = xstrdup(storeUrl(e));
	errorAppendEntry(e, err);
	return;
    }
    min_w = urnFindMinRtt(urls, urnState->request->method, NULL);
    storeBuffer(e);
    S = stringCreate(1024);
    l = snprintf(line, 4096,
	"<TITLE>Select URL for %s</TITLE>\n"
	"<H2>Select URL for %s</H2>\n"
	"<UL>\n", storeUrl(e), storeUrl(e));
    stringAppend(S, line, l);
    for (w = urls; w; w = w->next) {
	l = snprintf(line, 4096, "<LI><A HREF=\"%s\">%s</A>\n", w->key, w->key);
        stringAppend(S, line, l);
    }
    l = snprintf(line, 4096,
	"</UL>"
	"<HR>\n"
	"<ADDRESS>\n"
	"Generated by %s/%s@%s\n"
	"</ADDRESS>\n",
	appname, version_string, getMyHostname());
    stringAppend(S, line, l);
    hdr = httpReplyHeader(1.0,
	HTTP_MOVED_TEMPORARILY,
	"text/html",
	stringLength(S),
	0,
	squid_curtime);
    storeAppend(e, hdr, strlen(hdr));
    if (min_w) {
	l = snprintf(line, 4096, "Location: %s\r\n", min_w->key);
        storeAppend(e, line, l);
    }
    storeAppend(e, "\r\n", 2);
    storeAppend(e, S->buf, stringLength(S));
    storeComplete(e);
    put_free_4k_page(buf);
    stringFree(S);
    storeUnlockObject(urlres_e);
}

static wordlist *
urn_parsebuffer(const char *inbuf)
{
    char *buf = xstrdup(inbuf);
    char *token;
    wordlist *u;
    wordlist *head = NULL;
    wordlist **last = &head;
    debug(50, 3) ("urn_parsebuffer\n");
    for (token = strtok(buf, crlf); token; token = strtok(NULL, crlf)) {
	debug(50, 3) ("urn_parsebuffer: got '%s'\n", token);
	u = xmalloc(sizeof(wordlist));
	u->key = xstrdup(token);
	u->next = NULL;
	*last = u;
	last = &u->next;
    }
    return head;
}
