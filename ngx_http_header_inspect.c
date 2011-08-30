/*
 * ngx_http_header_inspect - Inspect HTTP headers
 *
 * Copyright (c) 2011, Andreas Jaggi <andreas.jaggi@waterwave.ch>
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_array.h>



typedef struct {
	ngx_flag_t inspect;
	ngx_flag_t log;
	ngx_flag_t log_uninspected;
	ngx_flag_t block;

	ngx_uint_t range_max_bytesets;
} ngx_header_inspect_loc_conf_t;



static ngx_int_t ngx_header_inspect_init(ngx_conf_t *cf);
static ngx_int_t ngx_header_inspect_http_date(u_char *data, ngx_uint_t maxlen);
static ngx_int_t ngx_header_inspect_entity_tag(u_char *data, ngx_uint_t maxlen);
static ngx_int_t ngx_header_inspect_range_header(ngx_header_inspect_loc_conf_t *conf, ngx_log_t *log, ngx_str_t value);
static ngx_int_t ngx_header_inspect_acceptencoding_header(ngx_header_inspect_loc_conf_t *conf, ngx_log_t *log, ngx_str_t value);
static ngx_int_t ngx_header_inspect_ifrange_header(ngx_header_inspect_loc_conf_t *conf, ngx_log_t *log, ngx_str_t value);
static ngx_int_t ngx_header_inspect_date_header(ngx_header_inspect_loc_conf_t *conf, ngx_log_t *log, char *header, ngx_str_t value);
static ngx_int_t ngx_header_inspect_process_request(ngx_http_request_t *r);

static void *ngx_header_inspect_create_conf(ngx_conf_t *cf);
static char *ngx_header_inspect_merge_conf(ngx_conf_t *cf, void *parent, void *child);



static ngx_command_t ngx_header_inspect_commands[] = {
	{
		ngx_string("inspect_headers"),
		NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
		ngx_conf_set_flag_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_header_inspect_loc_conf_t, inspect),
		NULL
	},
	{
		ngx_string("inspect_headers_log_violations"),
		NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
		ngx_conf_set_flag_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_header_inspect_loc_conf_t, log),
		NULL
	},
	{
		ngx_string("inspect_headers_block_violations"),
		NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
		ngx_conf_set_flag_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_header_inspect_loc_conf_t, block),
		NULL
	},
	{
		ngx_string("inspect_headers_log_uninspected"),
		NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
		ngx_conf_set_flag_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_header_inspect_loc_conf_t, log_uninspected),
		NULL
	},
	{
		ngx_string("inspect_headers_range_max_bytesets"),
		NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
		ngx_conf_set_num_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_header_inspect_loc_conf_t, range_max_bytesets),
		NULL
	},
	ngx_null_command
};

static ngx_http_module_t ngx_header_inspect_module_ctx = {
	NULL,                             /* preconfiguration */
	ngx_header_inspect_init,          /* postconfiguration */

	NULL,                             /* create main configuration */
	NULL,                             /* init main configuration */

	NULL,                             /* create server configuration */
	NULL,                             /* merge server configuration */

	ngx_header_inspect_create_conf,   /* create location configuration */
	ngx_header_inspect_merge_conf,    /* merge location configuration */
};

ngx_module_t ngx_http_header_inspect_module = {
	NGX_MODULE_V1,
	&ngx_header_inspect_module_ctx, /* module context */
	ngx_header_inspect_commands,    /* module directives */
	NGX_HTTP_MODULE,                /* module type */
	NULL,                           /* init master */
	NULL,                           /* init module */
	NULL,                           /* init process */
	NULL,                           /* init thread */
	NULL,                           /* exit thread */
	NULL,                           /* exit process */
	NULL,                           /* exit master */
	NGX_MODULE_V1_PADDING
};



static ngx_int_t ngx_header_inspect_init(ngx_conf_t *cf) {
	ngx_http_handler_pt       *h;
	ngx_http_core_main_conf_t *cmcf;

	cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

	h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
	if (h == NULL) {
		return NGX_ERROR;
	}

	*h = ngx_header_inspect_process_request;

	return NGX_OK;
}

