/*
 * sftp.c: SFTP generic client code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include "misc.h"
#include "int64.h"
#include "tree234.h"
#include "sftp.h"

struct sftp_packet {
    char *data;
    unsigned length, maxlen;
    unsigned savedpos;
    int type;
};

static const char *fxp_error_message;
static int fxp_errtype;

static void fxp_internal_error(sftp_handle* sftp, char *msg);

/* ----------------------------------------------------------------------
 * SFTP packet construction functions.
 */
static void sftp_pkt_ensure(sftp_handle* sftp, struct sftp_packet *pkt, int length)
{
    if ((int)pkt->maxlen < length) {
	pkt->maxlen = length + 256;
	pkt->data = sresize(pkt->data, pkt->maxlen, char);
    }
}
static void sftp_pkt_adddata(sftp_handle* sftp, struct sftp_packet *pkt, void *data, int len)
{
    pkt->length += len;
    sftp_pkt_ensure(sftp, pkt, pkt->length);
    memcpy(pkt->data + pkt->length - len, data, len);
}
static void sftp_pkt_addbyte(sftp_handle* sftp, struct sftp_packet *pkt, unsigned char byte)
{
    sftp_pkt_adddata(sftp, pkt, &byte, 1);
}
static struct sftp_packet *sftp_pkt_init(sftp_handle* sftp, int pkt_type)
{
    struct sftp_packet *pkt;
    pkt = snew(struct sftp_packet);
    pkt->data = NULL;
    pkt->savedpos = -1;
    pkt->length = 0;
    pkt->maxlen = 0;
    sftp_pkt_addbyte(sftp, pkt, (unsigned char) pkt_type);
    return pkt;
}
/*
static void sftp_pkt_addbool(sftp_handle* sftp, struct sftp_packet *pkt, unsigned char value)
{
    sftp_pkt_adddata(sftp, pkt, &value, 1);
}
*/
static void sftp_pkt_adduint32(sftp_handle* sftp, struct sftp_packet *pkt,
			       unsigned long value)
{
    unsigned char x[4];
    PUT_32BIT(x, value);
    sftp_pkt_adddata(sftp, pkt, x, 4);
}
static void sftp_pkt_adduint64(sftp_handle* sftp, struct sftp_packet *pkt, uint64 value)
{
    unsigned char x[8];
    PUT_32BIT(x, value.hi);
    PUT_32BIT(x + 4, value.lo);
    sftp_pkt_adddata(sftp, pkt, x, 8);
}
static void sftp_pkt_addstring_start(sftp_handle* sftp, struct sftp_packet *pkt)
{
    sftp_pkt_adduint32(sftp, pkt, 0);
    pkt->savedpos = pkt->length;
}
static void sftp_pkt_addstring_str(sftp_handle* sftp, struct sftp_packet *pkt, char *data)
{
    sftp_pkt_adddata(sftp, pkt, data, strlen(data));
    PUT_32BIT(pkt->data + pkt->savedpos - 4, pkt->length - pkt->savedpos);
}
static void sftp_pkt_addstring_data(sftp_handle* sftp, struct sftp_packet *pkt,
				    char *data, int len)
{
    sftp_pkt_adddata(sftp, pkt, data, len);
    PUT_32BIT(pkt->data + pkt->savedpos - 4, pkt->length - pkt->savedpos);
}
static void sftp_pkt_addstring(sftp_handle* sftp, struct sftp_packet *pkt, char *data)
{
    sftp_pkt_addstring_start(sftp, pkt);
    sftp_pkt_addstring_str(sftp, pkt, data);
}
static void sftp_pkt_addattrs(sftp_handle* sftp, struct sftp_packet *pkt, struct fxp_attrs attrs)
{
    sftp_pkt_adduint32(sftp, pkt, attrs.flags);
    if (attrs.flags & SSH_FILEXFER_ATTR_SIZE) {
	sftp_pkt_adduint32(sftp, pkt, attrs.size.hi);
	sftp_pkt_adduint32(sftp, pkt, attrs.size.lo);
    }
    if (attrs.flags & SSH_FILEXFER_ATTR_UIDGID) {
	sftp_pkt_adduint32(sftp, pkt, attrs.uid);
	sftp_pkt_adduint32(sftp, pkt, attrs.gid);
    }
    if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
	sftp_pkt_adduint32(sftp, pkt, attrs.permissions);
    }
    if (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) {
	sftp_pkt_adduint32(sftp, pkt, attrs.atime);
	sftp_pkt_adduint32(sftp, pkt, attrs.mtime);
    }
    if (attrs.flags & SSH_FILEXFER_ATTR_EXTENDED) {
	/*
	 * We currently don't support sending any extended
	 * attributes.
	 */
    }
}

/* ----------------------------------------------------------------------
 * SFTP packet decode functions.
 */