static ngx_int_t ngx_header_inspect_range_header(ngx_header_inspect_loc_conf_t *conf, ngx_log_t *log, ngx_str_t value) {
	ngx_uint_t i,a,b,setcount;
	ngx_int_t rc = NGX_OK;
	enum range_header_states {RHS_NEWSET,RHS_NUM1,DELIM,RHS_NUM2,RHS_SUFDELIM,RHS_SUFNUM} state;

	if ( (value.len < 6) || (ngx_strncmp("bytes=", value.data, 6) != 0) ) {
		if ( conf->log ) {
			ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: Range header does not start with \"bytes=\"");
		}
		rc = NGX_ERROR;
	}

	setcount = 1;
	a = 0;
	b = 0;
	state = RHS_NEWSET;

	i = 6; /* start after bytes= */
	for ( ; i < value.len ; i++ ) {

		switch (value.data[i]) {
			case ',':
				if ( (state != DELIM) && (state != RHS_NUM2) && (state != RHS_SUFNUM) ) {
					if ( conf->log ) {
						ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: unexpected ',' at position %d in Range header \"%s\"", i, value.data);
					}
					rc = NGX_ERROR;
				}
				if ( state == RHS_NUM2 ) {
					/* verify a <= b in 'a-b' sets */
					if ( a > b ) {
						if ( conf->log ) {
							ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: invalid range definition at position %d in Range header \"%s\"", i, value.data);
						}
						rc = NGX_ERROR;
					}
				}
				setcount++;
				a = 0;
				b = 0;
				state = RHS_NEWSET;
				break;

			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				if ((state == RHS_NEWSET) || (state == RHS_NUM1)) {
					a = a*10 + (value.data[i] - '0');
					state = RHS_NUM1;
				} else if ((state == DELIM) || (state == RHS_NUM2)) {
					b = b*10 + (value.data[i] - '0');
					state = RHS_NUM2;
				} else if ((state == RHS_SUFDELIM) || (state == RHS_SUFNUM)) {
					state = RHS_SUFNUM;
				} else {
					if ( conf->log ) {
						ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: unexpected digit at position %d in Range header \"%s\"", i, value.data);
					}
					rc = NGX_ERROR;
				}
				break;

			case '-':
				if (state == RHS_NEWSET) {
					state = RHS_SUFDELIM;
				} else if (state == RHS_NUM1) {
					state = DELIM;
				} else {
					if ( conf->log ) {
						ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: unexpected '-' at position %d in Range header \"%s\"", i, value.data);
					}
					rc = NGX_ERROR;
				}
				break;

			default:
				if ( conf->log ) {
					ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: illegal char at position %d in Range header \"%s\"", i, value.data);
				}
				rc = NGX_ERROR;
		}

		if (setcount > conf->range_max_bytesets) {
			if ( conf->log ) {
				ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: Range header contains more than %d bytesets", conf->range_max_bytesets);
			}
			return NGX_ERROR;
			break;
		}
	}

	if ((state != DELIM) && (state != RHS_NUM2) && (state != RHS_SUFNUM)) {
		if ( conf->log ) {
			ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: Range header \"%s\" contains incomplete byteset definition", value.data);
		}
		rc = NGX_ERROR;
	}
	if ( state == RHS_NUM2 ) {
		/* verify a <= b in 'a-b' sets */
		if ( a > b ) {
			if ( conf->log ) {
				ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: invalid range definition at position %d in Range header \"%s\"", i, value.data);
			}
			rc = NGX_ERROR;
		}
	}

	return rc;
}

static ngx_int_t ngx_header_inspect_http_date(u_char *data, ngx_uint_t maxlen) {
	ngx_uint_t i;
	enum http_date_type {RFC1123, RFC850, ASCTIME} type;

	if ( maxlen < 24 ) {
		return NGX_ERROR;
	}

	if ((data[0] == 'M') && (data[1] == 'o') && (data[2] == 'n')) {
	/* Mon(day) */
		switch (data[3]) {
			case ',':
				type = RFC1123;
				i = 4;
				break;
			case ' ':
				type = ASCTIME;
				i = 3;
				break;
			case 'd':
				type = RFC850;
				if (
					(data[4] != 'a') ||
					(data[5] != 'y') ||
					(data[6] != ',')
				) {
					return NGX_ERROR;
				}
				i = 7;
				break;
			default:
				return NGX_ERROR;
		}
	} else if ((data[0] == 'T') && (data[1] == 'u') && (data[2] == 'e')) {
	/* Tue(sday) */
		switch (data[3]) {
			case ',':
				type = RFC1123;
				i = 4;
				break;
			case ' ':
				type = ASCTIME;
				i = 3;
				break;
			case 's':
				type = RFC850;
				if (
					(data[4] != 'd') ||
					(data[5] != 'a') ||
					(data[6] != 'y') ||
					(data[7] != ',')
				) {
					return NGX_ERROR;
				}
				i = 8;
				break;
			default:
				return NGX_ERROR;
		}
	} else if ((data[0] == 'W') && (data[1] == 'e') && (data[2] == 'd')) {
	/* Wed(nesday) */
		switch (data[3]) {
			case ',':
				type = RFC1123;
				i = 4;
				break;
			case ' ':
				type = ASCTIME;
				i = 3;
				break;
			case 'n':
				type = RFC850;
				if (
					(data[4] != 'e') ||
					(data[5] != 's') ||
					(data[6] != 'd') ||
					(data[7] != 'a') ||
					(data[8] != 'y') ||
					(data[9] != ',')
				) {
					return NGX_ERROR;
				}
				i = 10;
				break;
			default:
				return NGX_ERROR;
		}
	} else if ((data[0] == 'T') && (data[1] == 'h') && (data[2] == 'u')) {
	/* Thu(rsday) */
		switch (data[3]) {
			case ',':
				type = RFC1123;
				i = 4;
				break;
			case ' ':
				type = ASCTIME;
				i = 3;
				break;
			case 'r':
				type = RFC850;
				if (
					(data[4] != 's') ||
					(data[5] != 'd') ||
					(data[6] != 'a') ||
					(data[7] != 'y') ||
					(data[8] != ',')
				) {
					return NGX_ERROR;
				}
				i = 9;
				break;
			default:
				return NGX_ERROR;
		}
	} else if ((data[0] == 'F') && (data[1] == 'r') && (data[2] == 'i')) {
	/* Fri(day) */
		switch (data[3]) {
			case ',':
				type = RFC1123;
				i = 4;
				break;
			case ' ':
				type = ASCTIME;
				i = 3;
				break;
			case 'd':
				type = RFC850;
				if (
					(data[4] != 'a') ||
					(data[5] != 'y') ||
					(data[6] != ',')
				) {
					return NGX_ERROR;
				}
				i = 7;
				break;
			default:
				return NGX_ERROR;
		}
	} else if ((data[0] == 'S') && (data[1] == 'a') && (data[2] == 't')) {
	/* Sat(urday) */
		switch (data[3]) {
			case ',':
				type = RFC1123;
				i = 4;
				break;
			case ' ':
				type = ASCTIME;
				i = 3;
				break;
			case 'u':
				type = RFC850;
				if (
					(data[4] != 'r') ||
					(data[5] != 'd') ||
					(data[6] != 'a') ||
					(data[7] != 'y') ||
					(data[8] != ',')
				) {
					return NGX_ERROR;
				}
				i = 9;
				break;
			default:
				return NGX_ERROR;
		}
	} else if ((data[0] == 'S') && (data[1] == 'u') && (data[2] == 'n')) {
	/* Sun(day) */
		switch (data[3]) {
			case ',':
				type = RFC1123;
				i = 4;
				break;
			case ' ':
				type = ASCTIME;
				i = 3;
				break;
			case 'd':
				type = RFC850;
				if (
					(data[4] != 'a') ||
					(data[5] != 'y') ||
					(data[6] != ',')
				) {
					return NGX_ERROR;
				}
				i = 7;
				break;
			default:
				return NGX_ERROR;
		}
	} else {
		return NGX_ERROR;
	}

	switch (type) {
		case RFC1123:
			if (maxlen != 29) {
				return NGX_ERROR;
			}
			break;
		case RFC850:
			if (maxlen != 30) {
				return NGX_ERROR;
			}
			break;
		case ASCTIME:
			if (maxlen != 24) {
				return NGX_ERROR;
			}
			break;
		default:
			return NGX_ERROR;
	}

	if (data[i] != ' ') {
		return NGX_ERROR;
	}
	i++;

	if (type == RFC1123) {
	/* rfc1123: day */
		if ((data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
		if ((data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
		if (data[i] != ' ') {
			return NGX_ERROR;
		}
		i++;
	} else if (type == RFC850) {
	/* rfc850: day */
		if ((data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
		if ((data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
		if (data[i] != '-') {
			return NGX_ERROR;
		}
		i++;
	}

	/* month: Nov */
	if (
		((data[i] == 'J') && (data[i+1] == 'a') && (data[i+2] == 'n')) ||
		((data[i] == 'F') && (data[i+1] == 'e') && (data[i+2] == 'b')) ||
		((data[i] == 'M') && (data[i+1] == 'a') && (data[i+2] == 'r')) ||
		((data[i] == 'A') && (data[i+1] == 'p') && (data[i+2] == 'r')) ||
		((data[i] == 'M') && (data[i+1] == 'a') && (data[i+2] == 'y')) ||
		((data[i] == 'J') && (data[i+1] == 'u') && (data[i+2] == 'n')) ||
		((data[i] == 'J') && (data[i+1] == 'u') && (data[i+2] == 'l')) ||
		((data[i] == 'A') && (data[i+1] == 'u') && (data[i+2] == 'g')) ||
		((data[i] == 'S') && (data[i+1] == 'e') && (data[i+2] == 'p')) ||
		((data[i] == 'O') && (data[i+1] == 'c') && (data[i+2] == 't')) ||
		((data[i] == 'N') && (data[i+1] == 'o') && (data[i+2] == 'v')) ||
		((data[i] == 'D') && (data[i+1] == 'e') && (data[i+2] == 'c'))
	) {
		i += 3;
	} else {
		return NGX_ERROR;
	}

	if (type == RFC1123) {
	/* rfc1123: year */
		if (data[i] != ' ') {
			return NGX_ERROR;
		}
		i++;
		if ((data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
		if ((data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
		if ((data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
	} else if (type == RFC850) {
	/* rfc850: year */
		if (data[i] != '-') {
			return NGX_ERROR;
		}
		i++;
		if ((data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
	} else if (type == ASCTIME) {
	/* asctime: day */
		if (data[i] != ' ') {
			return NGX_ERROR;
		}
		i++;
		if ((data[i] != ' ') || (data[i] < '0') || (data[i] > '9')) {
			return NGX_ERROR;
		}
		i++;
	}
	if ((data[i] < '0') || (data[i] > '9')) {
		return NGX_ERROR;
	}
	i++;
	if (data[i] != ' ') {
		return NGX_ERROR;
	}
	i++;

	/* time 08:49:37 */
	if (
		(data[i] < '0') || (data[i] > '9') ||
		(data[i+1] < '0') || (data[i+1] > '9') ||
		(data[i+2] != ':')
	) {
		return NGX_ERROR;
	}
	i += 3;
	if (
		(data[i] < '0') || (data[i] > '9') ||
		(data[i+1] < '0') || (data[i+1] > '9') ||
		(data[i+2] != ':')
	) {
		return NGX_ERROR;
	}
	i += 3;
	if (
		(data[i] < '0') || (data[i] > '9') ||
		(data[i+1] < '0') || (data[i+1] > '9') ||
		(data[i+2] != ' ')
	) {
		return NGX_ERROR;
	}
	i += 3;

	if (type == ASCTIME) {
	/* asctime: year: 1994 */
		if (
			(data[i] < '0') || (data[i] > '9') ||
			(data[i+1] < '0') || (data[i+1] > '9') ||
			(data[i+2] < '0') || (data[i+2] > '9') ||
			(data[i+3] < '0') || (data[i+3] > '9')
		) {
			return NGX_ERROR;
		}
		i += 4;
	} else {
		/* GMT */
		if ((data[i] != 'G') || (data[i+1] != 'M') || (data[i+2] != 'T')) {
			return NGX_ERROR;
		}
		i += 3;
	}

	if ( i != maxlen ) {
		return NGX_ERROR;
	}

	return NGX_OK;
}

static ngx_int_t ngx_header_inspect_entity_tag(u_char *data, ngx_uint_t maxlen) {
	ngx_uint_t i = 0;

	if ( maxlen < 2 ) {
		return NGX_ERROR;
	}

	if ( data[0] == 'W' ) {
		if ( data[1] != '/' ) {
			return NGX_ERROR;
		}
		i = 2;
	}

	if ( i+1 >= maxlen ) {
		return NGX_ERROR;
	}

	if ( data[i] != '"' ) {
		return NGX_ERROR;
	}
	i++;

	for ( ; i < maxlen-1 ; i++ ) {
		if ( data[i] == '"' ) {
			return NGX_ERROR;
		}
	}

	if ( data[maxlen-1] != '"' ) {
		return NGX_ERROR;
	}

	return NGX_OK;
}

static ngx_int_t ngx_header_inspect_parse_qvalue(u_char *data, ngx_uint_t maxlen, ngx_uint_t *len) {

	*len = 0;

	if ((maxlen < 3) || (data[0] != 'q') || (data[1] != '=')) {
		return NGX_ERROR;
	}

	if (data[2] == '0') {
		if ((maxlen == 3) || (data[3] != '.')) {
			*len = 3;
			return NGX_OK;
		}
		if ((data[4] < '0') || (data[4] > '9')) {
			*len = 4;
			return NGX_OK;
		}
		if ((data[5] < '0') || (data[5] > '9')) {
			*len = 5;
			return NGX_OK;
		}
		if ((data[6] < '0') || (data[6] > '9')) {
			*len = 6;
		} else {
			*len = 7;
		}
		return NGX_OK;
	} else if (data[2] == '1') {
		if ((maxlen == 3) || (data[3] != '.')) {
			*len = 3;
			return NGX_OK;
		}
		if (data[4] != '0') {
			*len = 4;
			return NGX_OK;
		}
		if (data[5] != '0') {
			*len = 5;
			return NGX_OK;
		}
		if (data[6] != '0') {
			*len = 6;
		} else {
			*len = 7;
		}
		return NGX_OK;
	} else {
		*len = 2;
		return NGX_ERROR;
	}
}

static ngx_int_t ngx_header_inspect_parse_contentcoding(u_char *data, ngx_uint_t maxlen, ngx_uint_t *len) {

	if (maxlen < 1) {
		*len = 0;
		return NGX_ERROR;
	}
	*len = 1;

	switch (data[0]) {
		case '*':
			return NGX_OK;
			break;
		case 'c':
			if ( (maxlen < 8) || (ngx_strncmp("compress", data, 8) != 0)) {
				return NGX_ERROR;
			}
			*len = 8;
			break;
		case 'd':
			if ( (maxlen < 7) || (ngx_strncmp("deflate", data, 7) != 0)) {
				return NGX_ERROR;
			}
			*len = 7;
			break;
		case 'e':
			if ( (maxlen < 3) || (ngx_strncmp("exi", data, 3) != 0)) {
				return NGX_ERROR;
			}
			*len = 3;
			break;
		case 'g':
			if ( (maxlen < 4) || (ngx_strncmp("gzip", data, 4) != 0)) {
				return NGX_ERROR;
			}
			*len = 4;
			break;
		case 'i':
			if ( (maxlen < 8) || (ngx_strncmp("identity", data, 8) != 0)) {
				return NGX_ERROR;
			}
			*len = 8;
			break;
		case 'p':
			if ( (maxlen < 12) || (ngx_strncmp("pack200-gzip", data, 12) != 0)) {
				return NGX_ERROR;
			}
			*len = 12;
			break;
		default:
			return NGX_ERROR;
	}

	return NGX_OK;
}

static ngx_int_t ngx_header_inspect_acceptencoding_header(ngx_header_inspect_loc_conf_t *conf, ngx_log_t *log, ngx_str_t value) {
	ngx_int_t rc = NGX_AGAIN;
	ngx_uint_t i = 0;
	ngx_uint_t v;

	if ((value.len == 0) || ((value.len == 1) && (value.data[0] == '*'))) {
		return NGX_OK;
	}

	while ( i < value.len) {
		if (ngx_header_inspect_parse_contentcoding(&(value.data[i]), value.len-i, &v) != NGX_OK) {
			ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: invalid content-coding at position %d in Accept-Encoding header \"%s\"", i, value.data);
			rc = NGX_ERROR;
			break;
		}
		i += v;
		if ((value.data[i] == ' ') && (i < value.len)) {
			i++;
		}
		if (i == value.len) {
			rc = NGX_OK;
			break;
		}
		if (value.data[i] == ';') {
			i++;
			if (i >= value.len) {
				ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: unexpected end of Accept-Encoding header \"%s\"", value.data);
				rc = NGX_ERROR;
				break;
			}
			if ((value.data[i] == ' ') && (i < value.len)) {
				i++;
			}
			if (ngx_header_inspect_parse_qvalue(&(value.data[i]), value.len-i, &v) != NGX_OK) {
				ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: invalid qvalue at position %d in Accept-Encoding header \"%s\"", i, value.data);
				rc = NGX_ERROR;
				break;
			}
			i += v;
			if ((value.data[i] == ' ') && (i < value.len)) {
				i++;
			}
			if (i == value.len) {
				rc = NGX_OK;
				break;
			}
		}
		if (value.data[i] != ',') {
			ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: illegal char at position %d in Accept-Encoding header \"%s\"", i, value.data);
			rc = NGX_ERROR;
			break;
		}
		i++;
		if ((value.data[i] == ' ') && (i < value.len)) {
			i++;
		}
	}

	if (rc == NGX_AGAIN) {
		ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: unexpected end of Accept-Encoding header \"%s\"", value.data);
		rc = NGX_ERROR;
	}

	return rc;
}

static ngx_int_t ngx_header_inspect_ifrange_header(ngx_header_inspect_loc_conf_t *conf, ngx_log_t *log, ngx_str_t value) {
	if (((value.data[0] == 'W') && (value.data[1] == '/'))|| (value.data[0] == '"')) {
	/* 1. entity-tag */
		if ( ngx_header_inspect_entity_tag(value.data, value.len) != NGX_OK ) {
			if ( conf->log ) {
				ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: invalid entity-tag in If-Range header \"%s\"", value.data);
			}
			return NGX_ERROR;
		}
	} else {
	/* 2. HTTP-date */
		return ngx_header_inspect_date_header(conf, log, "If-Range", value);
	}

	return NGX_OK;
}

static ngx_int_t ngx_header_inspect_date_header(ngx_header_inspect_loc_conf_t *conf, ngx_log_t *log, char *header, ngx_str_t value) {

	/* HTTP-date */
	if ( ngx_header_inspect_http_date(value.data, value.len) != NGX_OK ) {
		if ( conf->log ) {
			ngx_log_error(NGX_LOG_ALERT, log, 0, "header_inspect: invalid HTTP-date in \"%s\" header \"%s\"", header, value.data);
		}
		return NGX_ERROR;
	}

	return NGX_OK;
}

static ngx_int_t ngx_header_inspect_process_request(ngx_http_request_t *r) {
	ngx_header_inspect_loc_conf_t *conf;
	ngx_table_elt_t *h;
	ngx_list_part_t *part;
	ngx_uint_t i;
	ngx_int_t rc;

	conf = ngx_http_get_module_loc_conf(r, ngx_http_header_inspect_module);

	if (conf->inspect) {
		part = &r->headers_in.headers.part;
		do {
			h = part->elts;
			for (i = 0; i < part->nelts; i++) {
				if ((h[i].key.len == 5) && (ngx_strcmp("Range", h[i].key.data) == 0)) {
					rc = ngx_header_inspect_range_header(conf, r->connection->log, h[i].value);
					if ((rc != NGX_OK) && conf->block) {
						return NGX_HTTP_BAD_REQUEST;
					}
				} else if ((h[i].key.len == 8) && (ngx_strcmp("If-Range", h[i].key.data) == 0) ) {
					rc = ngx_header_inspect_ifrange_header(conf, r->connection->log, h[i].value);
					if ((rc != NGX_OK) && conf->block) {
						return NGX_HTTP_BAD_REQUEST;
					}
				} else if ((h[i].key.len == 19) && (ngx_strcmp("If-Unmodified-Since", h[i].key.data) == 0) ) {
					rc = ngx_header_inspect_date_header(conf, r->connection->log, "If-Unmodified-Since", h[i].value);
					if ((rc != NGX_OK) && conf->block) {
						return NGX_HTTP_BAD_REQUEST;
					}
				} else if ((h[i].key.len == 17) && (ngx_strcmp("If-Modified-Since", h[i].key.data) == 0) ) {
					rc = ngx_header_inspect_date_header(conf, r->connection->log, "If-Modified-Since", h[i].value);
					if ((rc != NGX_OK) && conf->block) {
						return NGX_HTTP_BAD_REQUEST;
					}
				} else if ((h[i].key.len == 4) && (ngx_strcmp("Date", h[i].key.data) == 0) ) {
					rc = ngx_header_inspect_date_header(conf, r->connection->log, "Date", h[i].value);
					if ((rc != NGX_OK) && conf->block) {
						return NGX_HTTP_BAD_REQUEST;
					}
				} else if ((h[i].key.len == 15) && (ngx_strcmp("Accept-Encoding", h[i].key.data) == 0) ) {
					rc = ngx_header_inspect_acceptencoding_header(conf, r->connection->log, h[i].value);
					if ((rc != NGX_OK) && conf->block) {
						return NGX_HTTP_BAD_REQUEST;
					}
				} else {
					/* TODO: support for other headers */
					if (conf->log_uninspected) {
						ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0, "header_inspect: uninspected header \"%s: %s\"", h[i].key.data, h[i].value.data);
					}
				}
			}
			part = part->next;
		} while ( part != NULL );
	}

	return NGX_DECLINED;
}



static void *ngx_header_inspect_create_conf(ngx_conf_t *cf) {
	ngx_header_inspect_loc_conf_t *conf;

	conf = ngx_pcalloc(cf->pool, sizeof(ngx_header_inspect_loc_conf_t));
	if (conf == NULL) {
		return NGX_CONF_ERROR;
	}

	conf->inspect = NGX_CONF_UNSET;
	conf->log = NGX_CONF_UNSET;
	conf->block = NGX_CONF_UNSET;
	conf->log_uninspected = NGX_CONF_UNSET;

	conf->range_max_bytesets = NGX_CONF_UNSET_UINT;

	return conf;
}

static char *ngx_header_inspect_merge_conf(ngx_conf_t *cf, void *parent, void *child) {
	ngx_header_inspect_loc_conf_t *prev = parent;
	ngx_header_inspect_loc_conf_t *conf = child;

	ngx_conf_merge_off_value(conf->inspect, prev->inspect, 0);
	ngx_conf_merge_off_value(conf->log, prev->log, 1);
	ngx_conf_merge_off_value(conf->block, prev->block, 0);
	ngx_conf_merge_off_value(conf->log_uninspected, prev->log_uninspected, 0);

	ngx_conf_merge_uint_value(conf->range_max_bytesets, prev->range_max_bytesets, 5);

	return NGX_CONF_OK;
}