static int sftp_pkt_getbyte(sftp_handle* sftp, struct sftp_packet *pkt, unsigned char *ret)
{
    if (pkt->length - pkt->savedpos < 1)
	return 0;
    *ret = (unsigned char) pkt->data[pkt->savedpos];
    pkt->savedpos++;
    return 1;
}
static int sftp_pkt_getuint32(sftp_handle* sftp, struct sftp_packet *pkt, unsigned long *ret)
{
    if (pkt->length - pkt->savedpos < 4)
	return 0;
    *ret = GET_32BIT(pkt->data + pkt->savedpos);
    pkt->savedpos += 4;
    return 1;
}
static int sftp_pkt_getstring(struct sftp_packet *pkt,
			      char **p, int *length)
{
    *p = NULL;
    if (pkt->length - pkt->savedpos < 4)
	return 0;
    *length = GET_32BIT(pkt->data + pkt->savedpos);
    pkt->savedpos += 4;
    if ((int)(pkt->length - pkt->savedpos) < *length || *length < 0) {
	*length = 0;
	return 0;
    }
    *p = pkt->data + pkt->savedpos;
    pkt->savedpos += *length;
    return 1;
}
static int sftp_pkt_getattrs(sftp_handle* sftp, struct sftp_packet *pkt, struct fxp_attrs *ret)
{
    if (!sftp_pkt_getuint32(sftp, pkt, &ret->flags))
	return 0;
    if (ret->flags & SSH_FILEXFER_ATTR_SIZE) {
	unsigned long hi, lo;
	if (!sftp_pkt_getuint32(sftp, pkt, &hi) ||
	    !sftp_pkt_getuint32(sftp, pkt, &lo))
	    return 0;
	ret->size = uint64_make(hi, lo);
    }
    if (ret->flags & SSH_FILEXFER_ATTR_UIDGID) {
	if (!sftp_pkt_getuint32(sftp, pkt, &ret->uid) ||
	    !sftp_pkt_getuint32(sftp, pkt, &ret->gid))
	    return 0;
    }
    if (ret->flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
	if (!sftp_pkt_getuint32(sftp, pkt, &ret->permissions))
	    return 0;
    }
    if (ret->flags & SSH_FILEXFER_ATTR_ACMODTIME) {
	if (!sftp_pkt_getuint32(sftp, pkt, &ret->atime) ||
	    !sftp_pkt_getuint32(sftp, pkt, &ret->mtime))
	    return 0;
    }
    if (ret->flags & SSH_FILEXFER_ATTR_EXTENDED) {
	unsigned long count;
	if (!sftp_pkt_getuint32(sftp, pkt, &count))
	    return 0;
	while (count--) {
	    char *str;
	    int len;
	    /*
	     * We should try to analyse these, if we ever find one
	     * we recognise.
	     */
	    if (!sftp_pkt_getstring(pkt, &str, &len) ||
		!sftp_pkt_getstring(pkt, &str, &len))
		return 0;
	}
    }
    return 1;
}
static void sftp_pkt_free(sftp_handle* sftp, struct sftp_packet *pkt)
{
    if (pkt->data)
	sfree(pkt->data);
    sfree(pkt);
}

/* ----------------------------------------------------------------------
 * Send and receive packet functions.
 */
int sftp_send(sftp_handle* sftp, struct sftp_packet *pkt)
{
    int ret;
    char x[4];
    PUT_32BIT(x, pkt->length);
    ret = (sftp_senddata(sftp, x, 4) && sftp_senddata(sftp, pkt->data, pkt->length));
    sftp_pkt_free(sftp, pkt);
    return ret;
}
struct sftp_packet *sftp_recv(sftp_handle* sftp)
{
    struct sftp_packet *pkt;
    char x[4];
    unsigned char uc;

    if (!sftp_recvdata(sftp, x, 4))
	return NULL;

    pkt = snew(struct sftp_packet);
    pkt->savedpos = 0;
    pkt->length = pkt->maxlen = GET_32BIT(x);
    pkt->data = snewn(pkt->length, char);

    if (!sftp_recvdata(sftp, pkt->data, pkt->length)) {
	sftp_pkt_free(sftp, pkt);
	return NULL;
    }

    if (!sftp_pkt_getbyte(sftp, pkt, &uc)) {
	sftp_pkt_free(sftp, pkt);
	return NULL;
    } else {
	pkt->type = uc;
    }

    return pkt;
}

/* ----------------------------------------------------------------------
 * Request ID allocation and temporary dispatch routines.
 */

#define REQUEST_ID_OFFSET 256

struct sftp_request {
    unsigned id;
    int registered;
    void *userdata;
};

static int sftp_reqcmp(void *av, void *bv)
{
    struct sftp_request *a = (struct sftp_request *)av;
    struct sftp_request *b = (struct sftp_request *)bv;
    if (a->id < b->id)
	return -1;
    if (a->id > b->id)
	return +1;
    return 0;
}
static int sftp_reqfind(void *av, void *bv)
{
    unsigned *a = (unsigned *) av;
    struct sftp_request *b = (struct sftp_request *)bv;
    if (*a < b->id)
	return -1;
    if (*a > b->id)
	return +1;
    return 0;
}

static tree234 *sftp_requests;

static struct sftp_request *sftp_alloc_request(sftp_handle* sftp)
{
    unsigned low, high, mid;
    int tsize;
    struct sftp_request *r;

    if (sftp_requests == NULL)
	sftp_requests = newtree234(sftp_reqcmp);

    /*
     * First-fit allocation of request IDs: always pick the lowest
     * unused one. To do this, binary-search using the counted
     * B-tree to find the largest ID which is in a contiguous
     * sequence from the beginning. (Precisely everything in that
     * sequence must have ID equal to its tree index plus
     * REQUEST_ID_OFFSET.)
     */
    tsize = count234(sftp_requests);

    low = -1;
    high = tsize;
    while (high - low > 1) {
	mid = (high + low) / 2;
	r = index234(sftp_requests, mid);
	if (r->id == mid + REQUEST_ID_OFFSET)
	    low = mid;		       /* this one is fine */
	else
	    high = mid;		       /* this one is past it */
    }
    /*
     * Now low points to either -1, or the tree index of the
     * largest ID in the initial sequence.
     */
    {
	unsigned i = low + 1 + REQUEST_ID_OFFSET;
	assert(NULL == find234(sftp_requests, &i, sftp_reqfind));
    }

    /*
     * So the request ID we need to create is
     * low + 1 + REQUEST_ID_OFFSET.
     */
    r = snew(struct sftp_request);
    r->id = low + 1 + REQUEST_ID_OFFSET;
    r->registered = 0;
    r->userdata = NULL;
    add234(sftp_requests, r);
    return r;
}

void sftp_cleanup_request(sftp_handle* sftp)
{
    if (sftp_requests != NULL) {
	freetree234(sftp_requests);
	sftp_requests = NULL;
    }
}

void sftp_register(sftp_handle* sftp, struct sftp_request *req)
{
    req->registered = 1;
}

struct sftp_request *sftp_find_request(sftp_handle* sftp, struct sftp_packet *pktin)
{
    unsigned long id;
    struct sftp_request *req;

    if (!pktin) {
	fxp_internal_error(sftp, "did not receive a valid SFTP packet\n");
	return NULL;
    }

    if (!sftp_pkt_getuint32(sftp, pktin, &id)) {
	fxp_internal_error(sftp, "did not receive a valid SFTP packet\n");
	return NULL;
    }
    req = find234(sftp_requests, &id, sftp_reqfind);

    if (!req || !req->registered) {
	fxp_internal_error(sftp, "request ID mismatch\n");
        sftp_pkt_free(sftp, pktin);
	return NULL;
    }

    del234(sftp_requests, req);

    return req;
}

/* ----------------------------------------------------------------------
 * String handling routines.
 */

static char *mkstr(sftp_handle* sftp, char *s, int len)
{
    char *p = snewn(len + 1, char);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ----------------------------------------------------------------------
 * SFTP primitives.
 */

/*
 * Deal with (and free) an FXP_STATUS packet. Return 1 if
 * SSH_FX_OK, 0 if SSH_FX_EOF, and -1 for anything else (error).
 * Also place the status into fxp_errtype.
 */
static int fxp_got_status(sftp_handle* sftp, struct sftp_packet *pktin)
{
    static const char *const messages[] = {
	/* SSH_FX_OK. The only time we will display a _message_ for this
	 * is if we were expecting something other than FXP_STATUS on
	 * success, so this is actually an error message! */
	"unexpected OK response",
	"end of file",
	"no such file or directory",
	"permission denied",
	"failure",
	"bad message",
	"no connection",
	"connection lost",
	"operation unsupported",
    };

    if (pktin->type != SSH_FXP_STATUS) {
	fxp_error_message = "expected FXP_STATUS packet";
	fxp_errtype = -1;
    } else {
	unsigned long ul;
	if (!sftp_pkt_getuint32(sftp, pktin, &ul)) {
	    fxp_error_message = "malformed FXP_STATUS packet";
	    fxp_errtype = -1;
	} else {
	    fxp_errtype = ul;
	    if (fxp_errtype < 0 ||
		fxp_errtype >= sizeof(messages) / sizeof(*messages))
		fxp_error_message = "unknown error code";
	    else
		fxp_error_message = messages[fxp_errtype];
	}
    }

    if (fxp_errtype == SSH_FX_OK)
	return 1;
    else if (fxp_errtype == SSH_FX_EOF)
	return 0;
    else
	return -1;
}

static void fxp_internal_error(sftp_handle* sftp, char *msg)
{
    fxp_error_message = msg;
    fxp_errtype = -1;
}

const char *fxp_error(sftp_handle* sftp)
{
    return fxp_error_message;
}

int fxp_error_type(sftp_handle* sftp)
{
    return fxp_errtype;
}

/*
 * Perform exchange of init/version packets. Return 0 on failure.
 */
int fxp_init(sftp_handle* sftp)
{
    struct sftp_packet *pktout, *pktin;
    unsigned long remotever;

    pktout = sftp_pkt_init(sftp, SSH_FXP_INIT);
    sftp_pkt_adduint32(sftp, pktout, SFTP_PROTO_VERSION);
    sftp_send(sftp, pktout);

    pktin = sftp_recv(sftp);
    if (!pktin) {
	fxp_internal_error(sftp, "could not connect");
	return 0;
    }
    if (pktin->type != SSH_FXP_VERSION) {
	fxp_internal_error(sftp, "did not receive FXP_VERSION");
        sftp_pkt_free(sftp, pktin);
	return 0;
    }
    if (!sftp_pkt_getuint32(sftp, pktin, &remotever)) {
	fxp_internal_error(sftp, "malformed FXP_VERSION packet");
        sftp_pkt_free(sftp, pktin);
	return 0;
    }
    if (remotever > SFTP_PROTO_VERSION) {
	fxp_internal_error
	    (sftp, "remote protocol is more advanced than we support");
        sftp_pkt_free(sftp, pktin);
	return 0;
    }
    /*
     * In principle, this packet might also contain extension-
     * string pairs. We should work through them and look for any
     * we recognise. In practice we don't currently do so because
     * we know we don't recognise _any_.
     */
    sftp_pkt_free(sftp, pktin);

    return 1;
}

/*
 * Canonify a pathname.
 */
struct sftp_request *fxp_realpath_send(sftp_handle* sftp, char *path)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_REALPATH);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring_start(sftp, pktout);
    sftp_pkt_addstring_str(sftp, pktout, path);
    sftp_send(sftp, pktout);

    return req;
}

char *fxp_realpath_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    sfree(req);

    if (pktin->type == SSH_FXP_NAME) {
	unsigned long count;
	char *path;
	int len;

	if (!sftp_pkt_getuint32(sftp, pktin, &count) || count != 1) {
	    fxp_internal_error(sftp, "REALPATH did not return name count of 1\n");
            sftp_pkt_free(sftp, pktin);
	    return NULL;
	}
	if (!sftp_pkt_getstring(pktin, &path, &len)) {
	    fxp_internal_error(sftp, "REALPATH returned malformed FXP_NAME\n");
            sftp_pkt_free(sftp, pktin);
	    return NULL;
	}
	path = mkstr(sftp, path, len);
	sftp_pkt_free(sftp, pktin);
	return path;
    } else {
	fxp_got_status(sftp, pktin);
        sftp_pkt_free(sftp, pktin);
	return NULL;
    }
}

/*
 * Open a file.
 */
struct sftp_request *fxp_open_send(sftp_handle* sftp, char *path, int type)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_OPEN);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring(sftp, pktout, path);
    sftp_pkt_adduint32(sftp, pktout, type);
    sftp_pkt_adduint32(sftp, pktout, 0);     /* (FIXME) empty ATTRS structure */
    sftp_send(sftp, pktout);

    return req;
}

struct fxp_handle *fxp_open_recv(sftp_handle* sftp, struct sftp_packet *pktin,
				 struct sftp_request *req)
{
    sfree(req);

    if (pktin->type == SSH_FXP_HANDLE) {
	char *hstring;
	struct fxp_handle *handle;
	int len;

	if (!sftp_pkt_getstring(pktin, &hstring, &len)) {
	    fxp_internal_error(sftp, "OPEN returned malformed FXP_HANDLE\n");
            sftp_pkt_free(sftp, pktin);
	    return NULL;
	}
	handle = snew(struct fxp_handle);
	handle->hstring = mkstr(sftp, hstring, len);
	handle->hlen = len;
	sftp_pkt_free(sftp, pktin);
	return handle;
    } else {
	fxp_got_status(sftp, pktin);
        sftp_pkt_free(sftp, pktin);
	return NULL;
    }
}

/*
 * Open a directory.
 */
struct sftp_request *fxp_opendir_send(sftp_handle* sftp, char *path)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_OPENDIR);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring(sftp, pktout, path);
    sftp_send(sftp, pktout);

    return req;
}

struct fxp_handle *fxp_opendir_recv(sftp_handle* sftp, struct sftp_packet *pktin,
				    struct sftp_request *req)
{
    sfree(req);
    if (pktin->type == SSH_FXP_HANDLE) {
	char *hstring;
	struct fxp_handle *handle;
	int len;

	if (!sftp_pkt_getstring(pktin, &hstring, &len)) {
	    fxp_internal_error(sftp, "OPENDIR returned malformed FXP_HANDLE\n");
            sftp_pkt_free(sftp, pktin);
	    return NULL;
	}
	handle = snew(struct fxp_handle);
	handle->hstring = mkstr(sftp, hstring, len);
	handle->hlen = len;
	sftp_pkt_free(sftp, pktin);
	return handle;
    } else {
	fxp_got_status(sftp, pktin);
        sftp_pkt_free(sftp, pktin);
	return NULL;
    }
}

/*
 * Close a file/dir.
 */
struct sftp_request *fxp_close_send(sftp_handle* sftp, struct fxp_handle *handle)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_CLOSE);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring_start(sftp, pktout);
    sftp_pkt_addstring_data(sftp, pktout, handle->hstring, handle->hlen);
    sftp_send(sftp, pktout);

    sfree(handle->hstring);
    sfree(handle);

    return req;
}

void fxp_close_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    sfree(req);
    fxp_got_status(sftp, pktin);
    sftp_pkt_free(sftp, pktin);
}

struct sftp_request *fxp_mkdir_send(sftp_handle* sftp, char *path)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_MKDIR);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring(sftp, pktout, path);
    sftp_pkt_adduint32(sftp, pktout, 0);     /* (FIXME) empty ATTRS structure */
    sftp_send(sftp, pktout);

    return req;
}

int fxp_mkdir_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(sftp, pktin);
    sftp_pkt_free(sftp, pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

struct sftp_request *fxp_rmdir_send(sftp_handle* sftp, char *path)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_RMDIR);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring(sftp, pktout, path);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_rmdir_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(sftp, pktin);
    sftp_pkt_free(sftp, pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

struct sftp_request *fxp_remove_send(sftp_handle* sftp, char *fname)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_REMOVE);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring(sftp, pktout, fname);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_remove_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(sftp, pktin);
    sftp_pkt_free(sftp, pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

struct sftp_request *fxp_rename_send(sftp_handle* sftp, char *srcfname, char *dstfname)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_RENAME);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring(sftp, pktout, srcfname);
    sftp_pkt_addstring(sftp, pktout, dstfname);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_rename_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(sftp, pktin);
    sftp_pkt_free(sftp, pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

/*
 * Retrieve the attributes of a file. We have fxp_stat which works
 * on filenames, and fxp_fstat which works on open file handles.
 */
struct sftp_request *fxp_stat_send(sftp_handle* sftp, char *fname)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_STAT);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring(sftp, pktout, fname);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_stat_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req,
		  struct fxp_attrs *attrs)
{
    sfree(req);
    if (pktin->type == SSH_FXP_ATTRS) {
	if (!sftp_pkt_getattrs(sftp, pktin, attrs)) {
	    fxp_internal_error(sftp, "malformed SSH_FXP_ATTRS packet");
	    sftp_pkt_free(sftp, pktin);
	    return 0;
	}
	sftp_pkt_free(sftp, pktin);
	return 1;
    } else {
	fxp_got_status(sftp, pktin);
        sftp_pkt_free(sftp, pktin);
	return 0;
    }
}

struct sftp_request *fxp_fstat_send(sftp_handle* sftp, struct fxp_handle *handle)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_FSTAT);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring_start(sftp, pktout);
    sftp_pkt_addstring_data(sftp, pktout, handle->hstring, handle->hlen);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_fstat_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req,
		   struct fxp_attrs *attrs)
{
    sfree(req);
    if (pktin->type == SSH_FXP_ATTRS) {
	if (!sftp_pkt_getattrs(sftp, pktin, attrs)) {
	    fxp_internal_error(sftp, "malformed SSH_FXP_ATTRS packet");
	    sftp_pkt_free(sftp, pktin);
	    return 0;
	}
	sftp_pkt_free(sftp, pktin);
	return 1;
    } else {
	fxp_got_status(sftp, pktin);
        sftp_pkt_free(sftp, pktin);
	return 0;
    }
}

/*
 * Set the attributes of a file.
 */
struct sftp_request *fxp_setstat_send(sftp_handle* sftp, char *fname, struct fxp_attrs attrs)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_SETSTAT);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring(sftp, pktout, fname);
    sftp_pkt_addattrs(sftp, pktout, attrs);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_setstat_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(sftp, pktin);
    sftp_pkt_free(sftp, pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

struct sftp_request *fxp_fsetstat_send(sftp_handle* sftp, struct fxp_handle *handle,
				       struct fxp_attrs attrs)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_FSETSTAT);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring_start(sftp, pktout);
    sftp_pkt_addstring_data(sftp, pktout, handle->hstring, handle->hlen);
    sftp_pkt_addattrs(sftp, pktout, attrs);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_fsetstat_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(sftp, pktin);
    sftp_pkt_free(sftp, pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

/*
 * Read from a file. Returns the number of bytes read, or -1 on an
 * error, or possibly 0 if EOF. (I'm not entirely sure whether it
 * will return 0 on EOF, or return -1 and store SSH_FX_EOF in the
 * error indicator. It might even depend on the SFTP server.)
 */
struct sftp_request *fxp_read_send(sftp_handle* sftp, struct fxp_handle *handle,
				   uint64 offset, int len)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_READ);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring_start(sftp, pktout);
    sftp_pkt_addstring_data(sftp, pktout, handle->hstring, handle->hlen);
    sftp_pkt_adduint64(sftp, pktout, offset);
    sftp_pkt_adduint32(sftp, pktout, len);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_read_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req,
		  char *buffer, int len)
{
    sfree(req);
    if (pktin->type == SSH_FXP_DATA) {
	char *str;
	int rlen;

	if (!sftp_pkt_getstring(pktin, &str, &rlen)) {
	    fxp_internal_error(sftp, "READ returned malformed SSH_FXP_DATA packet");
            sftp_pkt_free(sftp, pktin);
	    return -1;
	}

	if (rlen > len || rlen < 0) {
	    fxp_internal_error(sftp, "READ returned more bytes than requested");
            sftp_pkt_free(sftp, pktin);
	    return -1;
	}

	memcpy(buffer, str, rlen);
        sftp_pkt_free(sftp, pktin);
	return rlen;
    } else {
	fxp_got_status(sftp, pktin);
        sftp_pkt_free(sftp, pktin);
	return -1;
    }
}

/*
 * Read from a directory.
 */
struct sftp_request *fxp_readdir_send(sftp_handle* sftp, struct fxp_handle *handle)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_READDIR);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring_start(sftp, pktout);
    sftp_pkt_addstring_data(sftp, pktout, handle->hstring, handle->hlen);
    sftp_send(sftp, pktout);

    return req;
}

struct fxp_names *fxp_readdir_recv(sftp_handle* sftp, struct sftp_packet *pktin,
				   struct sftp_request *req)
{
    sfree(req);
    if (pktin->type == SSH_FXP_NAME) {
	struct fxp_names *ret;
	unsigned long i;

	/*
	 * Sanity-check the number of names. Minimum is obviously
	 * zero. Maximum is the remaining space in the packet
	 * divided by the very minimum length of a name, which is
	 * 12 bytes (4 for an empty filename, 4 for an empty
	 * longname, 4 for a set of attribute flags indicating that
	 * no other attributes are supplied).
	 */
	if (!sftp_pkt_getuint32(sftp, pktin, &i) ||
	    i > (pktin->length-pktin->savedpos)/12) {
	    fxp_internal_error(sftp, "malformed FXP_NAME packet");
	    sftp_pkt_free(sftp, pktin);
	    return NULL;
	}

	/*
	 * Ensure the implicit multiplication in the snewn() call
	 * doesn't suffer integer overflow and cause us to malloc
	 * too little space.
	 */
	if (i > INT_MAX / sizeof(struct fxp_name)) {
	    fxp_internal_error(sftp, "unreasonably large FXP_NAME packet");
	    sftp_pkt_free(sftp, pktin);
	    return NULL;
	}

	ret = snew(struct fxp_names);
	ret->nnames = i;
	ret->names = snewn(ret->nnames, struct fxp_name);
	for (i = 0; i < (unsigned long)ret->nnames; i++) {
	    char *str1, *str2;
	    int len1, len2;
	    if (!sftp_pkt_getstring(pktin, &str1, &len1) ||
		!sftp_pkt_getstring(pktin, &str2, &len2) ||
		!sftp_pkt_getattrs(sftp, pktin, &ret->names[i].attrs)) {
		fxp_internal_error(sftp, "malformed FXP_NAME packet");
		while (i--) {
		    sfree(ret->names[i].filename);
		    sfree(ret->names[i].longname);
		}
		sfree(ret->names);
		sfree(ret);
		sfree(pktin);
		return NULL;
	    }
	    ret->names[i].filename = mkstr(sftp, str1, len1);
	    ret->names[i].longname = mkstr(sftp, str2, len2);
	}
        sftp_pkt_free(sftp, pktin);
	return ret;
    } else {
	fxp_got_status(sftp, pktin);
        sftp_pkt_free(sftp, pktin);
	return NULL;
    }
}

/*
 * Write to a file. Returns 0 on error, 1 on OK.
 */
struct sftp_request *fxp_write_send(sftp_handle* sftp, struct fxp_handle *handle,
				    char *buffer, uint64 offset, int len)
{
    struct sftp_request *req = sftp_alloc_request(sftp);
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(sftp, SSH_FXP_WRITE);
    sftp_pkt_adduint32(sftp, pktout, req->id);
    sftp_pkt_addstring_start(sftp, pktout);
    sftp_pkt_addstring_data(sftp, pktout, handle->hstring, handle->hlen);
    sftp_pkt_adduint64(sftp, pktout, offset);
    sftp_pkt_addstring_start(sftp, pktout);
    sftp_pkt_addstring_data(sftp, pktout, buffer, len);
    sftp_send(sftp, pktout);

    return req;
}

int fxp_write_recv(sftp_handle* sftp, struct sftp_packet *pktin, struct sftp_request *req)
{
    sfree(req);
    fxp_got_status(sftp, pktin);
    sftp_pkt_free(sftp, pktin);
    return fxp_errtype == SSH_FX_OK;
}

/*
 * Free up an fxp_names structure.
 */
void fxp_free_names(sftp_handle* sftp, struct fxp_names *names)
{
    int i;

    for (i = 0; i < names->nnames; i++) {
	sfree(names->names[i].filename);
	sfree(names->names[i].longname);
    }
    sfree(names->names);
    sfree(names);
}

/*
 * Duplicate an fxp_name structure.
 */
struct fxp_name *fxp_dup_name(sftp_handle* sftp, struct fxp_name *name)
{
    struct fxp_name *ret;
    ret = snew(struct fxp_name);
    ret->filename = dupstr(name->filename);
    ret->longname = dupstr(name->longname);
    ret->attrs = name->attrs;	       /* structure copy */
    return ret;
}

/*
 * Free up an fxp_name structure.
 */
void fxp_free_name(sftp_handle* sftp, struct fxp_name *name)
{
    sfree(name->filename);
    sfree(name->longname);
    sfree(name);
}

/*
 * Store user data in an sftp_request structure.
 */
void *fxp_get_userdata(sftp_handle* sftp, struct sftp_request *req)
{
    return req->userdata;
}

void fxp_set_userdata(sftp_handle* sftp, struct sftp_request *req, void *data)
{
    req->userdata = data;
}

/*
 * A wrapper to go round fxp_read_* and fxp_write_*, which manages
 * the queueing of multiple read/write requests.
 */

struct req {
    char *buffer;
    int len, retlen, complete;
    uint64 offset;
    struct req *next, *prev;
};

struct fxp_xfer {
    uint64 offset, furthestdata, filesize;
    int req_totalsize, req_maxsize, eof, err;
    struct fxp_handle *fh;
    struct req *head, *tail;
};

static struct fxp_xfer *xfer_init(sftp_handle* sftp, struct fxp_handle *fh, uint64 offset)
{
    struct fxp_xfer *xfer = snew(struct fxp_xfer);

    xfer->fh = fh;
    xfer->offset = offset;
    xfer->head = xfer->tail = NULL;
    xfer->req_totalsize = 0;
    xfer->req_maxsize = 1048576;
    xfer->err = 0;
    xfer->filesize = uint64_make(ULONG_MAX, ULONG_MAX);
    xfer->furthestdata = uint64_make(0, 0);

    return xfer;
}

int xfer_done(sftp_handle* sftp, struct fxp_xfer *xfer)
{
    /*
     * We're finished if we've seen EOF _and_ there are no
     * outstanding requests.
     */
    return (xfer->eof || xfer->err) && !xfer->head;
}

void xfer_download_queue(sftp_handle* sftp, struct fxp_xfer *xfer)
{
    while (xfer->req_totalsize < xfer->req_maxsize &&
	   !xfer->eof && !xfer->err) {
	/*
	 * Queue a new read request.
	 */
	struct req *rr;
	struct sftp_request *req;

	rr = snew(struct req);
	rr->offset = xfer->offset;
	rr->complete = 0;
	if (xfer->tail) {
	    xfer->tail->next = rr;
	    rr->prev = xfer->tail;
	} else {
	    xfer->head = rr;
	    rr->prev = NULL;
	}
	xfer->tail = rr;
	rr->next = NULL;

	rr->len = 32768;
	rr->buffer = snewn(rr->len, char);
	sftp_register(sftp, req = fxp_read_send(sftp, xfer->fh, rr->offset, rr->len));
	fxp_set_userdata(sftp, req, rr);

	xfer->offset = uint64_add32(xfer->offset, rr->len);
	xfer->req_totalsize += rr->len;

#ifdef DEBUG_DOWNLOAD
	{ char buf[40]; uint64_decimal(rr->offset, buf); printf("queueing read request %p at %s\n", rr, buf); }
#endif
    }
}

struct fxp_xfer *xfer_download_init(sftp_handle* sftp, struct fxp_handle *fh, uint64 offset)
{
    struct fxp_xfer *xfer = xfer_init(sftp, fh, offset);

    xfer->eof = FALSE;
    xfer_download_queue(sftp, xfer);

    return xfer;
}

int xfer_download_gotpkt(sftp_handle* sftp, struct fxp_xfer *xfer, struct sftp_packet *pktin)
{
    struct sftp_request *rreq;
    struct req *rr;

    rreq = sftp_find_request(sftp, pktin);
    rr = (struct req *)fxp_get_userdata(sftp, rreq);
    if (!rr)
	return 0;		       /* this packet isn't ours */
    rr->retlen = fxp_read_recv(sftp, pktin, rreq, rr->buffer, rr->len);
#ifdef DEBUG_DOWNLOAD
    printf("read request %p has returned [%d]\n", rr, rr->retlen);
#endif

    if ((rr->retlen < 0 && fxp_error_type(sftp)==SSH_FX_EOF) || rr->retlen == 0) {
	xfer->eof = TRUE;
	rr->complete = -1;
#ifdef DEBUG_DOWNLOAD
	printf("setting eof\n");
#endif
    } else if (rr->retlen < 0) {
	/* some error other than EOF; signal it back to caller */
	xfer_set_error(sftp, xfer);
	rr->complete = -1;
	return -1;
    }

    rr->complete = 1;

    /*
     * Special case: if we have received fewer bytes than we
     * actually read, we should do something. For the moment I'll
     * just throw an ersatz FXP error to signal this; the SFTP
     * draft I've got says that it can't happen except on special
     * files, in which case seeking probably has very little
     * meaning and so queueing an additional read request to fill
     * up the gap sounds like the wrong answer. I'm not sure what I
     * should be doing here - if it _was_ a special file, I suspect
     * I simply shouldn't have been queueing multiple requests in
     * the first place...
     */
    if (rr->retlen > 0 && uint64_compare(xfer->furthestdata, rr->offset) < 0) {
	xfer->furthestdata = rr->offset;
#ifdef DEBUG_DOWNLOAD
	{ char buf[40];
	uint64_decimal(xfer->furthestdata, buf);
	printf("setting furthestdata = %s\n", buf); }
#endif
    }

    if (rr->retlen < rr->len) {
	uint64 filesize = uint64_add32(rr->offset,
				       (rr->retlen < 0 ? 0 : rr->retlen));
#ifdef DEBUG_DOWNLOAD
	{ char buf[40];
	uint64_decimal(filesize, buf);
	printf("short block! trying filesize = %s\n", buf); }
#endif
	if (uint64_compare(xfer->filesize, filesize) > 0) {
	    xfer->filesize = filesize;
#ifdef DEBUG_DOWNLOAD
	    printf("actually changing filesize\n");
#endif	    
	}
    }

    if (uint64_compare(xfer->furthestdata, xfer->filesize) > 0) {
	fxp_error_message = "received a short buffer from FXP_READ, but not"
	    " at EOF";
	fxp_errtype = -1;
	xfer_set_error(sftp, xfer);
	return -1;
    }

    return 1;
}

void xfer_set_error(sftp_handle* sftp, struct fxp_xfer *xfer)
{
    xfer->err = 1;
}

int xfer_download_data(sftp_handle* sftp, struct fxp_xfer *xfer, void **buf, int *len)
{
    void *retbuf = NULL;
    int retlen = 0;

    /*
     * Discard anything at the head of the rr queue with complete <
     * 0; return the first thing with complete > 0.
     */
    while (xfer->head && xfer->head->complete && !retbuf) {
	struct req *rr = xfer->head;

	if (rr->complete > 0) {
	    retbuf = rr->buffer;
	    retlen = rr->retlen;
#ifdef DEBUG_DOWNLOAD
	    printf("handing back data from read request %p\n", rr);
#endif
	}
#ifdef DEBUG_DOWNLOAD
	else
	    printf("skipping failed read request %p\n", rr);
#endif

	xfer->head = xfer->head->next;
	if (xfer->head)
	    xfer->head->prev = NULL;
	else
	    xfer->tail = NULL;
	xfer->req_totalsize -= rr->len;
	sfree(rr);
    }

    if (retbuf) {
	*buf = retbuf;
	*len = retlen;
	return 1;
    } else
	return 0;
}

struct fxp_xfer *xfer_upload_init(sftp_handle* sftp, struct fxp_handle *fh, uint64 offset)
{
    struct fxp_xfer *xfer = xfer_init(sftp, fh, offset);

    /*
     * We set `eof' to 1 because this will cause xfer_done() to
     * return true iff there are no outstanding requests. During an
     * upload, our caller will be responsible for working out
     * whether all the data has been sent, so all it needs to know
     * from us is whether the outstanding requests have been
     * handled once that's done.
     */
    xfer->eof = 1;

    return xfer;
}

int xfer_upload_ready(sftp_handle* sftp, struct fxp_xfer *xfer)
{
    if (xfer->req_totalsize < xfer->req_maxsize)
	return 1;
    else
	return 0;
}

void xfer_upload_data(sftp_handle* sftp, struct fxp_xfer *xfer, char *buffer, int len)
{
    struct req *rr;
    struct sftp_request *req;

    rr = snew(struct req);
    rr->offset = xfer->offset;
    rr->complete = 0;
    if (xfer->tail) {
	xfer->tail->next = rr;
	rr->prev = xfer->tail;
    } else {
	xfer->head = rr;
	rr->prev = NULL;
    }
    xfer->tail = rr;
    rr->next = NULL;

    rr->len = len;
    rr->buffer = NULL;
    sftp_register(sftp, req = fxp_write_send(sftp, xfer->fh, buffer, rr->offset, len));
    fxp_set_userdata(sftp, req, rr);

    xfer->offset = uint64_add32(xfer->offset, rr->len);
    xfer->req_totalsize += rr->len;

#ifdef DEBUG_UPLOAD
    { char buf[40]; uint64_decimal(rr->offset, buf); printf("queueing write request %p at %s [len %d]\n", rr, buf, len); }
#endif
}

int xfer_upload_gotpkt(sftp_handle* sftp, struct fxp_xfer *xfer, struct sftp_packet *pktin)
{
    struct sftp_request *rreq;
    struct req *rr, *prev, *next;
    int ret;

    rreq = sftp_find_request(sftp, pktin);
    rr = (struct req *)fxp_get_userdata(sftp, rreq);
    if (!rr)
	return 0;		       /* this packet isn't ours */
    ret = fxp_write_recv(sftp, pktin, rreq);
#ifdef DEBUG_UPLOAD
    printf("write request %p has returned [%d]\n", rr, ret);
#endif

    /*
     * Remove this one from the queue.
     */
    prev = rr->prev;
    next = rr->next;
    if (prev)
	prev->next = next;
    else
	xfer->head = next;
    if (next)
	next->prev = prev;
    else
	xfer->tail = prev;
    xfer->req_totalsize -= rr->len;
    sfree(rr);

    if (!ret)
	return -1;

    return 1;
}

void xfer_cleanup(sftp_handle* sftp, struct fxp_xfer *xfer)
{
    struct req *rr;
    while (xfer->head) {
	rr = xfer->head;
	xfer->head = xfer->head->next;
	sfree(rr->buffer);
	sfree(rr);
    }
    sfree(xfer);
}
