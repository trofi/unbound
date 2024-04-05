/*
 * daemon/remote.c - remote control for the unbound daemon.
 *
 * Copyright (c) 2008, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file contains the remote control functionality for the daemon.
 * The remote control can be performed using either the commandline
 * unbound-control tool, or a TLS capable web browser.
 * The channel is secured using TLSv1, and certificates.
 * Both the server and the client(control tool) have their own keys.
 */
#include "config.h"
#ifdef HAVE_OPENSSL_ERR_H
#include <openssl/err.h>
#endif
#ifdef HAVE_OPENSSL_DH_H
#include <openssl/dh.h>
#endif
#ifdef HAVE_OPENSSL_BN_H
#include <openssl/bn.h>
#endif
#ifdef HAVE_STDATOMIC_H
#include <stdatomic.h>
#endif

#include <ctype.h>
#include "daemon/remote.h"
#include "daemon/worker.h"
#include "daemon/daemon.h"
#include "daemon/stats.h"
#include "daemon/cachedump.h"
#include "util/log.h"
#include "util/config_file.h"
#include "util/net_help.h"
#include "util/module.h"
#include "util/ub_event.h"
#include "services/listen_dnsport.h"
#include "services/cache/rrset.h"
#include "services/cache/infra.h"
#include "services/mesh.h"
#include "services/localzone.h"
#include "services/authzone.h"
#include "services/rpz.h"
#include "util/storage/slabhash.h"
#include "util/fptr_wlist.h"
#include "util/data/dname.h"
#include "validator/validator.h"
#include "validator/val_kcache.h"
#include "validator/val_kentry.h"
#include "validator/val_anchor.h"
#include "iterator/iterator.h"
#include "iterator/iter_fwd.h"
#include "iterator/iter_hints.h"
#include "iterator/iter_delegpt.h"
#include "services/outbound_list.h"
#include "services/outside_network.h"
#include "sldns/str2wire.h"
#include "sldns/parseutil.h"
#include "sldns/wire2str.h"
#include "sldns/sbuffer.h"
#include "util/timeval_func.h"

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_POLL_H
#include <poll.h>
#endif

/* just for portability */
#ifdef SQ
#undef SQ
#endif

/** what to put on statistics lines between var and value, ": " or "=" */
#define SQ "="

/** What number of loop iterations is too much for ipc retries */
#define IPC_LOOP_MAX 200
/** Timeout in msec for ipc socket poll. */
#define IPC_NOTIFICATION_WAIT 200

static void fr_printq_delete(struct fast_reload_printq* printq);
static void fr_main_perform_printout(struct fast_reload_thread* fr);
static int fr_printq_empty(struct fast_reload_printq* printq);
static void fr_printq_list_insert(struct fast_reload_printq* printq,
	struct daemon* daemon);
static void fr_printq_remove(struct fast_reload_printq* printq);
static void fr_check_cmd_from_thread(struct fast_reload_thread* fr);

static int
remote_setup_ctx(struct daemon_remote* rc, struct config_file* cfg)
{
	char* s_cert;
	char* s_key;
	rc->ctx = SSL_CTX_new(SSLv23_server_method());
	if(!rc->ctx) {
		log_crypto_err("could not SSL_CTX_new");
		return 0;
	}
	if(!listen_sslctx_setup(rc->ctx)) {
		return 0;
	}

	s_cert = fname_after_chroot(cfg->server_cert_file, cfg, 1);
	s_key = fname_after_chroot(cfg->server_key_file, cfg, 1);
	if(!s_cert || !s_key) {
		log_err("out of memory in remote control fname");
		goto setup_error;
	}
	verbose(VERB_ALGO, "setup SSL certificates");
	if (!SSL_CTX_use_certificate_chain_file(rc->ctx,s_cert)) {
		log_err("Error for server-cert-file: %s", s_cert);
		log_crypto_err("Error in SSL_CTX use_certificate_chain_file");
		goto setup_error;
	}
	if(!SSL_CTX_use_PrivateKey_file(rc->ctx,s_key,SSL_FILETYPE_PEM)) {
		log_err("Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX use_PrivateKey_file");
		goto setup_error;
	}
	if(!SSL_CTX_check_private_key(rc->ctx)) {
		log_err("Error for server-key-file: %s", s_key);
		log_crypto_err("Error in SSL_CTX check_private_key");
		goto setup_error;
	}
	listen_sslctx_setup_2(rc->ctx);
	if(!SSL_CTX_load_verify_locations(rc->ctx, s_cert, NULL)) {
		log_crypto_err("Error setting up SSL_CTX verify locations");
	setup_error:
		free(s_cert);
		free(s_key);
		return 0;
	}
	SSL_CTX_set_client_CA_list(rc->ctx, SSL_load_client_CA_file(s_cert));
	SSL_CTX_set_verify(rc->ctx, SSL_VERIFY_PEER, NULL);
	free(s_cert);
	free(s_key);
	return 1;
}

struct daemon_remote*
daemon_remote_create(struct config_file* cfg)
{
	struct daemon_remote* rc = (struct daemon_remote*)calloc(1,
		sizeof(*rc));
	if(!rc) {
		log_err("out of memory in daemon_remote_create");
		return NULL;
	}
	rc->max_active = 10;

	if(!cfg->remote_control_enable) {
		rc->ctx = NULL;
		return rc;
	}
	if(options_remote_is_address(cfg) && cfg->control_use_cert) {
		if(!remote_setup_ctx(rc, cfg)) {
			daemon_remote_delete(rc);
			return NULL;
		}
		rc->use_cert = 1;
	} else {
		struct config_strlist* p;
		rc->ctx = NULL;
		rc->use_cert = 0;
		if(!options_remote_is_address(cfg))
		  for(p = cfg->control_ifs.first; p; p = p->next) {
			if(p->str && p->str[0] != '/')
				log_warn("control-interface %s is not using TLS, but plain transfer, because first control-interface in config file is a local socket (starts with a /).", p->str);
		}
	}
	return rc;
}

void daemon_remote_clear(struct daemon_remote* rc)
{
	struct rc_state* p, *np;
	if(!rc) return;
	/* but do not close the ports */
	listen_list_delete(rc->accept_list);
	rc->accept_list = NULL;
	/* do close these sockets */
	p = rc->busy_list;
	while(p) {
		np = p->next;
		if(p->ssl)
			SSL_free(p->ssl);
		comm_point_delete(p->c);
		free(p);
		p = np;
	}
	rc->busy_list = NULL;
	rc->active = 0;
	rc->worker = NULL;
}

void daemon_remote_delete(struct daemon_remote* rc)
{
	if(!rc) return;
	daemon_remote_clear(rc);
	if(rc->ctx) {
		SSL_CTX_free(rc->ctx);
	}
	free(rc);
}

/**
 * Add and open a new control port
 * @param ip: ip str
 * @param nr: port nr
 * @param list: list head
 * @param noproto_is_err: if lack of protocol support is an error.
 * @param cfg: config with username for chown of unix-sockets.
 * @return false on failure.
 */
static int
add_open(const char* ip, int nr, struct listen_port** list, int noproto_is_err,
	struct config_file* cfg)
{
	struct addrinfo hints;
	struct addrinfo* res;
	struct listen_port* n;
	int noproto = 0;
	int fd, r;
	char port[15];
	snprintf(port, sizeof(port), "%d", nr);
	port[sizeof(port)-1]=0;
	memset(&hints, 0, sizeof(hints));
	log_assert(ip);

	if(ip[0] == '/') {
		/* This looks like a local socket */
		fd = create_local_accept_sock(ip, &noproto, cfg->use_systemd);
		/*
		 * Change socket ownership and permissions so users other
		 * than root can access it provided they are in the same
		 * group as the user we run as.
		 */
		if(fd != -1) {
#ifdef HAVE_CHOWN
			chmod(ip, (mode_t)(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP));
			if (cfg->username && cfg->username[0] &&
				cfg_uid != (uid_t)-1) {
				if(chown(ip, cfg_uid, cfg_gid) == -1)
					verbose(VERB_QUERY, "cannot chown %u.%u %s: %s",
					  (unsigned)cfg_uid, (unsigned)cfg_gid,
					  ip, strerror(errno));
			}
#else
			(void)cfg;
#endif
		}
	} else {
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
		if((r = getaddrinfo(ip, port, &hints, &res)) != 0 || !res) {
#ifdef USE_WINSOCK
			if(!noproto_is_err && r == EAI_NONAME) {
				/* tried to lookup the address as name */
				return 1; /* return success, but do nothing */
			}
#endif /* USE_WINSOCK */
			log_err("control interface %s:%s getaddrinfo: %s %s",
				ip?ip:"default", port, gai_strerror(r),
#ifdef EAI_SYSTEM
				r==EAI_SYSTEM?(char*)strerror(errno):""
#else
				""
#endif
			);
			return 0;
		}

		/* open fd */
		fd = create_tcp_accept_sock(res, 1, &noproto, 0,
			cfg->ip_transparent, 0, 0, cfg->ip_freebind,
			cfg->use_systemd, cfg->ip_dscp);
		freeaddrinfo(res);
	}

	if(fd == -1 && noproto) {
		if(!noproto_is_err)
			return 1; /* return success, but do nothing */
		log_err("cannot open control interface %s %d : "
			"protocol not supported", ip, nr);
		return 0;
	}
	if(fd == -1) {
		log_err("cannot open control interface %s %d", ip, nr);
		return 0;
	}

	/* alloc */
	n = (struct listen_port*)calloc(1, sizeof(*n));
	if(!n) {
		sock_close(fd);
		log_err("out of memory");
		return 0;
	}
	n->next = *list;
	*list = n;
	n->fd = fd;
	return 1;
}

struct listen_port* daemon_remote_open_ports(struct config_file* cfg)
{
	struct listen_port* l = NULL;
	log_assert(cfg->remote_control_enable && cfg->control_port);
	if(cfg->control_ifs.first) {
		char** rcif = NULL;
		int i, num_rcif = 0;
		if(!resolve_interface_names(NULL, 0, cfg->control_ifs.first,
			&rcif, &num_rcif)) {
			return NULL;
		}
		for(i=0; i<num_rcif; i++) {
			if(!add_open(rcif[i], cfg->control_port, &l, 1, cfg)) {
				listening_ports_free(l);
				config_del_strarray(rcif, num_rcif);
				return NULL;
			}
		}
		config_del_strarray(rcif, num_rcif);
	} else {
		/* defaults */
		if(cfg->do_ip6 &&
			!add_open("::1", cfg->control_port, &l, 0, cfg)) {
			listening_ports_free(l);
			return NULL;
		}
		if(cfg->do_ip4 &&
			!add_open("127.0.0.1", cfg->control_port, &l, 1, cfg)) {
			listening_ports_free(l);
			return NULL;
		}
	}
	return l;
}

/** open accept commpoint */
static int
accept_open(struct daemon_remote* rc, int fd)
{
	struct listen_list* n = (struct listen_list*)malloc(sizeof(*n));
	if(!n) {
		log_err("out of memory");
		return 0;
	}
	n->next = rc->accept_list;
	rc->accept_list = n;
	/* open commpt */
	n->com = comm_point_create_raw(rc->worker->base, fd, 0,
		&remote_accept_callback, rc);
	if(!n->com)
		return 0;
	/* keep this port open, its fd is kept in the rc portlist */
	n->com->do_not_close = 1;
	return 1;
}

int daemon_remote_open_accept(struct daemon_remote* rc,
	struct listen_port* ports, struct worker* worker)
{
	struct listen_port* p;
	rc->worker = worker;
	for(p = ports; p; p = p->next) {
		if(!accept_open(rc, p->fd)) {
			log_err("could not create accept comm point");
			return 0;
		}
	}
	return 1;
}

void daemon_remote_stop_accept(struct daemon_remote* rc)
{
	struct listen_list* p;
	for(p=rc->accept_list; p; p=p->next) {
		comm_point_stop_listening(p->com);
	}
}

void daemon_remote_start_accept(struct daemon_remote* rc)
{
	struct listen_list* p;
	for(p=rc->accept_list; p; p=p->next) {
		comm_point_start_listening(p->com, -1, -1);
	}
}

int remote_accept_callback(struct comm_point* c, void* arg, int err,
	struct comm_reply* ATTR_UNUSED(rep))
{
	struct daemon_remote* rc = (struct daemon_remote*)arg;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int newfd;
	struct rc_state* n;
	if(err != NETEVENT_NOERROR) {
		log_err("error %d on remote_accept_callback", err);
		return 0;
	}
	/* perform the accept */
	newfd = comm_point_perform_accept(c, &addr, &addrlen);
	if(newfd == -1)
		return 0;
	/* create new commpoint unless we are servicing already */
	if(rc->active >= rc->max_active) {
		log_warn("drop incoming remote control: too many connections");
	close_exit:
		sock_close(newfd);
		return 0;
	}

	/* setup commpoint to service the remote control command */
	n = (struct rc_state*)calloc(1, sizeof(*n));
	if(!n) {
		log_err("out of memory");
		goto close_exit;
	}
	n->fd = newfd;
	/* start in reading state */
	n->c = comm_point_create_raw(rc->worker->base, newfd, 0,
		&remote_control_callback, n);
	if(!n->c) {
		log_err("out of memory");
		free(n);
		goto close_exit;
	}
	log_addr(VERB_QUERY, "new control connection from", &addr, addrlen);
	n->c->do_not_close = 0;
	comm_point_stop_listening(n->c);
	comm_point_start_listening(n->c, -1, REMOTE_CONTROL_TCP_TIMEOUT);
	memcpy(&n->c->repinfo.remote_addr, &addr, addrlen);
	n->c->repinfo.remote_addrlen = addrlen;
	if(rc->use_cert) {
		n->shake_state = rc_hs_read;
		n->ssl = SSL_new(rc->ctx);
		if(!n->ssl) {
			log_crypto_err("could not SSL_new");
			comm_point_delete(n->c);
			free(n);
			goto close_exit;
		}
		SSL_set_accept_state(n->ssl);
		(void)SSL_set_mode(n->ssl, (long)SSL_MODE_AUTO_RETRY);
		if(!SSL_set_fd(n->ssl, newfd)) {
			log_crypto_err("could not SSL_set_fd");
			SSL_free(n->ssl);
			comm_point_delete(n->c);
			free(n);
			goto close_exit;
		}
	} else {
		n->ssl = NULL;
	}

	n->rc = rc;
	n->next = rc->busy_list;
	rc->busy_list = n;
	rc->active ++;

	/* perform the first nonblocking read already, for windows,
	 * so it can return wouldblock. could be faster too. */
	(void)remote_control_callback(n->c, n, NETEVENT_NOERROR, NULL);
	return 0;
}

/** delete from list */
static void
state_list_remove_elem(struct rc_state** list, struct comm_point* c)
{
	while(*list) {
		if( (*list)->c == c) {
			*list = (*list)->next;
			return;
		}
		list = &(*list)->next;
	}
}

/** decrease active count and remove commpoint from busy list */
static void
clean_point(struct daemon_remote* rc, struct rc_state* s)
{
	if(!s->rc) {
		/* the state has been picked up and moved away */
		free(s);
		return;
	}
	state_list_remove_elem(&rc->busy_list, s->c);
	rc->active --;
	if(s->ssl) {
		SSL_shutdown(s->ssl);
		SSL_free(s->ssl);
	}
	comm_point_delete(s->c);
	free(s);
}

int
ssl_print_text(RES* res, const char* text)
{
	int r;
	if(!res)
		return 0;
	if(res->ssl) {
		ERR_clear_error();
		if((r=SSL_write(res->ssl, text, (int)strlen(text))) <= 0) {
			int r2;
			if((r2=SSL_get_error(res->ssl, r)) == SSL_ERROR_ZERO_RETURN) {
				verbose(VERB_QUERY, "warning, in SSL_write, peer "
					"closed connection");
				return 0;
			}
			log_crypto_err_io("could not SSL_write", r2);
			return 0;
		}
	} else {
		size_t at = 0;
		while(at < strlen(text)) {
			ssize_t r = send(res->fd, text+at, strlen(text)-at, 0);
			if(r == -1) {
				if(errno == EAGAIN || errno == EINTR)
					continue;
				log_err("could not send: %s",
					sock_strerror(errno));
				return 0;
			}
			at += r;
		}
	}
	return 1;
}

/** print text over the ssl connection */
static int
ssl_print_vmsg(RES* ssl, const char* format, va_list args)
{
	char msg[65535];
	vsnprintf(msg, sizeof(msg), format, args);
	return ssl_print_text(ssl, msg);
}

/** printf style printing to the ssl connection */
int ssl_printf(RES* ssl, const char* format, ...)
{
	va_list args;
	int ret;
	va_start(args, format);
	ret = ssl_print_vmsg(ssl, format, args);
	va_end(args);
	return ret;
}

int
ssl_read_line(RES* res, char* buf, size_t max)
{
	int r;
	size_t len = 0;
	if(!res)
		return 0;
	while(len < max) {
		if(res->ssl) {
			ERR_clear_error();
			if((r=SSL_read(res->ssl, buf+len, 1)) <= 0) {
				int r2;
				if((r2=SSL_get_error(res->ssl, r)) == SSL_ERROR_ZERO_RETURN) {
					buf[len] = 0;
					return 1;
				}
				log_crypto_err_io("could not SSL_read", r2);
				return 0;
			}
		} else {
			while(1) {
				ssize_t rr = recv(res->fd, buf+len, 1, 0);
				if(rr <= 0) {
					if(rr == 0) {
						buf[len] = 0;
						return 1;
					}
					if(errno == EINTR || errno == EAGAIN)
						continue;
					if(rr < 0) log_err("could not recv: %s",
						sock_strerror(errno));
					return 0;
				}
				break;
			}
		}
		if(buf[len] == '\n') {
			/* return string without \n */
			buf[len] = 0;
			return 1;
		}
		len++;
	}
	buf[max-1] = 0;
	log_err("control line too long (%d): %s", (int)max, buf);
	return 0;
}

/** skip whitespace, return new pointer into string */
static char*
skipwhite(char* str)
{
	/* EOS \0 is not a space */
	while( isspace((unsigned char)*str) )
		str++;
	return str;
}

/** send the OK to the control client */
static void send_ok(RES* ssl)
{
	(void)ssl_printf(ssl, "ok\n");
}

/** do the stop command */
static void
do_stop(RES* ssl, struct worker* worker)
{
	worker->need_to_exit = 1;
	comm_base_exit(worker->base);
	send_ok(ssl);
}

/** do the reload command */
static void
do_reload(RES* ssl, struct worker* worker, int reuse_cache)
{
	worker->reuse_cache = reuse_cache;
	worker->need_to_exit = 0;
	comm_base_exit(worker->base);
	send_ok(ssl);
}

#ifndef THREADS_DISABLED
/** parse fast reload command options. */
static int
fr_parse_options(RES* ssl, char* arg, int* fr_verb, int* fr_nopause,
	int* fr_drop_mesh)
{
	char* argp = arg;
	while(*argp=='+') {
		argp++;
		while(*argp!=0 && *argp!=' ' && *argp!='\t') {
			if(*argp == 'v') {
				(*fr_verb)++;
			} else if(*argp == 'p') {
				(*fr_nopause) = 1;
			} else if(*argp == 'd') {
				(*fr_drop_mesh) = 1;
			} else {
				if(!ssl_printf(ssl,
					"error: unknown option '+%c'\n",
					*argp))
					return 0;
				return 0;
			}
			argp++;
		}
		argp = skipwhite(argp);
	}
	if(*argp!=0) {
		if(!ssl_printf(ssl, "error: unknown option '%s'\n", argp))
			return 0;
		return 0;
	}
	return 1;
}
#endif /* !THREADS_DISABLED */

/** do the fast_reload command */
static void
do_fast_reload(RES* ssl, struct worker* worker, struct rc_state* s, char* arg)
{
#ifdef THREADS_DISABLED
	if(!ssl_printf(ssl, "error: no threads for fast_reload, compiled without threads.\n"))
		return;
	(void)worker;
	(void)s;
	(void)arg;
#else
	int fr_verb = 0, fr_nopause = 0, fr_drop_mesh = 0;
	if(!fr_parse_options(ssl, arg, &fr_verb, &fr_nopause, &fr_drop_mesh))
		return;
	if(fr_verb >= 1) {
		if(!ssl_printf(ssl, "start fast_reload\n"))
			return;
	}
	fast_reload_thread_start(ssl, worker, s, fr_verb, fr_nopause,
		fr_drop_mesh);
#endif
}

/** do the verbosity command */
static void
do_verbosity(RES* ssl, char* str)
{
	int val = atoi(str);
	if(val == 0 && strcmp(str, "0") != 0) {
		ssl_printf(ssl, "error in verbosity number syntax: %s\n", str);
		return;
	}
	verbosity = val;
	send_ok(ssl);
}

/** print stats from statinfo */
static int
print_stats(RES* ssl, const char* nm, struct ub_stats_info* s)
{
	struct timeval sumwait, avg;
	if(!ssl_printf(ssl, "%s.num.queries"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries)) return 0;
	if(!ssl_printf(ssl, "%s.num.queries_ip_ratelimited"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries_ip_ratelimited)) return 0;
	if(!ssl_printf(ssl, "%s.num.queries_cookie_valid"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries_cookie_valid)) return 0;
	if(!ssl_printf(ssl, "%s.num.queries_cookie_client"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries_cookie_client)) return 0;
	if(!ssl_printf(ssl, "%s.num.queries_cookie_invalid"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries_cookie_invalid)) return 0;
	if(!ssl_printf(ssl, "%s.num.cachehits"SQ"%lu\n", nm,
		(unsigned long)(s->svr.num_queries
			- s->svr.num_queries_missed_cache))) return 0;
	if(!ssl_printf(ssl, "%s.num.cachemiss"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries_missed_cache)) return 0;
	if(!ssl_printf(ssl, "%s.num.prefetch"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries_prefetch)) return 0;
	if(!ssl_printf(ssl, "%s.num.queries_timed_out"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_queries_timed_out)) return 0;
	if(!ssl_printf(ssl, "%s.query.queue_time_us.max"SQ"%lu\n", nm,
		(unsigned long)s->svr.max_query_time_us)) return 0;
	if(!ssl_printf(ssl, "%s.num.expired"SQ"%lu\n", nm,
		(unsigned long)s->svr.ans_expired)) return 0;
	if(!ssl_printf(ssl, "%s.num.recursivereplies"SQ"%lu\n", nm,
		(unsigned long)s->mesh_replies_sent)) return 0;
#ifdef USE_DNSCRYPT
	if(!ssl_printf(ssl, "%s.num.dnscrypt.crypted"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_query_dnscrypt_crypted)) return 0;
	if(!ssl_printf(ssl, "%s.num.dnscrypt.cert"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_query_dnscrypt_cert)) return 0;
	if(!ssl_printf(ssl, "%s.num.dnscrypt.cleartext"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_query_dnscrypt_cleartext)) return 0;
	if(!ssl_printf(ssl, "%s.num.dnscrypt.malformed"SQ"%lu\n", nm,
		(unsigned long)s->svr.num_query_dnscrypt_crypted_malformed)) return 0;
#endif
	if(!ssl_printf(ssl, "%s.requestlist.avg"SQ"%g\n", nm,
		(s->svr.num_queries_missed_cache+s->svr.num_queries_prefetch)?
			(double)s->svr.sum_query_list_size/
			(double)(s->svr.num_queries_missed_cache+
			s->svr.num_queries_prefetch) : 0.0)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.max"SQ"%lu\n", nm,
		(unsigned long)s->svr.max_query_list_size)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.overwritten"SQ"%lu\n", nm,
		(unsigned long)s->mesh_jostled)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.exceeded"SQ"%lu\n", nm,
		(unsigned long)s->mesh_dropped)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.current.all"SQ"%lu\n", nm,
		(unsigned long)s->mesh_num_states)) return 0;
	if(!ssl_printf(ssl, "%s.requestlist.current.user"SQ"%lu\n", nm,
		(unsigned long)s->mesh_num_reply_states)) return 0;
#ifndef S_SPLINT_S
	sumwait.tv_sec = s->mesh_replies_sum_wait_sec;
	sumwait.tv_usec = s->mesh_replies_sum_wait_usec;
#endif
	timeval_divide(&avg, &sumwait, s->mesh_replies_sent);
	if(!ssl_printf(ssl, "%s.recursion.time.avg"SQ ARG_LL "d.%6.6d\n", nm,
		(long long)avg.tv_sec, (int)avg.tv_usec)) return 0;
	if(!ssl_printf(ssl, "%s.recursion.time.median"SQ"%g\n", nm,
		s->mesh_time_median)) return 0;
	if(!ssl_printf(ssl, "%s.tcpusage"SQ"%lu\n", nm,
		(unsigned long)s->svr.tcp_accept_usage)) return 0;
	return 1;
}

/** print stats for one thread */
static int
print_thread_stats(RES* ssl, int i, struct ub_stats_info* s)
{
	char nm[32];
	snprintf(nm, sizeof(nm), "thread%d", i);
	nm[sizeof(nm)-1]=0;
	return print_stats(ssl, nm, s);
}

/** print long number */
static int
print_longnum(RES* ssl, const char* desc, size_t x)
{
	if(x > 1024*1024*1024) {
		/* more than a Gb */
		size_t front = x / (size_t)1000000;
		size_t back = x % (size_t)1000000;
		return ssl_printf(ssl, "%s%u%6.6u\n", desc,
			(unsigned)front, (unsigned)back);
	} else {
		return ssl_printf(ssl, "%s%lu\n", desc, (unsigned long)x);
	}
}

/** print mem stats */
static int
print_mem(RES* ssl, struct worker* worker, struct daemon* daemon,
	struct ub_stats_info* s)
{
	size_t msg, rrset, val, iter, respip;
#ifdef CLIENT_SUBNET
	size_t subnet = 0;
#endif /* CLIENT_SUBNET */
#ifdef USE_IPSECMOD
	size_t ipsecmod = 0;
#endif /* USE_IPSECMOD */
#ifdef USE_DNSCRYPT
	size_t dnscrypt_shared_secret = 0;
	size_t dnscrypt_nonce = 0;
#endif /* USE_DNSCRYPT */
#ifdef WITH_DYNLIBMODULE
    size_t dynlib = 0;
#endif /* WITH_DYNLIBMODULE */
	msg = slabhash_get_mem(daemon->env->msg_cache);
	rrset = slabhash_get_mem(&daemon->env->rrset_cache->table);
	val = mod_get_mem(&worker->env, "validator");
	iter = mod_get_mem(&worker->env, "iterator");
	respip = mod_get_mem(&worker->env, "respip");
#ifdef CLIENT_SUBNET
	subnet = mod_get_mem(&worker->env, "subnetcache");
#endif /* CLIENT_SUBNET */
#ifdef USE_IPSECMOD
	ipsecmod = mod_get_mem(&worker->env, "ipsecmod");
#endif /* USE_IPSECMOD */
#ifdef USE_DNSCRYPT
	if(daemon->dnscenv) {
		dnscrypt_shared_secret = slabhash_get_mem(
			daemon->dnscenv->shared_secrets_cache);
		dnscrypt_nonce = slabhash_get_mem(daemon->dnscenv->nonces_cache);
	}
#endif /* USE_DNSCRYPT */
#ifdef WITH_DYNLIBMODULE
    dynlib = mod_get_mem(&worker->env, "dynlib");
#endif /* WITH_DYNLIBMODULE */

	if(!print_longnum(ssl, "mem.cache.rrset"SQ, rrset))
		return 0;
	if(!print_longnum(ssl, "mem.cache.message"SQ, msg))
		return 0;
	if(!print_longnum(ssl, "mem.mod.iterator"SQ, iter))
		return 0;
	if(!print_longnum(ssl, "mem.mod.validator"SQ, val))
		return 0;
	if(!print_longnum(ssl, "mem.mod.respip"SQ, respip))
		return 0;
#ifdef CLIENT_SUBNET
	if(!print_longnum(ssl, "mem.mod.subnet"SQ, subnet))
		return 0;
#endif /* CLIENT_SUBNET */
#ifdef USE_IPSECMOD
	if(!print_longnum(ssl, "mem.mod.ipsecmod"SQ, ipsecmod))
		return 0;
#endif /* USE_IPSECMOD */
#ifdef USE_DNSCRYPT
	if(!print_longnum(ssl, "mem.cache.dnscrypt_shared_secret"SQ,
			dnscrypt_shared_secret))
		return 0;
	if(!print_longnum(ssl, "mem.cache.dnscrypt_nonce"SQ,
			dnscrypt_nonce))
		return 0;
#endif /* USE_DNSCRYPT */
#ifdef WITH_DYNLIBMODULE
	if(!print_longnum(ssl, "mem.mod.dynlibmod"SQ, dynlib))
		return 0;
#endif /* WITH_DYNLIBMODULE */
	if(!print_longnum(ssl, "mem.streamwait"SQ,
		(size_t)s->svr.mem_stream_wait))
		return 0;
	if(!print_longnum(ssl, "mem.http.query_buffer"SQ,
		(size_t)s->svr.mem_http2_query_buffer))
		return 0;
	if(!print_longnum(ssl, "mem.http.response_buffer"SQ,
		(size_t)s->svr.mem_http2_response_buffer))
		return 0;
	return 1;
}

/** print uptime stats */
static int
print_uptime(RES* ssl, struct worker* worker, int reset)
{
	struct timeval now = *worker->env.now_tv;
	struct timeval up, dt;
	timeval_subtract(&up, &now, &worker->daemon->time_boot);
	timeval_subtract(&dt, &now, &worker->daemon->time_last_stat);
	if(reset)
		worker->daemon->time_last_stat = now;
	if(!ssl_printf(ssl, "time.now"SQ ARG_LL "d.%6.6d\n",
		(long long)now.tv_sec, (unsigned)now.tv_usec)) return 0;
	if(!ssl_printf(ssl, "time.up"SQ ARG_LL "d.%6.6d\n",
		(long long)up.tv_sec, (unsigned)up.tv_usec)) return 0;
	if(!ssl_printf(ssl, "time.elapsed"SQ ARG_LL "d.%6.6d\n",
		(long long)dt.tv_sec, (unsigned)dt.tv_usec)) return 0;
	return 1;
}

/** print extended histogram */
static int
print_hist(RES* ssl, struct ub_stats_info* s)
{
	struct timehist* hist;
	size_t i;
	hist = timehist_setup();
	if(!hist) {
		log_err("out of memory");
		return 0;
	}
	timehist_import(hist, s->svr.hist, NUM_BUCKETS_HIST);
	for(i=0; i<hist->num; i++) {
		if(!ssl_printf(ssl,
			"histogram.%6.6d.%6.6d.to.%6.6d.%6.6d=%lu\n",
			(int)hist->buckets[i].lower.tv_sec,
			(int)hist->buckets[i].lower.tv_usec,
			(int)hist->buckets[i].upper.tv_sec,
			(int)hist->buckets[i].upper.tv_usec,
			(unsigned long)hist->buckets[i].count)) {
			timehist_delete(hist);
			return 0;
		}
	}
	timehist_delete(hist);
	return 1;
}

/** print extended stats */
static int
print_ext(RES* ssl, struct ub_stats_info* s, int inhibit_zero)
{
	int i;
	char nm[32];
	const sldns_rr_descriptor* desc;
	const sldns_lookup_table* lt;
	/* TYPE */
	for(i=0; i<UB_STATS_QTYPE_NUM; i++) {
		if(inhibit_zero && s->svr.qtype[i] == 0)
			continue;
		desc = sldns_rr_descript((uint16_t)i);
		if(desc && desc->_name) {
			snprintf(nm, sizeof(nm), "%s", desc->_name);
		} else if (i == LDNS_RR_TYPE_IXFR) {
			snprintf(nm, sizeof(nm), "IXFR");
		} else if (i == LDNS_RR_TYPE_AXFR) {
			snprintf(nm, sizeof(nm), "AXFR");
		} else if (i == LDNS_RR_TYPE_MAILA) {
			snprintf(nm, sizeof(nm), "MAILA");
		} else if (i == LDNS_RR_TYPE_MAILB) {
			snprintf(nm, sizeof(nm), "MAILB");
		} else if (i == LDNS_RR_TYPE_ANY) {
			snprintf(nm, sizeof(nm), "ANY");
		} else {
			snprintf(nm, sizeof(nm), "TYPE%d", i);
		}
		if(!ssl_printf(ssl, "num.query.type.%s"SQ"%lu\n",
			nm, (unsigned long)s->svr.qtype[i])) return 0;
	}
	if(!inhibit_zero || s->svr.qtype_big) {
		if(!ssl_printf(ssl, "num.query.type.other"SQ"%lu\n",
			(unsigned long)s->svr.qtype_big)) return 0;
	}
	/* CLASS */
	for(i=0; i<UB_STATS_QCLASS_NUM; i++) {
		if(inhibit_zero && s->svr.qclass[i] == 0)
			continue;
		lt = sldns_lookup_by_id(sldns_rr_classes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "CLASS%d", i);
		}
		if(!ssl_printf(ssl, "num.query.class.%s"SQ"%lu\n",
			nm, (unsigned long)s->svr.qclass[i])) return 0;
	}
	if(!inhibit_zero || s->svr.qclass_big) {
		if(!ssl_printf(ssl, "num.query.class.other"SQ"%lu\n",
			(unsigned long)s->svr.qclass_big)) return 0;
	}
	/* OPCODE */
	for(i=0; i<UB_STATS_OPCODE_NUM; i++) {
		if(inhibit_zero && s->svr.qopcode[i] == 0)
			continue;
		lt = sldns_lookup_by_id(sldns_opcodes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "OPCODE%d", i);
		}
		if(!ssl_printf(ssl, "num.query.opcode.%s"SQ"%lu\n",
			nm, (unsigned long)s->svr.qopcode[i])) return 0;
	}
	/* transport */
	if(!ssl_printf(ssl, "num.query.tcp"SQ"%lu\n",
		(unsigned long)s->svr.qtcp)) return 0;
	if(!ssl_printf(ssl, "num.query.tcpout"SQ"%lu\n",
		(unsigned long)s->svr.qtcp_outgoing)) return 0;
	if(!ssl_printf(ssl, "num.query.udpout"SQ"%lu\n",
		(unsigned long)s->svr.qudp_outgoing)) return 0;
	if(!ssl_printf(ssl, "num.query.tls"SQ"%lu\n",
		(unsigned long)s->svr.qtls)) return 0;
	if(!ssl_printf(ssl, "num.query.tls.resume"SQ"%lu\n",
		(unsigned long)s->svr.qtls_resume)) return 0;
	if(!ssl_printf(ssl, "num.query.ipv6"SQ"%lu\n",
		(unsigned long)s->svr.qipv6)) return 0;
	if(!ssl_printf(ssl, "num.query.https"SQ"%lu\n",
		(unsigned long)s->svr.qhttps)) return 0;
	/* flags */
	if(!ssl_printf(ssl, "num.query.flags.QR"SQ"%lu\n",
		(unsigned long)s->svr.qbit_QR)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.AA"SQ"%lu\n",
		(unsigned long)s->svr.qbit_AA)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.TC"SQ"%lu\n",
		(unsigned long)s->svr.qbit_TC)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.RD"SQ"%lu\n",
		(unsigned long)s->svr.qbit_RD)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.RA"SQ"%lu\n",
		(unsigned long)s->svr.qbit_RA)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.Z"SQ"%lu\n",
		(unsigned long)s->svr.qbit_Z)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.AD"SQ"%lu\n",
		(unsigned long)s->svr.qbit_AD)) return 0;
	if(!ssl_printf(ssl, "num.query.flags.CD"SQ"%lu\n",
		(unsigned long)s->svr.qbit_CD)) return 0;
	if(!ssl_printf(ssl, "num.query.edns.present"SQ"%lu\n",
		(unsigned long)s->svr.qEDNS)) return 0;
	if(!ssl_printf(ssl, "num.query.edns.DO"SQ"%lu\n",
		(unsigned long)s->svr.qEDNS_DO)) return 0;

	/* RCODE */
	for(i=0; i<UB_STATS_RCODE_NUM; i++) {
		/* Always include RCODEs 0-5 */
		if(inhibit_zero && i > LDNS_RCODE_REFUSED && s->svr.ans_rcode[i] == 0)
			continue;
		lt = sldns_lookup_by_id(sldns_rcodes, i);
		if(lt && lt->name) {
			snprintf(nm, sizeof(nm), "%s", lt->name);
		} else {
			snprintf(nm, sizeof(nm), "RCODE%d", i);
		}
		if(!ssl_printf(ssl, "num.answer.rcode.%s"SQ"%lu\n",
			nm, (unsigned long)s->svr.ans_rcode[i])) return 0;
	}
	if(!inhibit_zero || s->svr.ans_rcode_nodata) {
		if(!ssl_printf(ssl, "num.answer.rcode.nodata"SQ"%lu\n",
			(unsigned long)s->svr.ans_rcode_nodata)) return 0;
	}
	/* iteration */
	if(!ssl_printf(ssl, "num.query.ratelimited"SQ"%lu\n",
		(unsigned long)s->svr.queries_ratelimited)) return 0;
	/* validation */
	if(!ssl_printf(ssl, "num.answer.secure"SQ"%lu\n",
		(unsigned long)s->svr.ans_secure)) return 0;
	if(!ssl_printf(ssl, "num.answer.bogus"SQ"%lu\n",
		(unsigned long)s->svr.ans_bogus)) return 0;
	if(!ssl_printf(ssl, "num.rrset.bogus"SQ"%lu\n",
		(unsigned long)s->svr.rrset_bogus)) return 0;
	if(!ssl_printf(ssl, "num.query.aggressive.NOERROR"SQ"%lu\n",
		(unsigned long)s->svr.num_neg_cache_noerror)) return 0;
	if(!ssl_printf(ssl, "num.query.aggressive.NXDOMAIN"SQ"%lu\n",
		(unsigned long)s->svr.num_neg_cache_nxdomain)) return 0;
	/* threat detection */
	if(!ssl_printf(ssl, "unwanted.queries"SQ"%lu\n",
		(unsigned long)s->svr.unwanted_queries)) return 0;
	if(!ssl_printf(ssl, "unwanted.replies"SQ"%lu\n",
		(unsigned long)s->svr.unwanted_replies)) return 0;
	/* cache counts */
	if(!ssl_printf(ssl, "msg.cache.count"SQ"%u\n",
		(unsigned)s->svr.msg_cache_count)) return 0;
	if(!ssl_printf(ssl, "rrset.cache.count"SQ"%u\n",
		(unsigned)s->svr.rrset_cache_count)) return 0;
	if(!ssl_printf(ssl, "infra.cache.count"SQ"%u\n",
		(unsigned)s->svr.infra_cache_count)) return 0;
	if(!ssl_printf(ssl, "key.cache.count"SQ"%u\n",
		(unsigned)s->svr.key_cache_count)) return 0;
	/* max collisions */
	if(!ssl_printf(ssl, "msg.cache.max_collisions"SQ"%u\n",
		(unsigned)s->svr.msg_cache_max_collisions)) return 0;
	if(!ssl_printf(ssl, "rrset.cache.max_collisions"SQ"%u\n",
		(unsigned)s->svr.rrset_cache_max_collisions)) return 0;
	/* applied RPZ actions */
	for(i=0; i<UB_STATS_RPZ_ACTION_NUM; i++) {
		if(i == RPZ_NO_OVERRIDE_ACTION)
			continue;
		if(inhibit_zero && s->svr.rpz_action[i] == 0)
			continue;
		if(!ssl_printf(ssl, "num.rpz.action.%s"SQ"%lu\n",
			rpz_action_to_string(i),
			(unsigned long)s->svr.rpz_action[i])) return 0;
	}
#ifdef USE_DNSCRYPT
	if(!ssl_printf(ssl, "dnscrypt_shared_secret.cache.count"SQ"%u\n",
		(unsigned)s->svr.shared_secret_cache_count)) return 0;
	if(!ssl_printf(ssl, "dnscrypt_nonce.cache.count"SQ"%u\n",
		(unsigned)s->svr.nonce_cache_count)) return 0;
	if(!ssl_printf(ssl, "num.query.dnscrypt.shared_secret.cachemiss"SQ"%lu\n",
		(unsigned long)s->svr.num_query_dnscrypt_secret_missed_cache)) return 0;
	if(!ssl_printf(ssl, "num.query.dnscrypt.replay"SQ"%lu\n",
		(unsigned long)s->svr.num_query_dnscrypt_replay)) return 0;
#endif /* USE_DNSCRYPT */
	if(!ssl_printf(ssl, "num.query.authzone.up"SQ"%lu\n",
		(unsigned long)s->svr.num_query_authzone_up)) return 0;
	if(!ssl_printf(ssl, "num.query.authzone.down"SQ"%lu\n",
		(unsigned long)s->svr.num_query_authzone_down)) return 0;
#ifdef CLIENT_SUBNET
	if(!ssl_printf(ssl, "num.query.subnet"SQ"%lu\n",
		(unsigned long)s->svr.num_query_subnet)) return 0;
	if(!ssl_printf(ssl, "num.query.subnet_cache"SQ"%lu\n",
		(unsigned long)s->svr.num_query_subnet_cache)) return 0;
#endif /* CLIENT_SUBNET */
#ifdef USE_CACHEDB
	if(!ssl_printf(ssl, "num.query.cachedb"SQ"%lu\n",
		(unsigned long)s->svr.num_query_cachedb)) return 0;
#endif /* USE_CACHEDB */
	return 1;
}

/** do the stats command */
static void
do_stats(RES* ssl, struct worker* worker, int reset)
{
	struct daemon* daemon = worker->daemon;
	struct ub_stats_info total;
	struct ub_stats_info s;
	int i;
	memset(&total, 0, sizeof(total));
	log_assert(daemon->num > 0);
	/* gather all thread statistics in one place */
	for(i=0; i<daemon->num; i++) {
		server_stats_obtain(worker, daemon->workers[i], &s, reset);
		if(!print_thread_stats(ssl, i, &s))
			return;
		if(i == 0)
			total = s;
		else	server_stats_add(&total, &s);
	}
	/* print the thread statistics */
	total.mesh_time_median /= (double)daemon->num;
	if(!print_stats(ssl, "total", &total))
		return;
	if(!print_uptime(ssl, worker, reset))
		return;
	if(daemon->cfg->stat_extended) {
		if(!print_mem(ssl, worker, daemon, &total))
			return;
		if(!print_hist(ssl, &total))
			return;
		if(!print_ext(ssl, &total, daemon->cfg->stat_inhibit_zero))
			return;
	}
}

/** parse commandline argument domain name */
static int
parse_arg_name(RES* ssl, char* str, uint8_t** res, size_t* len, int* labs)
{
	uint8_t nm[LDNS_MAX_DOMAINLEN+1];
	size_t nmlen = sizeof(nm);
	int status;
	*res = NULL;
	*len = 0;
	*labs = 0;
	if(str[0] == '\0') {
		ssl_printf(ssl, "error: this option requires a domain name\n");
		return 0;
	}
	status = sldns_str2wire_dname_buf(str, nm, &nmlen);
	if(status != 0) {
		ssl_printf(ssl, "error cannot parse name %s at %d: %s\n", str,
			LDNS_WIREPARSE_OFFSET(status),
			sldns_get_errorstr_parse(status));
		return 0;
	}
	*res = memdup(nm, nmlen);
	if(!*res) {
		ssl_printf(ssl, "error out of memory\n");
		return 0;
	}
	*labs = dname_count_size_labels(*res, len);
	return 1;
}

/** find second argument, modifies string */
static int
find_arg2(RES* ssl, char* arg, char** arg2)
{
	char* as = strchr(arg, ' ');
	char* at = strchr(arg, '\t');
	if(as && at) {
		if(at < as)
			as = at;
		as[0]=0;
		*arg2 = skipwhite(as+1);
	} else if(as) {
		as[0]=0;
		*arg2 = skipwhite(as+1);
	} else if(at) {
		at[0]=0;
		*arg2 = skipwhite(at+1);
	} else {
		ssl_printf(ssl, "error could not find next argument "
			"after %s\n", arg);
		return 0;
	}
	return 1;
}

/** Add a new zone */
static int
perform_zone_add(RES* ssl, struct local_zones* zones, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	char* arg2;
	enum localzone_type t;
	struct local_zone* z;
	if(!find_arg2(ssl, arg, &arg2))
		return 0;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return 0;
	if(!local_zone_str2type(arg2, &t)) {
		ssl_printf(ssl, "error not a zone type. %s\n", arg2);
		free(nm);
		return 0;
	}
	lock_rw_wrlock(&zones->lock);
	if((z=local_zones_find(zones, nm, nmlen,
		nmlabs, LDNS_RR_CLASS_IN))) {
		/* already present in tree */
		lock_rw_wrlock(&z->lock);
		z->type = t; /* update type anyway */
		lock_rw_unlock(&z->lock);
		free(nm);
		lock_rw_unlock(&zones->lock);
		return 1;
	}
	if(!local_zones_add_zone(zones, nm, nmlen,
		nmlabs, LDNS_RR_CLASS_IN, t)) {
		lock_rw_unlock(&zones->lock);
		ssl_printf(ssl, "error out of memory\n");
		return 0;
	}
	lock_rw_unlock(&zones->lock);
	return 1;
}

/** Do the local_zone command */
static void
do_zone_add(RES* ssl, struct local_zones* zones, char* arg)
{
	if(!perform_zone_add(ssl, zones, arg))
		return;
	send_ok(ssl);
}

/** Do the local_zones command */
static void
do_zones_add(RES* ssl, struct local_zones* zones)
{
	char buf[2048];
	int num = 0;
	while(ssl_read_line(ssl, buf, sizeof(buf))) {
		if(buf[0] == 0 || (buf[0] == 0x04 && buf[1] == 0))
			break; /* zero byte line or end of transmission */
		if(!perform_zone_add(ssl, zones, buf)) {
			if(!ssl_printf(ssl, "error for input line: %s\n", buf))
				return;
		}
		else
			num++;
	}
	(void)ssl_printf(ssl, "added %d zones\n", num);
}

/** Remove a zone */
static int
perform_zone_remove(RES* ssl, struct local_zones* zones, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	struct local_zone* z;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return 0;
	lock_rw_wrlock(&zones->lock);
	if((z=local_zones_find(zones, nm, nmlen,
		nmlabs, LDNS_RR_CLASS_IN))) {
		/* present in tree */
		local_zones_del_zone(zones, z);
	}
	lock_rw_unlock(&zones->lock);
	free(nm);
	return 1;
}

/** Do the local_zone_remove command */
static void
do_zone_remove(RES* ssl, struct local_zones* zones, char* arg)
{
	if(!perform_zone_remove(ssl, zones, arg))
		return;
	send_ok(ssl);
}

/** Do the local_zones_remove command */
static void
do_zones_remove(RES* ssl, struct local_zones* zones)
{
	char buf[2048];
	int num = 0;
	while(ssl_read_line(ssl, buf, sizeof(buf))) {
		if(buf[0] == 0 || (buf[0] == 0x04 && buf[1] == 0))
			break; /* zero byte line or end of transmission */
		if(!perform_zone_remove(ssl, zones, buf)) {
			if(!ssl_printf(ssl, "error for input line: %s\n", buf))
				return;
		}
		else
			num++;
	}
	(void)ssl_printf(ssl, "removed %d zones\n", num);
}

/** check syntax of newly added RR */
static int
check_RR_syntax(RES* ssl, char* str, int line)
{
	uint8_t rr[LDNS_RR_BUF_SIZE];
	size_t len = sizeof(rr), dname_len = 0;
	int s = sldns_str2wire_rr_buf(str, rr, &len, &dname_len, 3600,
		NULL, 0, NULL, 0);
	if(s != 0) {
		char linestr[32];
		if(line == 0)
			linestr[0]=0;
		else 	snprintf(linestr, sizeof(linestr), "line %d ", line);
		if(!ssl_printf(ssl, "error parsing local-data at %sposition %d '%s': %s\n",
			linestr, LDNS_WIREPARSE_OFFSET(s), str,
			sldns_get_errorstr_parse(s)))
			return 0;
		return 0;
	}
	return 1;
}

/** Add new RR data */
static int
perform_data_add(RES* ssl, struct local_zones* zones, char* arg, int line)
{
	if(!check_RR_syntax(ssl, arg, line)) {
		return 0;
	}
	if(!local_zones_add_RR(zones, arg)) {
		ssl_printf(ssl,"error in syntax or out of memory, %s\n", arg);
		return 0;
	}
	return 1;
}

/** Do the local_data command */
static void
do_data_add(RES* ssl, struct local_zones* zones, char* arg)
{
	if(!perform_data_add(ssl, zones, arg, 0))
		return;
	send_ok(ssl);
}

/** Do the local_datas command */
static void
do_datas_add(RES* ssl, struct local_zones* zones)
{
	char buf[2048];
	int num = 0, line = 0;
	while(ssl_read_line(ssl, buf, sizeof(buf))) {
		if(buf[0] == 0 || (buf[0] == 0x04 && buf[1] == 0))
			break; /* zero byte line or end of transmission */
		line++;
		if(perform_data_add(ssl, zones, buf, line))
			num++;
	}
	(void)ssl_printf(ssl, "added %d datas\n", num);
}

/** Remove RR data */
static int
perform_data_remove(RES* ssl, struct local_zones* zones, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return 0;
	local_zones_del_data(zones, nm,
		nmlen, nmlabs, LDNS_RR_CLASS_IN);
	free(nm);
	return 1;
}

/** Do the local_data_remove command */
static void
do_data_remove(RES* ssl, struct local_zones* zones, char* arg)
{
	if(!perform_data_remove(ssl, zones, arg))
		return;
	send_ok(ssl);
}

/** Do the local_datas_remove command */
static void
do_datas_remove(RES* ssl, struct local_zones* zones)
{
	char buf[2048];
	int num = 0;
	while(ssl_read_line(ssl, buf, sizeof(buf))) {
		if(buf[0] == 0 || (buf[0] == 0x04 && buf[1] == 0))
			break; /* zero byte line or end of transmission */
		if(!perform_data_remove(ssl, zones, buf)) {
			if(!ssl_printf(ssl, "error for input line: %s\n", buf))
				return;
		}
		else
			num++;
	}
	(void)ssl_printf(ssl, "removed %d datas\n", num);
}

/** Add a new zone to view */
static void
do_view_zone_add(RES* ssl, struct worker* worker, char* arg)
{
	char* arg2;
	struct view* v;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	v = views_find_view(worker->daemon->views,
		arg, 1 /* get write lock*/);
	if(!v) {
		ssl_printf(ssl,"no view with name: %s\n", arg);
		return;
	}
	if(!v->local_zones) {
		if(!(v->local_zones = local_zones_create())){
			lock_rw_unlock(&v->lock);
			ssl_printf(ssl,"error out of memory\n");
			return;
		}
		if(!v->isfirst) {
			/* Global local-zone is not used for this view,
			 * therefore add defaults to this view-specic
			 * local-zone. */
			struct config_file lz_cfg;
			memset(&lz_cfg, 0, sizeof(lz_cfg));
			local_zone_enter_defaults(v->local_zones, &lz_cfg);
		}
	}
	do_zone_add(ssl, v->local_zones, arg2);
	lock_rw_unlock(&v->lock);
}

/** Remove a zone from view */
static void
do_view_zone_remove(RES* ssl, struct worker* worker, char* arg)
{
	char* arg2;
	struct view* v;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	v = views_find_view(worker->daemon->views,
		arg, 1 /* get write lock*/);
	if(!v) {
		ssl_printf(ssl,"no view with name: %s\n", arg);
		return;
	}
	if(!v->local_zones) {
		lock_rw_unlock(&v->lock);
		send_ok(ssl);
		return;
	}
	do_zone_remove(ssl, v->local_zones, arg2);
	lock_rw_unlock(&v->lock);
}

/** Add new RR data to view */
static void
do_view_data_add(RES* ssl, struct worker* worker, char* arg)
{
	char* arg2;
	struct view* v;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	v = views_find_view(worker->daemon->views,
		arg, 1 /* get write lock*/);
	if(!v) {
		ssl_printf(ssl,"no view with name: %s\n", arg);
		return;
	}
	if(!v->local_zones) {
		if(!(v->local_zones = local_zones_create())){
			lock_rw_unlock(&v->lock);
			ssl_printf(ssl,"error out of memory\n");
			return;
		}
	}
	do_data_add(ssl, v->local_zones, arg2);
	lock_rw_unlock(&v->lock);
}

/** Add new RR data from stdin to view */
static void
do_view_datas_add(RES* ssl, struct worker* worker, char* arg)
{
	struct view* v;
	v = views_find_view(worker->daemon->views,
		arg, 1 /* get write lock*/);
	if(!v) {
		ssl_printf(ssl,"no view with name: %s\n", arg);
		return;
	}
	if(!v->local_zones) {
		if(!(v->local_zones = local_zones_create())){
			lock_rw_unlock(&v->lock);
			ssl_printf(ssl,"error out of memory\n");
			return;
		}
	}
	do_datas_add(ssl, v->local_zones);
	lock_rw_unlock(&v->lock);
}

/** Remove RR data from view */
static void
do_view_data_remove(RES* ssl, struct worker* worker, char* arg)
{
	char* arg2;
	struct view* v;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	v = views_find_view(worker->daemon->views,
		arg, 1 /* get write lock*/);
	if(!v) {
		ssl_printf(ssl,"no view with name: %s\n", arg);
		return;
	}
	if(!v->local_zones) {
		lock_rw_unlock(&v->lock);
		send_ok(ssl);
		return;
	}
	do_data_remove(ssl, v->local_zones, arg2);
	lock_rw_unlock(&v->lock);
}

/** Remove RR data from stdin from view */
static void
do_view_datas_remove(RES* ssl, struct worker* worker, char* arg)
{
	struct view* v;
	v = views_find_view(worker->daemon->views,
		arg, 1 /* get write lock*/);
	if(!v) {
		ssl_printf(ssl,"no view with name: %s\n", arg);
		return;
	}
	if(!v->local_zones){
		lock_rw_unlock(&v->lock);
		ssl_printf(ssl, "removed 0 datas\n");
		return;
	}

	do_datas_remove(ssl, v->local_zones);
	lock_rw_unlock(&v->lock);
}

/** cache lookup of nameservers */
static void
do_lookup(RES* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	(void)print_deleg_lookup(ssl, worker, nm, nmlen, nmlabs);
	free(nm);
}

/** flush something from rrset and msg caches */
static void
do_cache_remove(struct worker* worker, uint8_t* nm, size_t nmlen,
	uint16_t t, uint16_t c)
{
	hashvalue_type h;
	struct query_info k;
	rrset_cache_remove(worker->env.rrset_cache, nm, nmlen, t, c, 0);
	if(t == LDNS_RR_TYPE_SOA)
		rrset_cache_remove(worker->env.rrset_cache, nm, nmlen, t, c,
			PACKED_RRSET_SOA_NEG);
	k.qname = nm;
	k.qname_len = nmlen;
	k.qtype = t;
	k.qclass = c;
	k.local_alias = NULL;
	h = query_info_hash(&k, 0);
	slabhash_remove(worker->env.msg_cache, h, &k);
	if(t == LDNS_RR_TYPE_AAAA) {
		/* for AAAA also flush dns64 bit_cd packet */
		h = query_info_hash(&k, BIT_CD);
		slabhash_remove(worker->env.msg_cache, h, &k);
	}
}

/** flush a type */
static void
do_flush_type(RES* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	char* arg2;
	uint16_t t;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	t = sldns_get_rr_type_by_name(arg2);
	if(t == 0 && strcmp(arg2, "TYPE0") != 0) {
		return;
	}
	do_cache_remove(worker, nm, nmlen, t, LDNS_RR_CLASS_IN);

	free(nm);
	send_ok(ssl);
}

/** flush statistics */
static void
do_flush_stats(RES* ssl, struct worker* worker)
{
	worker_stats_clear(worker);
	send_ok(ssl);
}

/**
 * Local info for deletion functions
 */
struct del_info {
	/** worker */
	struct worker* worker;
	/** name to delete */
	uint8_t* name;
	/** length */
	size_t len;
	/** labels */
	int labs;
	/** time to invalidate to */
	time_t expired;
	/** number of rrsets removed */
	size_t num_rrsets;
	/** number of msgs removed */
	size_t num_msgs;
	/** number of key entries removed */
	size_t num_keys;
	/** length of addr */
	socklen_t addrlen;
	/** socket address for host deletion */
	struct sockaddr_storage addr;
};

/** callback to delete hosts in infra cache */
static void
infra_del_host(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct infra_key* k = (struct infra_key*)e->key;
	if(sockaddr_cmp(&inf->addr, inf->addrlen, &k->addr, k->addrlen) == 0) {
		struct infra_data* d = (struct infra_data*)e->data;
		d->probedelay = 0;
		d->timeout_A = 0;
		d->timeout_AAAA = 0;
		d->timeout_other = 0;
		rtt_init(&d->rtt);
		if(d->ttl > inf->expired) {
			d->ttl = inf->expired;
			inf->num_keys++;
		}
	}
}

/** flush infra cache */
static void
do_flush_infra(RES* ssl, struct worker* worker, char* arg)
{
	struct sockaddr_storage addr;
	socklen_t len;
	struct del_info inf;
	if(strcmp(arg, "all") == 0) {
		slabhash_clear(worker->env.infra_cache->hosts);
		send_ok(ssl);
		return;
	}
	if(!ipstrtoaddr(arg, UNBOUND_DNS_PORT, &addr, &len)) {
		(void)ssl_printf(ssl, "error parsing ip addr: '%s'\n", arg);
		return;
	}
	/* delete all entries from cache */
	/* what we do is to set them all expired */
	inf.worker = worker;
	inf.name = 0;
	inf.len = 0;
	inf.labs = 0;
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;
	inf.addrlen = len;
	memmove(&inf.addr, &addr, len);
	slabhash_traverse(worker->env.infra_cache->hosts, 1, &infra_del_host,
		&inf);
	send_ok(ssl);
}

/** flush requestlist */
static void
do_flush_requestlist(RES* ssl, struct worker* worker)
{
	mesh_delete_all(worker->env.mesh);
	send_ok(ssl);
}

/** callback to delete rrsets in a zone */
static void
zone_del_rrset(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct ub_packed_rrset_key* k = (struct ub_packed_rrset_key*)e->key;
	if(dname_subdomain_c(k->rk.dname, inf->name)) {
		struct packed_rrset_data* d =
			(struct packed_rrset_data*)e->data;
		if(d->ttl > inf->expired) {
			d->ttl = inf->expired;
			inf->num_rrsets++;
		}
	}
}

/** callback to delete messages in a zone */
static void
zone_del_msg(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct msgreply_entry* k = (struct msgreply_entry*)e->key;
	if(dname_subdomain_c(k->key.qname, inf->name)) {
		struct reply_info* d = (struct reply_info*)e->data;
		if(d->ttl > inf->expired) {
			d->ttl = inf->expired;
			d->prefetch_ttl = inf->expired;
			d->serve_expired_ttl = inf->expired;
			inf->num_msgs++;
		}
	}
}

/** callback to delete keys in zone */
static void
zone_del_kcache(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct key_entry_key* k = (struct key_entry_key*)e->key;
	if(dname_subdomain_c(k->name, inf->name)) {
		struct key_entry_data* d = (struct key_entry_data*)e->data;
		if(d->ttl > inf->expired) {
			d->ttl = inf->expired;
			inf->num_keys++;
		}
	}
}

/** remove all rrsets and keys from zone from cache */
static void
do_flush_zone(RES* ssl, struct worker* worker, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	struct del_info inf;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	/* delete all RRs and key entries from zone */
	/* what we do is to set them all expired */
	inf.worker = worker;
	inf.name = nm;
	inf.len = nmlen;
	inf.labs = nmlabs;
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;
	slabhash_traverse(&worker->env.rrset_cache->table, 1,
		&zone_del_rrset, &inf);

	slabhash_traverse(worker->env.msg_cache, 1, &zone_del_msg, &inf);

	/* and validator cache */
	if(worker->env.key_cache) {
		slabhash_traverse(worker->env.key_cache->slab, 1,
			&zone_del_kcache, &inf);
	}

	free(nm);

	(void)ssl_printf(ssl, "ok removed %lu rrsets, %lu messages "
		"and %lu key entries\n", (unsigned long)inf.num_rrsets,
		(unsigned long)inf.num_msgs, (unsigned long)inf.num_keys);
}

/** callback to delete bogus rrsets */
static void
bogus_del_rrset(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct packed_rrset_data* d = (struct packed_rrset_data*)e->data;
	if(d->security == sec_status_bogus) {
		d->ttl = inf->expired;
		inf->num_rrsets++;
	}
}

/** callback to delete bogus messages */
static void
bogus_del_msg(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct reply_info* d = (struct reply_info*)e->data;
	if(d->security == sec_status_bogus) {
		d->ttl = inf->expired;
		inf->num_msgs++;
	}
}

/** callback to delete bogus keys */
static void
bogus_del_kcache(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct key_entry_data* d = (struct key_entry_data*)e->data;
	if(d->isbad) {
		d->ttl = inf->expired;
		inf->num_keys++;
	}
}

/** remove all bogus rrsets, msgs and keys from cache */
static void
do_flush_bogus(RES* ssl, struct worker* worker)
{
	struct del_info inf;
	/* what we do is to set them all expired */
	inf.worker = worker;
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;
	slabhash_traverse(&worker->env.rrset_cache->table, 1,
		&bogus_del_rrset, &inf);

	slabhash_traverse(worker->env.msg_cache, 1, &bogus_del_msg, &inf);

	/* and validator cache */
	if(worker->env.key_cache) {
		slabhash_traverse(worker->env.key_cache->slab, 1,
			&bogus_del_kcache, &inf);
	}

	(void)ssl_printf(ssl, "ok removed %lu rrsets, %lu messages "
		"and %lu key entries\n", (unsigned long)inf.num_rrsets,
		(unsigned long)inf.num_msgs, (unsigned long)inf.num_keys);
}

/** callback to delete negative and servfail rrsets */
static void
negative_del_rrset(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct ub_packed_rrset_key* k = (struct ub_packed_rrset_key*)e->key;
	struct packed_rrset_data* d = (struct packed_rrset_data*)e->data;
	/* delete the parentside negative cache rrsets,
	 * these are nameserver rrsets that failed lookup, rdata empty */
	if((k->rk.flags & PACKED_RRSET_PARENT_SIDE) && d->count == 1 &&
		d->rrsig_count == 0 && d->rr_len[0] == 0) {
		d->ttl = inf->expired;
		inf->num_rrsets++;
	}
}

/** callback to delete negative and servfail messages */
static void
negative_del_msg(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct reply_info* d = (struct reply_info*)e->data;
	/* rcode not NOERROR: NXDOMAIN, SERVFAIL, ..: an nxdomain or error
	 * or NOERROR rcode with ANCOUNT==0: a NODATA answer */
	if(FLAGS_GET_RCODE(d->flags) != 0 || d->an_numrrsets == 0) {
		d->ttl = inf->expired;
		inf->num_msgs++;
	}
}

/** callback to delete negative key entries */
static void
negative_del_kcache(struct lruhash_entry* e, void* arg)
{
	/* entry is locked */
	struct del_info* inf = (struct del_info*)arg;
	struct key_entry_data* d = (struct key_entry_data*)e->data;
	/* could be bad because of lookup failure on the DS, DNSKEY, which
	 * was nxdomain or servfail, and thus a result of negative lookups */
	if(d->isbad) {
		d->ttl = inf->expired;
		inf->num_keys++;
	}
}

/** remove all negative(NODATA,NXDOMAIN), and servfail messages from cache */
static void
do_flush_negative(RES* ssl, struct worker* worker)
{
	struct del_info inf;
	/* what we do is to set them all expired */
	inf.worker = worker;
	inf.expired = *worker->env.now;
	inf.expired -= 3; /* handle 3 seconds skew between threads */
	inf.num_rrsets = 0;
	inf.num_msgs = 0;
	inf.num_keys = 0;
	slabhash_traverse(&worker->env.rrset_cache->table, 1,
		&negative_del_rrset, &inf);

	slabhash_traverse(worker->env.msg_cache, 1, &negative_del_msg, &inf);

	/* and validator cache */
	if(worker->env.key_cache) {
		slabhash_traverse(worker->env.key_cache->slab, 1,
			&negative_del_kcache, &inf);
	}

	(void)ssl_printf(ssl, "ok removed %lu rrsets, %lu messages "
		"and %lu key entries\n", (unsigned long)inf.num_rrsets,
		(unsigned long)inf.num_msgs, (unsigned long)inf.num_keys);
}

/** remove name rrset from cache */
static void
do_flush_name(RES* ssl, struct worker* w, char* arg)
{
	uint8_t* nm;
	int nmlabs;
	size_t nmlen;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_AAAA, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_NS, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_SOA, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_CNAME, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_DNAME, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_MX, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_PTR, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_SRV, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_NAPTR, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_SVCB, LDNS_RR_CLASS_IN);
	do_cache_remove(w, nm, nmlen, LDNS_RR_TYPE_HTTPS, LDNS_RR_CLASS_IN);

	free(nm);
	send_ok(ssl);
}

/** printout a delegation point info */
static int
ssl_print_name_dp(RES* ssl, const char* str, uint8_t* nm, uint16_t dclass,
	struct delegpt* dp)
{
	char buf[257];
	struct delegpt_ns* ns;
	struct delegpt_addr* a;
	int f = 0;
	if(str) { /* print header for forward, stub */
		char* c = sldns_wire2str_class(dclass);
		dname_str(nm, buf);
		if(!ssl_printf(ssl, "%s %s %s ", buf, (c?c:"CLASS??"), str)) {
			free(c);
			return 0;
		}
		free(c);
	}
	for(ns = dp->nslist; ns; ns = ns->next) {
		dname_str(ns->name, buf);
		if(!ssl_printf(ssl, "%s%s", (f?" ":""), buf))
			return 0;
		f = 1;
	}
	for(a = dp->target_list; a; a = a->next_target) {
		addr_to_str(&a->addr, a->addrlen, buf, sizeof(buf));
		if(!ssl_printf(ssl, "%s%s", (f?" ":""), buf))
			return 0;
		f = 1;
	}
	return ssl_printf(ssl, "\n");
}


/** print root forwards */
static int
print_root_fwds(RES* ssl, struct iter_forwards* fwds, uint8_t* root)
{
	struct delegpt* dp;
	lock_rw_rdlock(&fwds->lock);
	dp = forwards_lookup(fwds, root, LDNS_RR_CLASS_IN);
	if(!dp) {
		lock_rw_unlock(&fwds->lock);
		return ssl_printf(ssl, "off (using root hints)\n");
	}
	/* if dp is returned it must be the root */
	log_assert(query_dname_compare(dp->name, root)==0);
	if(!ssl_print_name_dp(ssl, NULL, root, LDNS_RR_CLASS_IN, dp)) {
		lock_rw_unlock(&fwds->lock);
		return 0;
	}
	lock_rw_unlock(&fwds->lock);
	return 1;
}

/** parse args into delegpt */
static struct delegpt*
parse_delegpt(RES* ssl, char* args, uint8_t* nm)
{
	/* parse args and add in */
	char* p = args;
	char* todo;
	struct delegpt* dp = delegpt_create_mlc(nm);
	struct sockaddr_storage addr;
	socklen_t addrlen;
	char* auth_name;
	if(!dp) {
		(void)ssl_printf(ssl, "error out of memory\n");
		return NULL;
	}
	while(p) {
		todo = p;
		p = strchr(p, ' '); /* find next spot, if any */
		if(p) {
			*p++ = 0;	/* end this spot */
			p = skipwhite(p); /* position at next spot */
		}
		/* parse address */
		if(!authextstrtoaddr(todo, &addr, &addrlen, &auth_name)) {
			uint8_t* dname= NULL;
			int port;
			dname = authextstrtodname(todo, &port, &auth_name);
			if(!dname) {
				(void)ssl_printf(ssl, "error cannot parse"
					" '%s'\n", todo);
				delegpt_free_mlc(dp);
				return NULL;
			}
#if ! defined(HAVE_SSL_SET1_HOST) && ! defined(HAVE_X509_VERIFY_PARAM_SET1_HOST)
			if(auth_name)
				log_err("no name verification functionality in "
				"ssl library, ignored name for %s", todo);
#endif
			if(!delegpt_add_ns_mlc(dp, dname, 0, auth_name, port)) {
				(void)ssl_printf(ssl, "error out of memory\n");
				free(dname);
				delegpt_free_mlc(dp);
				return NULL;
			}
		} else {
#if ! defined(HAVE_SSL_SET1_HOST) && ! defined(HAVE_X509_VERIFY_PARAM_SET1_HOST)
			if(auth_name)
				log_err("no name verification functionality in "
				"ssl library, ignored name for %s", todo);
#endif
			/* add address */
			if(!delegpt_add_addr_mlc(dp, &addr, addrlen, 0, 0,
				auth_name, -1)) {
				(void)ssl_printf(ssl, "error out of memory\n");
				delegpt_free_mlc(dp);
				return NULL;
			}
		}
	}
	dp->has_parent_side_NS = 1;
	return dp;
}

/** do the status command */
static void
do_forward(RES* ssl, struct worker* worker, char* args)
{
	struct iter_forwards* fwd = worker->env.fwds;
	uint8_t* root = (uint8_t*)"\000";
	if(!fwd) {
		(void)ssl_printf(ssl, "error: structure not allocated\n");
		return;
	}
	if(args == NULL || args[0] == 0) {
		(void)print_root_fwds(ssl, fwd, root);
		return;
	}
	/* set root forwards for this thread. since we are in remote control
	 * the actual mesh is not running, so we can freely edit it. */
	/* delete all the existing queries first */
	mesh_delete_all(worker->env.mesh);
	if(strcmp(args, "off") == 0) {
		lock_rw_wrlock(&fwd->lock);
		forwards_delete_zone(fwd, LDNS_RR_CLASS_IN, root);
		lock_rw_unlock(&fwd->lock);
	} else {
		struct delegpt* dp;
		if(!(dp = parse_delegpt(ssl, args, root)))
			return;
		lock_rw_wrlock(&fwd->lock);
		if(!forwards_add_zone(fwd, LDNS_RR_CLASS_IN, dp)) {
			lock_rw_unlock(&fwd->lock);
			(void)ssl_printf(ssl, "error out of memory\n");
			return;
		}
		lock_rw_unlock(&fwd->lock);
	}
	send_ok(ssl);
}

static int
parse_fs_args(RES* ssl, char* args, uint8_t** nm, struct delegpt** dp,
	int* insecure, int* prime, int* tls)
{
	char* zonename;
	char* rest;
	size_t nmlen;
	int nmlabs;
	/* parse all -x args */
	while(args[0] == '+') {
		if(!find_arg2(ssl, args, &rest))
			return 0;
		while(*(++args) != 0) {
			if(*args == 'i' && insecure)
				*insecure = 1;
			else if(*args == 'p' && prime)
				*prime = 1;
			else if(*args == 't' && tls)
				*tls = 1;
			else {
				(void)ssl_printf(ssl, "error: unknown option %s\n", args);
				return 0;
			}
		}
		args = rest;
	}
	/* parse name */
	if(dp) {
		if(!find_arg2(ssl, args, &rest))
			return 0;
		zonename = args;
		args = rest;
	} else	zonename = args;
	if(!parse_arg_name(ssl, zonename, nm, &nmlen, &nmlabs))
		return 0;

	/* parse dp */
	if(dp) {
		if(!(*dp = parse_delegpt(ssl, args, *nm))) {
			free(*nm);
			return 0;
		}
	}
	return 1;
}

/** do the forward_add command */
static void
do_forward_add(RES* ssl, struct worker* worker, char* args)
{
	struct iter_forwards* fwd = worker->env.fwds;
	int insecure = 0, tls = 0;
	uint8_t* nm = NULL;
	struct delegpt* dp = NULL;
	if(!parse_fs_args(ssl, args, &nm, &dp, &insecure, NULL, &tls))
		return;
	if(tls)
		dp->ssl_upstream = 1;
	lock_rw_wrlock(&fwd->lock);
	if(insecure && worker->env.anchors) {
		if(!anchors_add_insecure(worker->env.anchors, LDNS_RR_CLASS_IN,
			nm)) {
			lock_rw_unlock(&fwd->lock);
			(void)ssl_printf(ssl, "error out of memory\n");
			delegpt_free_mlc(dp);
			free(nm);
			return;
		}
	}
	if(!forwards_add_zone(fwd, LDNS_RR_CLASS_IN, dp)) {
		lock_rw_unlock(&fwd->lock);
		(void)ssl_printf(ssl, "error out of memory\n");
		free(nm);
		return;
	}
	lock_rw_unlock(&fwd->lock);
	free(nm);
	send_ok(ssl);
}

/** do the forward_remove command */
static void
do_forward_remove(RES* ssl, struct worker* worker, char* args)
{
	struct iter_forwards* fwd = worker->env.fwds;
	int insecure = 0;
	uint8_t* nm = NULL;
	if(!parse_fs_args(ssl, args, &nm, NULL, &insecure, NULL, NULL))
		return;
	lock_rw_wrlock(&fwd->lock);
	if(insecure && worker->env.anchors)
		anchors_delete_insecure(worker->env.anchors, LDNS_RR_CLASS_IN,
			nm);
	forwards_delete_zone(fwd, LDNS_RR_CLASS_IN, nm);
	lock_rw_unlock(&fwd->lock);
	free(nm);
	send_ok(ssl);
}

/** do the stub_add command */
static void
do_stub_add(RES* ssl, struct worker* worker, char* args)
{
	struct iter_forwards* fwd = worker->env.fwds;
	int insecure = 0, prime = 0, tls = 0;
	uint8_t* nm = NULL;
	struct delegpt* dp = NULL;
	if(!parse_fs_args(ssl, args, &nm, &dp, &insecure, &prime, &tls))
		return;
	if(tls)
		dp->ssl_upstream = 1;
	lock_rw_wrlock(&fwd->lock);
	lock_rw_wrlock(&worker->env.hints->lock);
	if(insecure && worker->env.anchors) {
		if(!anchors_add_insecure(worker->env.anchors, LDNS_RR_CLASS_IN,
			nm)) {
			lock_rw_unlock(&fwd->lock);
			lock_rw_unlock(&worker->env.hints->lock);
			(void)ssl_printf(ssl, "error out of memory\n");
			delegpt_free_mlc(dp);
			free(nm);
			return;
		}
	}
	if(!forwards_add_stub_hole(fwd, LDNS_RR_CLASS_IN, nm)) {
		if(insecure && worker->env.anchors)
			anchors_delete_insecure(worker->env.anchors,
				LDNS_RR_CLASS_IN, nm);
		lock_rw_unlock(&fwd->lock);
		lock_rw_unlock(&worker->env.hints->lock);
		(void)ssl_printf(ssl, "error out of memory\n");
		delegpt_free_mlc(dp);
		free(nm);
		return;
	}
	if(!hints_add_stub(worker->env.hints, LDNS_RR_CLASS_IN, dp, !prime)) {
		(void)ssl_printf(ssl, "error out of memory\n");
		forwards_delete_stub_hole(fwd, LDNS_RR_CLASS_IN, nm);
		if(insecure && worker->env.anchors)
			anchors_delete_insecure(worker->env.anchors,
				LDNS_RR_CLASS_IN, nm);
		lock_rw_unlock(&fwd->lock);
		lock_rw_unlock(&worker->env.hints->lock);
		free(nm);
		return;
	}
	lock_rw_unlock(&fwd->lock);
	lock_rw_unlock(&worker->env.hints->lock);
	free(nm);
	send_ok(ssl);
}

/** do the stub_remove command */
static void
do_stub_remove(RES* ssl, struct worker* worker, char* args)
{
	struct iter_forwards* fwd = worker->env.fwds;
	int insecure = 0;
	uint8_t* nm = NULL;
	if(!parse_fs_args(ssl, args, &nm, NULL, &insecure, NULL, NULL))
		return;
	lock_rw_wrlock(&fwd->lock);
	lock_rw_wrlock(&worker->env.hints->lock);
	if(insecure && worker->env.anchors)
		anchors_delete_insecure(worker->env.anchors, LDNS_RR_CLASS_IN,
			nm);
	forwards_delete_stub_hole(fwd, LDNS_RR_CLASS_IN, nm);
	hints_delete_stub(worker->env.hints, LDNS_RR_CLASS_IN, nm);
	lock_rw_unlock(&fwd->lock);
	lock_rw_unlock(&worker->env.hints->lock);
	free(nm);
	send_ok(ssl);
}

/** do the insecure_add command */
static void
do_insecure_add(RES* ssl, struct worker* worker, char* arg)
{
	size_t nmlen;
	int nmlabs;
	uint8_t* nm = NULL;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	if(worker->env.anchors) {
		if(!anchors_add_insecure(worker->env.anchors,
			LDNS_RR_CLASS_IN, nm)) {
			(void)ssl_printf(ssl, "error out of memory\n");
			free(nm);
			return;
		}
	}
	free(nm);
	send_ok(ssl);
}

/** do the insecure_remove command */
static void
do_insecure_remove(RES* ssl, struct worker* worker, char* arg)
{
	size_t nmlen;
	int nmlabs;
	uint8_t* nm = NULL;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	if(worker->env.anchors)
		anchors_delete_insecure(worker->env.anchors,
			LDNS_RR_CLASS_IN, nm);
	free(nm);
	send_ok(ssl);
}

static void
do_insecure_list(RES* ssl, struct worker* worker)
{
	char buf[257];
	struct trust_anchor* a;
	if(worker->env.anchors) {
		RBTREE_FOR(a, struct trust_anchor*, worker->env.anchors->tree) {
			if(a->numDS == 0 && a->numDNSKEY == 0) {
				dname_str(a->name, buf);
				ssl_printf(ssl, "%s\n", buf);
			}
		}
	}
}

/** do the status command */
static void
do_status(RES* ssl, struct worker* worker)
{
	int i;
	time_t uptime;
	if(!ssl_printf(ssl, "version: %s\n", PACKAGE_VERSION))
		return;
	if(!ssl_printf(ssl, "verbosity: %d\n", verbosity))
		return;
	if(!ssl_printf(ssl, "threads: %d\n", worker->daemon->num))
		return;
	if(!ssl_printf(ssl, "modules: %d [", worker->daemon->mods.num))
		return;
	for(i=0; i<worker->daemon->mods.num; i++) {
		if(!ssl_printf(ssl, " %s", worker->daemon->mods.mod[i]->name))
			return;
	}
	if(!ssl_printf(ssl, " ]\n"))
		return;
	uptime = (time_t)time(NULL) - (time_t)worker->daemon->time_boot.tv_sec;
	if(!ssl_printf(ssl, "uptime: " ARG_LL "d seconds\n", (long long)uptime))
		return;
	if(!ssl_printf(ssl, "options:%s%s%s%s\n" ,
		(worker->daemon->reuseport?" reuseport":""),
		(worker->daemon->rc->accept_list?" control":""),
		(worker->daemon->rc->accept_list && worker->daemon->rc->use_cert?"(ssl)":""),
		(worker->daemon->rc->accept_list && worker->daemon->cfg->control_ifs.first && worker->daemon->cfg->control_ifs.first->str && worker->daemon->cfg->control_ifs.first->str[0] == '/'?"(namedpipe)":"")
		))
		return;
	if(!ssl_printf(ssl, "unbound (pid %d) is running...\n",
		(int)getpid()))
		return;
}

/** get age for the mesh state */
static void
get_mesh_age(struct mesh_state* m, char* buf, size_t len,
	struct module_env* env)
{
	if(m->reply_list) {
		struct timeval d;
		struct mesh_reply* r = m->reply_list;
		/* last reply is the oldest */
		while(r && r->next)
			r = r->next;
		timeval_subtract(&d, env->now_tv, &r->start_time);
		snprintf(buf, len, ARG_LL "d.%6.6d",
			(long long)d.tv_sec, (int)d.tv_usec);
	} else {
		snprintf(buf, len, "-");
	}
}

/** get status of a mesh state */
static void
get_mesh_status(struct mesh_area* mesh, struct mesh_state* m,
	char* buf, size_t len)
{
	enum module_ext_state s = m->s.ext_state[m->s.curmod];
	const char *modname = mesh->mods.mod[m->s.curmod]->name;
	size_t l;
	if(strcmp(modname, "iterator") == 0 && s == module_wait_reply &&
		m->s.minfo[m->s.curmod]) {
		/* break into iterator to find out who its waiting for */
		struct iter_qstate* qstate = (struct iter_qstate*)
			m->s.minfo[m->s.curmod];
		struct outbound_list* ol = &qstate->outlist;
		struct outbound_entry* e;
		snprintf(buf, len, "%s wait for", modname);
		l = strlen(buf);
		buf += l; len -= l;
		if(ol->first == NULL)
			snprintf(buf, len, " (empty_list)");
		for(e = ol->first; e; e = e->next) {
			snprintf(buf, len, " ");
			l = strlen(buf);
			buf += l; len -= l;
			addr_to_str(&e->qsent->addr, e->qsent->addrlen,
				buf, len);
			l = strlen(buf);
			buf += l; len -= l;
		}
	} else if(s == module_wait_subquery) {
		/* look in subs from mesh state to see what */
		char nm[257];
		struct mesh_state_ref* sub;
		snprintf(buf, len, "%s wants", modname);
		l = strlen(buf);
		buf += l; len -= l;
		if(m->sub_set.count == 0)
			snprintf(buf, len, " (empty_list)");
		RBTREE_FOR(sub, struct mesh_state_ref*, &m->sub_set) {
			char* t = sldns_wire2str_type(sub->s->s.qinfo.qtype);
			char* c = sldns_wire2str_class(sub->s->s.qinfo.qclass);
			dname_str(sub->s->s.qinfo.qname, nm);
			snprintf(buf, len, " %s %s %s", (t?t:"TYPE??"),
				(c?c:"CLASS??"), nm);
			l = strlen(buf);
			buf += l; len -= l;
			free(t);
			free(c);
		}
	} else {
		snprintf(buf, len, "%s is %s", modname, strextstate(s));
	}
}

/** do the dump_requestlist command */
static void
do_dump_requestlist(RES* ssl, struct worker* worker)
{
	struct mesh_area* mesh;
	struct mesh_state* m;
	int num = 0;
	char buf[257];
	char timebuf[32];
	char statbuf[10240];
	if(!ssl_printf(ssl, "thread #%d\n", worker->thread_num))
		return;
	if(!ssl_printf(ssl, "#   type cl name    seconds    module status\n"))
		return;
	/* show worker mesh contents */
	mesh = worker->env.mesh;
	if(!mesh) return;
	RBTREE_FOR(m, struct mesh_state*, &mesh->all) {
		char* t = sldns_wire2str_type(m->s.qinfo.qtype);
		char* c = sldns_wire2str_class(m->s.qinfo.qclass);
		dname_str(m->s.qinfo.qname, buf);
		get_mesh_age(m, timebuf, sizeof(timebuf), &worker->env);
		get_mesh_status(mesh, m, statbuf, sizeof(statbuf));
		if(!ssl_printf(ssl, "%3d %4s %2s %s %s %s\n",
			num, (t?t:"TYPE??"), (c?c:"CLASS??"), buf, timebuf,
			statbuf)) {
			free(t);
			free(c);
			return;
		}
		num++;
		free(t);
		free(c);
	}
}

/** structure for argument data for dump infra host */
struct infra_arg {
	/** the infra cache */
	struct infra_cache* infra;
	/** the SSL connection */
	RES* ssl;
	/** the time now */
	time_t now;
	/** ssl failure? stop writing and skip the rest.  If the tcp
	 * connection is broken, and writes fail, we then stop writing. */
	int ssl_failed;
};

/** callback for every host element in the infra cache */
static void
dump_infra_host(struct lruhash_entry* e, void* arg)
{
	struct infra_arg* a = (struct infra_arg*)arg;
	struct infra_key* k = (struct infra_key*)e->key;
	struct infra_data* d = (struct infra_data*)e->data;
	char ip_str[1024];
	char name[257];
	int port;
	if(a->ssl_failed)
		return;
	addr_to_str(&k->addr, k->addrlen, ip_str, sizeof(ip_str));
	dname_str(k->zonename, name);
	port = (int)ntohs(((struct sockaddr_in*)&k->addr)->sin_port);
	if(port != UNBOUND_DNS_PORT) {
		snprintf(ip_str+strlen(ip_str), sizeof(ip_str)-strlen(ip_str),
			"@%d", port);
	}
	/* skip expired stuff (only backed off) */
	if(d->ttl < a->now) {
		if(d->rtt.rto >= USEFUL_SERVER_TOP_TIMEOUT) {
			if(!ssl_printf(a->ssl, "%s %s expired rto %d\n", ip_str,
				name, d->rtt.rto))  {
				a->ssl_failed = 1;
				return;
			}
		}
		return;
	}
	if(!ssl_printf(a->ssl, "%s %s ttl %lu ping %d var %d rtt %d rto %d "
		"tA %d tAAAA %d tother %d "
		"ednsknown %d edns %d delay %d lame dnssec %d rec %d A %d "
		"other %d\n", ip_str, name, (unsigned long)(d->ttl - a->now),
		d->rtt.srtt, d->rtt.rttvar, rtt_notimeout(&d->rtt), d->rtt.rto,
		d->timeout_A, d->timeout_AAAA, d->timeout_other,
		(int)d->edns_lame_known, (int)d->edns_version,
		(int)(a->now<d->probedelay?(d->probedelay - a->now):0),
		(int)d->isdnsseclame, (int)d->rec_lame, (int)d->lame_type_A,
		(int)d->lame_other)) {
		a->ssl_failed = 1;
		return;
	}
}

/** do the dump_infra command */
static void
do_dump_infra(RES* ssl, struct worker* worker)
{
	struct infra_arg arg;
	arg.infra = worker->env.infra_cache;
	arg.ssl = ssl;
	arg.now = *worker->env.now;
	arg.ssl_failed = 0;
	slabhash_traverse(arg.infra->hosts, 0, &dump_infra_host, (void*)&arg);
}

/** do the log_reopen command */
static void
do_log_reopen(RES* ssl, struct worker* worker)
{
	struct config_file* cfg = worker->env.cfg;
	send_ok(ssl);
	log_init(cfg->logfile, cfg->use_syslog, cfg->chrootdir);
}

/** do the auth_zone_reload command */
static void
do_auth_zone_reload(RES* ssl, struct worker* worker, char* arg)
{
	size_t nmlen;
	int nmlabs;
	uint8_t* nm = NULL;
	struct auth_zones* az = worker->env.auth_zones;
	struct auth_zone* z = NULL;
	struct auth_xfer* xfr = NULL;
	char* reason = NULL;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	if(az) {
		lock_rw_rdlock(&az->lock);
		z = auth_zone_find(az, nm, nmlen, LDNS_RR_CLASS_IN);
		if(z) {
			lock_rw_wrlock(&z->lock);
		}
		xfr = auth_xfer_find(az, nm, nmlen, LDNS_RR_CLASS_IN);
		if(xfr) {
			lock_basic_lock(&xfr->lock);
		}
		lock_rw_unlock(&az->lock);
	}
	free(nm);
	if(!z) {
		if(xfr) {
			lock_basic_unlock(&xfr->lock);
		}
		(void)ssl_printf(ssl, "error no auth-zone %s\n", arg);
		return;
	}
	if(!auth_zone_read_zonefile(z, worker->env.cfg)) {
		lock_rw_unlock(&z->lock);
		if(xfr) {
			lock_basic_unlock(&xfr->lock);
		}
		(void)ssl_printf(ssl, "error failed to read %s\n", arg);
		return;
	}

	z->zone_expired = 0;
	if(xfr) {
		xfr->zone_expired = 0;
		if(!xfr_find_soa(z, xfr)) {
			if(z->data.count == 0) {
				lock_rw_unlock(&z->lock);
				lock_basic_unlock(&xfr->lock);
				(void)ssl_printf(ssl, "zone %s has no contents\n", arg);
				return;
			}
			lock_rw_unlock(&z->lock);
			lock_basic_unlock(&xfr->lock);
			(void)ssl_printf(ssl, "error: no SOA in zone after read %s\n", arg);
			return;
		}
		if(xfr->have_zone)
			xfr->lease_time = *worker->env.now;
		lock_basic_unlock(&xfr->lock);
	}

	auth_zone_verify_zonemd(z, &worker->env, &worker->env.mesh->mods,
		&reason, 0, 0);
	if(reason && z->zone_expired) {
		lock_rw_unlock(&z->lock);
		(void)ssl_printf(ssl, "error zonemd for %s failed: %s\n",
			arg, reason);
		free(reason);
		return;
	} else if(reason && strcmp(reason, "ZONEMD verification successful")
		==0) {
		(void)ssl_printf(ssl, "%s: %s\n", arg, reason);
	}
	lock_rw_unlock(&z->lock);
	free(reason);
	send_ok(ssl);
}

/** do the auth_zone_transfer command */
static void
do_auth_zone_transfer(RES* ssl, struct worker* worker, char* arg)
{
	size_t nmlen;
	int nmlabs;
	uint8_t* nm = NULL;
	struct auth_zones* az = worker->env.auth_zones;
	if(!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
		return;
	if(!az || !auth_zones_startprobesequence(az, &worker->env, nm, nmlen,
		LDNS_RR_CLASS_IN)) {
		(void)ssl_printf(ssl, "error zone xfr task not found %s\n", arg);
		free(nm);
		return;
	}
	free(nm);
	send_ok(ssl);
}

/** do the set_option command */
static void
do_set_option(RES* ssl, struct worker* worker, char* arg)
{
	char* arg2;
	if(!find_arg2(ssl, arg, &arg2))
		return;
	if(!config_set_option(worker->env.cfg, arg, arg2)) {
		(void)ssl_printf(ssl, "error setting option\n");
		return;
	}
	/* effectuate some arguments */
	if(strcmp(arg, "val-override-date:") == 0) {
		int m = modstack_find(&worker->env.mesh->mods, "validator");
		struct val_env* val_env = NULL;
		if(m != -1) val_env = (struct val_env*)worker->env.modinfo[m];
		if(val_env)
			val_env->date_override = worker->env.cfg->val_date_override;
	}
	send_ok(ssl);
}

/* routine to printout option values over SSL */
void remote_get_opt_ssl(char* line, void* arg)
{
	RES* ssl = (RES*)arg;
	(void)ssl_printf(ssl, "%s\n", line);
}

/** do the get_option command */
static void
do_get_option(RES* ssl, struct worker* worker, char* arg)
{
	int r;
	r = config_get_option(worker->env.cfg, arg, remote_get_opt_ssl, ssl);
	if(!r) {
		(void)ssl_printf(ssl, "error unknown option\n");
		return;
	}
}

/** do the list_forwards command */
static void
do_list_forwards(RES* ssl, struct worker* worker)
{
	/* since its a per-worker structure no locks needed */
	struct iter_forwards* fwds = worker->env.fwds;
	struct iter_forward_zone* z;
	struct trust_anchor* a;
	int insecure;
	lock_rw_rdlock(&fwds->lock);
	RBTREE_FOR(z, struct iter_forward_zone*, fwds->tree) {
		if(!z->dp) continue; /* skip empty marker for stub */

		/* see if it is insecure */
		insecure = 0;
		if(worker->env.anchors &&
			(a=anchor_find(worker->env.anchors, z->name,
			z->namelabs, z->namelen,  z->dclass))) {
			if(!a->keylist && !a->numDS && !a->numDNSKEY)
				insecure = 1;
			lock_basic_unlock(&a->lock);
		}

		if(!ssl_print_name_dp(ssl, (insecure?"forward +i":"forward"),
			z->name, z->dclass, z->dp)) {
			lock_rw_unlock(&fwds->lock);
			return;
		}
	}
	lock_rw_unlock(&fwds->lock);
}

/** do the list_stubs command */
static void
do_list_stubs(RES* ssl, struct worker* worker)
{
	struct iter_hints_stub* z;
	struct trust_anchor* a;
	int insecure;
	char str[32];
	lock_rw_rdlock(&worker->env.hints->lock);
	RBTREE_FOR(z, struct iter_hints_stub*, &worker->env.hints->tree) {

		/* see if it is insecure */
		insecure = 0;
		if(worker->env.anchors &&
			(a=anchor_find(worker->env.anchors, z->node.name,
			z->node.labs, z->node.len,  z->node.dclass))) {
			if(!a->keylist && !a->numDS && !a->numDNSKEY)
				insecure = 1;
			lock_basic_unlock(&a->lock);
		}

		snprintf(str, sizeof(str), "stub %sprime%s",
			(z->noprime?"no":""), (insecure?" +i":""));
		if(!ssl_print_name_dp(ssl, str, z->node.name,
			z->node.dclass, z->dp)) {
			lock_rw_unlock(&worker->env.hints->lock);
			return;
		}
	}
	lock_rw_unlock(&worker->env.hints->lock);
}

/** do the list_auth_zones command */
static void
do_list_auth_zones(RES* ssl, struct auth_zones* az)
{
	struct auth_zone* z;
	char buf[257], buf2[256];
	lock_rw_rdlock(&az->lock);
	RBTREE_FOR(z, struct auth_zone*, &az->ztree) {
		lock_rw_rdlock(&z->lock);
		dname_str(z->name, buf);
		if(z->zone_expired)
			snprintf(buf2, sizeof(buf2), "expired");
		else {
			uint32_t serial = 0;
			if(auth_zone_get_serial(z, &serial))
				snprintf(buf2, sizeof(buf2), "serial %u",
					(unsigned)serial);
			else	snprintf(buf2, sizeof(buf2), "no serial");
		}
		if(!ssl_printf(ssl, "%s\t%s\n", buf, buf2)) {
			/* failure to print */
			lock_rw_unlock(&z->lock);
			lock_rw_unlock(&az->lock);
			return;
		}
		lock_rw_unlock(&z->lock);
	}
	lock_rw_unlock(&az->lock);
}

/** do the list_local_zones command */
static void
do_list_local_zones(RES* ssl, struct local_zones* zones)
{
	struct local_zone* z;
	char buf[257];
	lock_rw_rdlock(&zones->lock);
	RBTREE_FOR(z, struct local_zone*, &zones->ztree) {
		lock_rw_rdlock(&z->lock);
		dname_str(z->name, buf);
		if(!ssl_printf(ssl, "%s %s\n", buf,
			local_zone_type2str(z->type))) {
			/* failure to print */
			lock_rw_unlock(&z->lock);
			lock_rw_unlock(&zones->lock);
			return;
		}
		lock_rw_unlock(&z->lock);
	}
	lock_rw_unlock(&zones->lock);
}

/** do the list_local_data command */
static void
do_list_local_data(RES* ssl, struct worker* worker, struct local_zones* zones)
{
	struct local_zone* z;
	struct local_data* d;
	struct local_rrset* p;
	char* s = (char*)sldns_buffer_begin(worker->env.scratch_buffer);
	size_t slen = sldns_buffer_capacity(worker->env.scratch_buffer);
	lock_rw_rdlock(&zones->lock);
	RBTREE_FOR(z, struct local_zone*, &zones->ztree) {
		lock_rw_rdlock(&z->lock);
		RBTREE_FOR(d, struct local_data*, &z->data) {
			for(p = d->rrsets; p; p = p->next) {
				struct packed_rrset_data* d =
					(struct packed_rrset_data*)p->rrset->entry.data;
				size_t i;
				for(i=0; i<d->count + d->rrsig_count; i++) {
					if(!packed_rr_to_string(p->rrset, i,
						0, s, slen)) {
						if(!ssl_printf(ssl, "BADRR\n")) {
							lock_rw_unlock(&z->lock);
							lock_rw_unlock(&zones->lock);
							return;
						}
					}
				        if(!ssl_printf(ssl, "%s\n", s)) {
						lock_rw_unlock(&z->lock);
						lock_rw_unlock(&zones->lock);
						return;
					}
				}
			}
		}
		lock_rw_unlock(&z->lock);
	}
	lock_rw_unlock(&zones->lock);
}

/** do the view_list_local_zones command */
static void
do_view_list_local_zones(RES* ssl, struct worker* worker, char* arg)
{
	struct view* v = views_find_view(worker->daemon->views,
		arg, 0 /* get read lock*/);
	if(!v) {
		ssl_printf(ssl,"no view with name: %s\n", arg);
		return;
	}
	if(v->local_zones) {
		do_list_local_zones(ssl, v->local_zones);
	}
	lock_rw_unlock(&v->lock);
}

/** do the view_list_local_data command */
static void
do_view_list_local_data(RES* ssl, struct worker* worker, char* arg)
{
	struct view* v = views_find_view(worker->daemon->views,
		arg, 0 /* get read lock*/);
	if(!v) {
		ssl_printf(ssl,"no view with name: %s\n", arg);
		return;
	}
	if(v->local_zones) {
		do_list_local_data(ssl, worker, v->local_zones);
	}
	lock_rw_unlock(&v->lock);
}

/** struct for user arg ratelimit list */
struct ratelimit_list_arg {
	/** the infra cache */
	struct infra_cache* infra;
	/** the SSL to print to */
	RES* ssl;
	/** all or only ratelimited */
	int all;
	/** current time */
	time_t now;
	/** if backoff is enabled */
	int backoff;
};

#define ip_ratelimit_list_arg ratelimit_list_arg

/** list items in the ratelimit table */
static void
rate_list(struct lruhash_entry* e, void* arg)
{
	struct ratelimit_list_arg* a = (struct ratelimit_list_arg*)arg;
	struct rate_key* k = (struct rate_key*)e->key;
	struct rate_data* d = (struct rate_data*)e->data;
	char buf[257];
	int lim = infra_find_ratelimit(a->infra, k->name, k->namelen);
	int max = infra_rate_max(d, a->now, a->backoff);
	if(a->all == 0) {
		if(max < lim)
			return;
	}
	dname_str(k->name, buf);
	ssl_printf(a->ssl, "%s %d limit %d\n", buf, max, lim);
}

/** list items in the ip_ratelimit table */
static void
ip_rate_list(struct lruhash_entry* e, void* arg)
{
	char ip[128];
	struct ip_ratelimit_list_arg* a = (struct ip_ratelimit_list_arg*)arg;
	struct ip_rate_key* k = (struct ip_rate_key*)e->key;
	struct ip_rate_data* d = (struct ip_rate_data*)e->data;
	int lim = infra_ip_ratelimit;
	int max = infra_rate_max(d, a->now, a->backoff);
	if(a->all == 0) {
		if(max < lim)
			return;
	}
	addr_to_str(&k->addr, k->addrlen, ip, sizeof(ip));
	ssl_printf(a->ssl, "%s %d limit %d\n", ip, max, lim);
}

/** do the ratelimit_list command */
static void
do_ratelimit_list(RES* ssl, struct worker* worker, char* arg)
{
	struct ratelimit_list_arg a;
	a.all = 0;
	a.infra = worker->env.infra_cache;
	a.now = *worker->env.now;
	a.ssl = ssl;
	a.backoff = worker->env.cfg->ratelimit_backoff;
	arg = skipwhite(arg);
	if(strcmp(arg, "+a") == 0)
		a.all = 1;
	if(a.infra->domain_rates==NULL ||
		(a.all == 0 && infra_dp_ratelimit == 0))
		return;
	slabhash_traverse(a.infra->domain_rates, 0, rate_list, &a);
}

/** do the ip_ratelimit_list command */
static void
do_ip_ratelimit_list(RES* ssl, struct worker* worker, char* arg)
{
	struct ip_ratelimit_list_arg a;
	a.all = 0;
	a.infra = worker->env.infra_cache;
	a.now = *worker->env.now;
	a.ssl = ssl;
	a.backoff = worker->env.cfg->ip_ratelimit_backoff;
	arg = skipwhite(arg);
	if(strcmp(arg, "+a") == 0)
		a.all = 1;
	if(a.infra->client_ip_rates==NULL ||
		(a.all == 0 && infra_ip_ratelimit == 0))
		return;
	slabhash_traverse(a.infra->client_ip_rates, 0, ip_rate_list, &a);
}

/** do the rpz_enable/disable command */
static void
do_rpz_enable_disable(RES* ssl, struct worker* worker, char* arg, int enable) {
    size_t nmlen;
    int nmlabs;
    uint8_t *nm = NULL;
    struct auth_zones *az = worker->env.auth_zones;
    struct auth_zone *z = NULL;
    if (!parse_arg_name(ssl, arg, &nm, &nmlen, &nmlabs))
        return;
    if (az) {
        lock_rw_rdlock(&az->lock);
        z = auth_zone_find(az, nm, nmlen, LDNS_RR_CLASS_IN);
        if (z) {
            lock_rw_wrlock(&z->lock);
        }
        lock_rw_unlock(&az->lock);
    }
    free(nm);
    if (!z) {
        (void) ssl_printf(ssl, "error no auth-zone %s\n", arg);
        return;
    }
    if (!z->rpz) {
        (void) ssl_printf(ssl, "error auth-zone %s not RPZ\n", arg);
        lock_rw_unlock(&z->lock);
        return;
    }
    if (enable) {
        rpz_enable(z->rpz);
    } else {
        rpz_disable(z->rpz);
    }
    lock_rw_unlock(&z->lock);
    send_ok(ssl);
}

/** do the rpz_enable command */
static void
do_rpz_enable(RES* ssl, struct worker* worker, char* arg)
{
    do_rpz_enable_disable(ssl, worker, arg, 1);
}

/** do the rpz_disable command */
static void
do_rpz_disable(RES* ssl, struct worker* worker, char* arg)
{
    do_rpz_enable_disable(ssl, worker, arg, 0);
}

/** tell other processes to execute the command */
static void
distribute_cmd(struct daemon_remote* rc, RES* ssl, char* cmd)
{
	int i;
	if(!cmd || !ssl)
		return;
	/* skip i=0 which is me */
	for(i=1; i<rc->worker->daemon->num; i++) {
		worker_send_cmd(rc->worker->daemon->workers[i],
			worker_cmd_remote);
		if(!tube_write_msg(rc->worker->daemon->workers[i]->cmd,
			(uint8_t*)cmd, strlen(cmd)+1, 0)) {
			ssl_printf(ssl, "error could not distribute cmd\n");
			return;
		}
	}
}

/** check for name with end-of-string, space or tab after it */
static int
cmdcmp(char* p, const char* cmd, size_t len)
{
	return strncmp(p,cmd,len)==0 && (p[len]==0||p[len]==' '||p[len]=='\t');
}

/** execute a remote control command */
static void
execute_cmd(struct daemon_remote* rc, struct rc_state* s, RES* ssl, char* cmd,
	struct worker* worker)
{
	char* p = skipwhite(cmd);
	/* compare command */
	if(cmdcmp(p, "stop", 4)) {
		do_stop(ssl, worker);
		return;
	} else if(cmdcmp(p, "reload_keep_cache", 17)) {
		do_reload(ssl, worker, 1);
		return;
	} else if(cmdcmp(p, "reload", 6)) {
		do_reload(ssl, worker, 0);
		return;
	} else if(cmdcmp(p, "fast_reload", 11)) {
		do_fast_reload(ssl, worker, s, skipwhite(p+11));
		return;
	} else if(cmdcmp(p, "stats_noreset", 13)) {
		do_stats(ssl, worker, 0);
		return;
	} else if(cmdcmp(p, "stats", 5)) {
		do_stats(ssl, worker, 1);
		return;
	} else if(cmdcmp(p, "status", 6)) {
		do_status(ssl, worker);
		return;
	} else if(cmdcmp(p, "dump_cache", 10)) {
		(void)dump_cache(ssl, worker);
		return;
	} else if(cmdcmp(p, "load_cache", 10)) {
		if(load_cache(ssl, worker)) send_ok(ssl);
		return;
	} else if(cmdcmp(p, "list_forwards", 13)) {
		do_list_forwards(ssl, worker);
		return;
	} else if(cmdcmp(p, "list_stubs", 10)) {
		do_list_stubs(ssl, worker);
		return;
	} else if(cmdcmp(p, "list_insecure", 13)) {
		do_insecure_list(ssl, worker);
		return;
	} else if(cmdcmp(p, "list_local_zones", 16)) {
		do_list_local_zones(ssl, worker->daemon->local_zones);
		return;
	} else if(cmdcmp(p, "list_local_data", 15)) {
		do_list_local_data(ssl, worker, worker->daemon->local_zones);
		return;
	} else if(cmdcmp(p, "view_list_local_zones", 21)) {
		do_view_list_local_zones(ssl, worker, skipwhite(p+21));
		return;
	} else if(cmdcmp(p, "view_list_local_data", 20)) {
		do_view_list_local_data(ssl, worker, skipwhite(p+20));
		return;
	} else if(cmdcmp(p, "ratelimit_list", 14)) {
		do_ratelimit_list(ssl, worker, p+14);
		return;
	} else if(cmdcmp(p, "ip_ratelimit_list", 17)) {
		do_ip_ratelimit_list(ssl, worker, p+17);
		return;
	} else if(cmdcmp(p, "list_auth_zones", 15)) {
		do_list_auth_zones(ssl, worker->env.auth_zones);
		return;
	} else if(cmdcmp(p, "auth_zone_reload", 16)) {
		do_auth_zone_reload(ssl, worker, skipwhite(p+16));
		return;
	} else if(cmdcmp(p, "auth_zone_transfer", 18)) {
		do_auth_zone_transfer(ssl, worker, skipwhite(p+18));
		return;
	} else if(cmdcmp(p, "insecure_add", 12)) {
		/* must always distribute this cmd */
		if(rc) distribute_cmd(rc, ssl, cmd);
		do_insecure_add(ssl, worker, skipwhite(p+12));
		return;
	} else if(cmdcmp(p, "insecure_remove", 15)) {
		/* must always distribute this cmd */
		if(rc) distribute_cmd(rc, ssl, cmd);
		do_insecure_remove(ssl, worker, skipwhite(p+15));
		return;
	} else if(cmdcmp(p, "flush_stats", 11)) {
		/* must always distribute this cmd */
		if(rc) distribute_cmd(rc, ssl, cmd);
		do_flush_stats(ssl, worker);
		return;
	} else if(cmdcmp(p, "flush_requestlist", 17)) {
		/* must always distribute this cmd */
		if(rc) distribute_cmd(rc, ssl, cmd);
		do_flush_requestlist(ssl, worker);
		return;
	} else if(cmdcmp(p, "lookup", 6)) {
		do_lookup(ssl, worker, skipwhite(p+6));
		return;
	}

#ifdef THREADS_DISABLED
	/* other processes must execute the command as well */
	/* commands that should not be distributed, returned above. */
	if(rc) { /* only if this thread is the master (rc) thread */
		/* done before the code below, which may split the string */
		distribute_cmd(rc, ssl, cmd);
	}
#endif
	if(cmdcmp(p, "verbosity", 9)) {
		do_verbosity(ssl, skipwhite(p+9));
	} else if(cmdcmp(p, "local_zone_remove", 17)) {
		do_zone_remove(ssl, worker->daemon->local_zones, skipwhite(p+17));
	} else if(cmdcmp(p, "local_zones_remove", 18)) {
		do_zones_remove(ssl, worker->daemon->local_zones);
	} else if(cmdcmp(p, "local_zone", 10)) {
		do_zone_add(ssl, worker->daemon->local_zones, skipwhite(p+10));
	} else if(cmdcmp(p, "local_zones", 11)) {
		do_zones_add(ssl, worker->daemon->local_zones);
	} else if(cmdcmp(p, "local_data_remove", 17)) {
		do_data_remove(ssl, worker->daemon->local_zones, skipwhite(p+17));
	} else if(cmdcmp(p, "local_datas_remove", 18)) {
		do_datas_remove(ssl, worker->daemon->local_zones);
	} else if(cmdcmp(p, "local_data", 10)) {
		do_data_add(ssl, worker->daemon->local_zones, skipwhite(p+10));
	} else if(cmdcmp(p, "local_datas", 11)) {
		do_datas_add(ssl, worker->daemon->local_zones);
	} else if(cmdcmp(p, "forward_add", 11)) {
		do_forward_add(ssl, worker, skipwhite(p+11));
	} else if(cmdcmp(p, "forward_remove", 14)) {
		do_forward_remove(ssl, worker, skipwhite(p+14));
	} else if(cmdcmp(p, "forward", 7)) {
		do_forward(ssl, worker, skipwhite(p+7));
	} else if(cmdcmp(p, "stub_add", 8)) {
		do_stub_add(ssl, worker, skipwhite(p+8));
	} else if(cmdcmp(p, "stub_remove", 11)) {
		do_stub_remove(ssl, worker, skipwhite(p+11));
	} else if(cmdcmp(p, "view_local_zone_remove", 22)) {
		do_view_zone_remove(ssl, worker, skipwhite(p+22));
	} else if(cmdcmp(p, "view_local_zone", 15)) {
		do_view_zone_add(ssl, worker, skipwhite(p+15));
	} else if(cmdcmp(p, "view_local_data_remove", 22)) {
		do_view_data_remove(ssl, worker, skipwhite(p+22));
	} else if(cmdcmp(p, "view_local_datas_remove", 23)){
		do_view_datas_remove(ssl, worker, skipwhite(p+23));
	} else if(cmdcmp(p, "view_local_data", 15)) {
		do_view_data_add(ssl, worker, skipwhite(p+15));
	} else if(cmdcmp(p, "view_local_datas", 16)) {
		do_view_datas_add(ssl, worker, skipwhite(p+16));
	} else if(cmdcmp(p, "flush_zone", 10)) {
		do_flush_zone(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "flush_type", 10)) {
		do_flush_type(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "flush_infra", 11)) {
		do_flush_infra(ssl, worker, skipwhite(p+11));
	} else if(cmdcmp(p, "flush", 5)) {
		do_flush_name(ssl, worker, skipwhite(p+5));
	} else if(cmdcmp(p, "dump_requestlist", 16)) {
		do_dump_requestlist(ssl, worker);
	} else if(cmdcmp(p, "dump_infra", 10)) {
		do_dump_infra(ssl, worker);
	} else if(cmdcmp(p, "log_reopen", 10)) {
		do_log_reopen(ssl, worker);
	} else if(cmdcmp(p, "set_option", 10)) {
		do_set_option(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "get_option", 10)) {
		do_get_option(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "flush_bogus", 11)) {
		do_flush_bogus(ssl, worker);
	} else if(cmdcmp(p, "flush_negative", 14)) {
		do_flush_negative(ssl, worker);
	} else if(cmdcmp(p, "rpz_enable", 10)) {
		do_rpz_enable(ssl, worker, skipwhite(p+10));
	} else if(cmdcmp(p, "rpz_disable", 11)) {
		do_rpz_disable(ssl, worker, skipwhite(p+11));
	} else {
		(void)ssl_printf(ssl, "error unknown command '%s'\n", p);
	}
}

void
daemon_remote_exec(struct worker* worker)
{
	/* read the cmd string */
	uint8_t* msg = NULL;
	uint32_t len = 0;
	if(!tube_read_msg(worker->cmd, &msg, &len, 0)) {
		log_err("daemon_remote_exec: tube_read_msg failed");
		return;
	}
	verbose(VERB_ALGO, "remote exec distributed: %s", (char*)msg);
	execute_cmd(NULL, NULL, NULL, (char*)msg, worker);
	free(msg);
}

/** handle remote control request */
static void
handle_req(struct daemon_remote* rc, struct rc_state* s, RES* res)
{
	int r;
	char pre[10];
	char magic[7];
	char buf[1024];
#ifdef USE_WINSOCK
	/* makes it possible to set the socket blocking again. */
	/* basically removes it from winsock_event ... */
	WSAEventSelect(s->c->fd, NULL, 0);
#endif
	fd_set_block(s->c->fd);

	/* try to read magic UBCT[version]_space_ string */
	if(res->ssl) {
		ERR_clear_error();
		if((r=SSL_read(res->ssl, magic, (int)sizeof(magic)-1)) <= 0) {
			int r2;
			if((r2=SSL_get_error(res->ssl, r)) == SSL_ERROR_ZERO_RETURN)
				return;
			log_crypto_err_io("could not SSL_read", r2);
			return;
		}
	} else {
		while(1) {
			ssize_t rr = recv(res->fd, magic, sizeof(magic)-1, 0);
			if(rr <= 0) {
				if(rr == 0) return;
				if(errno == EINTR || errno == EAGAIN)
					continue;
				log_err("could not recv: %s", sock_strerror(errno));
				return;
			}
			r = (int)rr;
			break;
		}
	}
	magic[6] = 0;
	if( r != 6 || strncmp(magic, "UBCT", 4) != 0) {
		verbose(VERB_QUERY, "control connection has bad magic string");
		/* probably wrong tool connected, ignore it completely */
		return;
	}

	/* read the command line */
	if(!ssl_read_line(res, buf, sizeof(buf))) {
		return;
	}
	snprintf(pre, sizeof(pre), "UBCT%d ", UNBOUND_CONTROL_VERSION);
	if(strcmp(magic, pre) != 0) {
		verbose(VERB_QUERY, "control connection had bad "
			"version %s, cmd: %s", magic, buf);
		ssl_printf(res, "error version mismatch\n");
		return;
	}
	verbose(VERB_DETAIL, "control cmd: %s", buf);

	/* figure out what to do */
	execute_cmd(rc, s, res, buf, rc->worker);
}

/** handle SSL_do_handshake changes to the file descriptor to wait for later */
static int
remote_handshake_later(struct daemon_remote* rc, struct rc_state* s,
	struct comm_point* c, int r, int r2)
{
	if(r2 == SSL_ERROR_WANT_READ) {
		if(s->shake_state == rc_hs_read) {
			/* try again later */
			return 0;
		}
		s->shake_state = rc_hs_read;
		comm_point_listen_for_rw(c, 1, 0);
		return 0;
	} else if(r2 == SSL_ERROR_WANT_WRITE) {
		if(s->shake_state == rc_hs_write) {
			/* try again later */
			return 0;
		}
		s->shake_state = rc_hs_write;
		comm_point_listen_for_rw(c, 0, 1);
		return 0;
	} else {
		if(r == 0)
			log_err("remote control connection closed prematurely");
		log_addr(VERB_OPS, "failed connection from",
			&s->c->repinfo.remote_addr, s->c->repinfo.remote_addrlen);
		log_crypto_err_io("remote control failed ssl", r2);
		clean_point(rc, s);
	}
	return 0;
}

int remote_control_callback(struct comm_point* c, void* arg, int err,
	struct comm_reply* ATTR_UNUSED(rep))
{
	RES res;
	struct rc_state* s = (struct rc_state*)arg;
	struct daemon_remote* rc = s->rc;
	int r;
	if(err != NETEVENT_NOERROR) {
		if(err==NETEVENT_TIMEOUT)
			log_err("remote control timed out");
		clean_point(rc, s);
		return 0;
	}
	if(s->ssl) {
		/* (continue to) setup the SSL connection */
		ERR_clear_error();
		r = SSL_do_handshake(s->ssl);
		if(r != 1) {
			int r2 = SSL_get_error(s->ssl, r);
			return remote_handshake_later(rc, s, c, r, r2);
		}
		s->shake_state = rc_none;
	}

	/* once handshake has completed, check authentication */
	if (!rc->use_cert) {
		verbose(VERB_ALGO, "unauthenticated remote control connection");
	} else if(SSL_get_verify_result(s->ssl) == X509_V_OK) {
#ifdef HAVE_SSL_GET1_PEER_CERTIFICATE
		X509* x = SSL_get1_peer_certificate(s->ssl);
#else
		X509* x = SSL_get_peer_certificate(s->ssl);
#endif
		if(!x) {
			verbose(VERB_DETAIL, "remote control connection "
				"provided no client certificate");
			clean_point(rc, s);
			return 0;
		}
		verbose(VERB_ALGO, "remote control connection authenticated");
		X509_free(x);
	} else {
		verbose(VERB_DETAIL, "remote control connection failed to "
			"authenticate with client certificate");
		clean_point(rc, s);
		return 0;
	}

	/* if OK start to actually handle the request */
	res.ssl = s->ssl;
	res.fd = c->fd;
	handle_req(rc, s, &res);

	verbose(VERB_ALGO, "remote control operation completed");
	clean_point(rc, s);
	return 0;
}

/**
 * This routine polls a socket for readiness.
 * @param fd: file descriptor, -1 uses no fd for a timer only.
 * @param timeout: time in msec to wait. 0 means nonblocking test,
 * 	-1 waits blocking for events.
 * @param pollin: check for input event.
 * @param pollout: check for output event.
 * @param event: output variable, set to true if the event happens.
 * 	It is false if there was an error or timeout.
 * @return false is system call failure, also logged.
 */
static int
sock_poll_timeout(int fd, int timeout, int pollin, int pollout, int* event)
{
	int loopcount = 0;
	/* Loop if the system call returns an errno to do so, like EINTR. */
	while(1) {
		struct pollfd p, *fds;
		int nfds, ret;
		if(++loopcount > IPC_LOOP_MAX) {
			log_err("sock_poll_timeout: loop");
			if(event)
				*event = 0;
			return 0;
		}
		if(fd == -1) {
			fds = NULL;
			nfds = 0;
		} else {
			fds = &p;
			nfds = 1;
			memset(&p, 0, sizeof(p));
			p.fd = fd;
			p.events = POLLERR | POLLHUP;
			if(pollin)
				p.events |= POLLIN;
			if(pollout)
				p.events |= POLLOUT;
		}
#ifndef USE_WINSOCK
		ret = poll(fds, nfds, timeout);
#else
		ret = WSAPoll(fds, nfds, timeout);
#endif
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
				errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
				|| errno == EWOULDBLOCK
#  endif
#else
				WSAGetLastError() == WSAEINTR ||
				WSAGetLastError() == WSAEINPROGRESS ||
				WSAGetLastError() == WSAEWOULDBLOCK
#endif
				)
				continue; /* Try again. */
			log_err("poll: %s", sock_strerror(errno));
			if(event)
				*event = 0;
			return 0;
		} else if(ret == 0) {
			/* Timeout */
			if(event)
				*event = 0;
			return 1;
		}
		break;
	}
	if(event)
		*event = 1;
	return 1;
}

/** fast reload convert fast reload notification status to string */
static const char*
fr_notification_to_string(enum fast_reload_notification status)
{
	switch(status) {
	case fast_reload_notification_none:
		return "none";
	case fast_reload_notification_done:
		return "done";
	case fast_reload_notification_done_error:
		return "done_error";
	case fast_reload_notification_exit:
		return "exit";
	case fast_reload_notification_exited:
		return "exited";
	case fast_reload_notification_printout:
		return "printout";
	case fast_reload_notification_reload_stop:
		return "reload_stop";
	case fast_reload_notification_reload_ack:
		return "reload_ack";
	case fast_reload_notification_reload_nopause_poll:
		return "reload_nopause_poll";
	case fast_reload_notification_reload_start:
		return "reload_start";
	default:
		break;
	}
	return "unknown";
}

#ifndef THREADS_DISABLED
/** fast reload, poll for notification incoming. True if quit */
static int
fr_poll_for_quit(struct fast_reload_thread* fr)
{
	int inevent, loopexit = 0, bcount = 0;
	uint32_t cmd;
	ssize_t ret;

	if(fr->need_to_quit)
		return 1;
	/* Is there data? */
	if(!sock_poll_timeout(fr->commpair[1], 0, 1, 0, &inevent)) {
		log_err("fr_poll_for_quit: poll failed");
		return 0;
	}
	if(!inevent)
		return 0;

	/* Read the data */
	while(1) {
		if(++loopexit > IPC_LOOP_MAX) {
			log_err("fr_poll_for_quit: recv loops %s",
				sock_strerror(errno));
			return 0;
		}
		ret = recv(fr->commpair[1], ((char*)&cmd)+bcount,
			sizeof(cmd)-bcount, 0);
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
				errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
				|| errno == EWOULDBLOCK
#  endif
#else
				WSAGetLastError() == WSAEINTR ||
				WSAGetLastError() == WSAEINPROGRESS ||
				WSAGetLastError() == WSAEWOULDBLOCK
#endif
				)
				continue; /* Try again. */
			log_err("fr_poll_for_quit: recv: %s",
				sock_strerror(errno));
			return 0;
		} else if(ret+(ssize_t)bcount != sizeof(cmd)) {
			bcount += ret;
			if((size_t)bcount < sizeof(cmd))
				continue;
		}
		break;
	}
	if(cmd == fast_reload_notification_exit) {
		fr->need_to_quit = 1;
		verbose(VERB_ALGO, "fast reload: exit notification received");
		return 1;
	}
	log_err("fr_poll_for_quit: unknown notification status received: %d %s",
		cmd, fr_notification_to_string(cmd));
	return 0;
}

/** fast reload thread. Send notification from the fast reload thread */
static void
fr_send_notification(struct fast_reload_thread* fr,
	enum fast_reload_notification status)
{
	int outevent, loopexit = 0, bcount = 0;
	uint32_t cmd;
	ssize_t ret;
	verbose(VERB_ALGO, "fast reload: send notification %s",
		fr_notification_to_string(status));
	/* Make a blocking attempt to send. But meanwhile stay responsive,
	 * once in a while for quit commands. In case the server has to quit. */
	/* see if there is incoming quit signals */
	if(fr_poll_for_quit(fr))
		return;
	cmd = status;
	while(1) {
		if(++loopexit > IPC_LOOP_MAX) {
			log_err("fast reload: could not send notification");
			return;
		}
		/* wait for socket to become writable */
		if(!sock_poll_timeout(fr->commpair[1], IPC_NOTIFICATION_WAIT,
			0, 1, &outevent)) {
			log_err("fast reload: poll failed");
			return;
		}
		if(fr_poll_for_quit(fr))
			return;
		if(!outevent)
			continue;
		ret = send(fr->commpair[1], ((char*)&cmd)+bcount,
			sizeof(cmd)-bcount, 0);
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
				errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
				|| errno == EWOULDBLOCK
#  endif
#else
				WSAGetLastError() == WSAEINTR ||
				WSAGetLastError() == WSAEINPROGRESS ||
				WSAGetLastError() == WSAEWOULDBLOCK
#endif
				)
				continue; /* Try again. */
			log_err("fast reload send notification: send: %s",
				sock_strerror(errno));
			return;
		} else if(ret+(ssize_t)bcount != sizeof(cmd)) {
			bcount += ret;
			if((size_t)bcount < sizeof(cmd))
				continue;
		}
		break;
	}
}

/** fast reload thread queue up text string for output */
static int
fr_output_text(struct fast_reload_thread* fr, const char* msg)
{
	char* item = strdup(msg);
	if(!item) {
		log_err("fast reload output text: strdup out of memory");
		return 0;
	}
	lock_basic_lock(&fr->fr_output_lock);
	if(!cfg_strlist_append(fr->fr_output, item)) {
		lock_basic_unlock(&fr->fr_output_lock);
		/* The item is freed by cfg_strlist_append on failure. */
		log_err("fast reload output text: append out of memory");
		return 0;
	}
	lock_basic_unlock(&fr->fr_output_lock);
	return 1;
}

/** fast reload thread output vmsg function */
static int
fr_output_vmsg(struct fast_reload_thread* fr, const char* format, va_list args)
{
	char msg[1024];
	vsnprintf(msg, sizeof(msg), format, args);
	return fr_output_text(fr, msg);
}

/** fast reload thread printout function, with printf arguments */
static int fr_output_printf(struct fast_reload_thread* fr,
	const char* format, ...) ATTR_FORMAT(printf, 2, 3);

/** fast reload thread printout function, prints to list and signals
 * the remote control thread to move that to get written to the socket
 * of the remote control connection. */
static int
fr_output_printf(struct fast_reload_thread* fr, const char* format, ...)
{
	va_list args;
	int ret;
	va_start(args, format);
	ret = fr_output_vmsg(fr, format, args);
	va_end(args);
	return ret;
}

/** fast reload thread, init time counters */
static void
fr_init_time(struct timeval* time_start, struct timeval* time_read,
	struct timeval* time_construct, struct timeval* time_reload,
	struct timeval* time_end)
{
	memset(time_start, 0, sizeof(*time_start));
	memset(time_read, 0, sizeof(*time_read));
	memset(time_construct, 0, sizeof(*time_construct));
	memset(time_reload, 0, sizeof(*time_reload));
	memset(time_end, 0, sizeof(*time_end));
	if(gettimeofday(time_start, NULL) < 0)
		log_err("gettimeofday: %s", strerror(errno));
}

/**
 * Structure with constructed elements for use during fast reload.
 * At the start it contains the tree items for the new config.
 * After the tree items are swapped into the server, the old elements
 * are kept in here. They can then be deleted.
 */
struct fast_reload_construct {
	/** construct for views */
	struct views* views;
	/** construct for forwards */
	struct iter_forwards* fwds;
	/** construct for stubs */
	struct iter_hints* hints;
	/** storage for the old configuration elements. The outer struct
	 * is allocated with malloc here, the items are from config. */
	struct config_file* oldcfg;
};

/** fast reload thread, read config */
static int
fr_read_config(struct fast_reload_thread* fr, struct config_file** newcfg)
{
	/* Create new config structure. */
	*newcfg = config_create();
	if(!*newcfg) {
		if(!fr_output_printf(fr, "config_create failed: out of memory\n"))
			return 0;
		fr_send_notification(fr, fast_reload_notification_printout);
		return 0;
	}
	if(fr_poll_for_quit(fr))
		return 1;

	/* Read new config from file */
	if(!config_read(*newcfg, fr->worker->daemon->cfgfile,
		fr->worker->daemon->chroot)) {
		config_delete(*newcfg);
		if(!fr_output_printf(fr, "config_read %s failed: %s\n",
			fr->worker->daemon->cfgfile, strerror(errno)))
			return 0;
		fr_send_notification(fr, fast_reload_notification_printout);
		return 0;
	}
	if(fr_poll_for_quit(fr))
		return 1;
	if(fr->fr_verb >= 1) {
		if(!fr_output_printf(fr, "done read config file %s\n",
			fr->worker->daemon->cfgfile))
			return 0;
		fr_send_notification(fr, fast_reload_notification_printout);
	}

	return 1;
}

/** fast reload thread, clear construct information, deletes items */
static void
fr_construct_clear(struct fast_reload_construct* ct)
{
	if(!ct)
		return;
	forwards_delete(ct->fwds);
	hints_delete(ct->hints);
	views_delete(ct->views);
	/* Delete the log identity here so that the global value is not
	 * reset by config_delete. */
	if(ct->oldcfg && ct->oldcfg->log_identity) {
		free(ct->oldcfg->log_identity);
		ct->oldcfg->log_identity = NULL;
	}
	config_delete(ct->oldcfg);
}

/** get memory for strlist */
static size_t
getmem_config_strlist(struct config_strlist* p)
{
	size_t m = 0;
	struct config_strlist* s;
	for(s = p; s; s = s->next)
		m += sizeof(*s) + getmem_str(s->str);
	return m;
}

/** get memory for str2list */
static size_t
getmem_config_str2list(struct config_str2list* p)
{
	size_t m = 0;
	struct config_str2list* s;
	for(s = p; s; s = s->next)
		m += sizeof(*s) + getmem_str(s->str) + getmem_str(s->str2);
	return m;
}

/** get memory for str3list */
static size_t
getmem_config_str3list(struct config_str3list* p)
{
	size_t m = 0;
	struct config_str3list* s;
	for(s = p; s; s = s->next)
		m += sizeof(*s) + getmem_str(s->str) + getmem_str(s->str2)
			+ getmem_str(s->str3);
	return m;
}

/** get memory for strbytelist */
static size_t
getmem_config_strbytelist(struct config_strbytelist* p)
{
	size_t m = 0;
	struct config_strbytelist* s;
	for(s = p; s; s = s->next)
		m += sizeof(*s) + getmem_str(s->str) + (s->str2?s->str2len:0);
	return m;
}

/** get memory used by ifs array */
static size_t
getmem_ifs(int numifs, char** ifs)
{
	size_t m = 0;
	int i;
	m += numifs * sizeof(char*);
	for(i=0; i<numifs; i++)
		m += getmem_str(ifs[i]);
	return m;
}

/** get memory for config_stub */
static size_t
getmem_config_stub(struct config_stub* p)
{
	size_t m = 0;
	struct config_stub* s;
	for(s = p; s; s = s->next)
		m += sizeof(*s) + getmem_str(s->name)
			+ getmem_config_strlist(s->hosts)
			+ getmem_config_strlist(s->addrs);
	return m;
}

/** get memory for config_auth */
static size_t
getmem_config_auth(struct config_auth* p)
{
	size_t m = 0;
	struct config_auth* s;
	for(s = p; s; s = s->next)
		m += sizeof(*s) + getmem_str(s->name)
			+ getmem_config_strlist(s->masters)
			+ getmem_config_strlist(s->urls)
			+ getmem_config_strlist(s->allow_notify)
			+ getmem_str(s->zonefile)
			+ s->rpz_taglistlen
			+ getmem_str(s->rpz_action_override)
			+ getmem_str(s->rpz_log_name)
			+ getmem_str(s->rpz_cname);
	return m;
}

/** get memory for config_view */
static size_t
getmem_config_view(struct config_view* p)
{
	size_t m = 0;
	struct config_view* s;
	for(s = p; s; s = s->next)
		m += sizeof(*s) + getmem_str(s->name)
			+ getmem_config_str2list(s->local_zones)
			+ getmem_config_strlist(s->local_data)
			+ getmem_config_strlist(s->local_zones_nodefault)
#ifdef USE_IPSET
			+ getmem_config_strlist(s->local_zones_ipset)
#endif
			+ getmem_config_str2list(s->respip_actions)
			+ getmem_config_str2list(s->respip_data);

	return m;
}

/** get memory used by config_file item, estimate */
static size_t
config_file_getmem(struct config_file* cfg)
{
	size_t m = 0;
	m += sizeof(*cfg);
	m += getmem_config_strlist(cfg->proxy_protocol_port);
	m += getmem_str(cfg->ssl_service_key);
	m += getmem_str(cfg->ssl_service_pem);
	m += getmem_str(cfg->tls_cert_bundle);
	m += getmem_config_strlist(cfg->tls_additional_port);
	m += getmem_config_strlist(cfg->tls_session_ticket_keys.first);
	m += getmem_str(cfg->tls_ciphers);
	m += getmem_str(cfg->tls_ciphersuites);
	m += getmem_str(cfg->http_endpoint);
	m += (cfg->outgoing_avail_ports?65536*sizeof(int):0);
	m += getmem_str(cfg->target_fetch_policy);
	m += getmem_str(cfg->if_automatic_ports);
	m += getmem_ifs(cfg->num_ifs, cfg->ifs);
	m += getmem_ifs(cfg->num_out_ifs, cfg->out_ifs);
	m += getmem_config_strlist(cfg->root_hints);
	m += getmem_config_stub(cfg->stubs);
	m += getmem_config_stub(cfg->forwards);
	m += getmem_config_auth(cfg->auths);
	m += getmem_config_view(cfg->views);
	m += getmem_config_strlist(cfg->donotqueryaddrs);
#ifdef CLIENT_SUBNET
	m += getmem_config_strlist(cfg->client_subnet);
	m += getmem_config_strlist(cfg->client_subnet_zone);
#endif
	m += getmem_config_str2list(cfg->acls);
	m += getmem_config_str2list(cfg->tcp_connection_limits);
	m += getmem_config_strlist(cfg->caps_whitelist);
	m += getmem_config_strlist(cfg->private_address);
	m += getmem_config_strlist(cfg->private_domain);
	m += getmem_str(cfg->chrootdir);
	m += getmem_str(cfg->username);
	m += getmem_str(cfg->directory);
	m += getmem_str(cfg->logfile);
	m += getmem_str(cfg->pidfile);
	m += getmem_str(cfg->log_identity);
	m += getmem_str(cfg->identity);
	m += getmem_str(cfg->version);
	m += getmem_str(cfg->http_user_agent);
	m += getmem_str(cfg->nsid_cfg_str);
	m += (cfg->nsid?cfg->nsid_len:0);
	m += getmem_str(cfg->module_conf);
	m += getmem_config_strlist(cfg->trust_anchor_file_list);
	m += getmem_config_strlist(cfg->trust_anchor_list);
	m += getmem_config_strlist(cfg->auto_trust_anchor_file_list);
	m += getmem_config_strlist(cfg->trusted_keys_file_list);
	m += getmem_config_strlist(cfg->domain_insecure);
	m += getmem_str(cfg->val_nsec3_key_iterations);
	m += getmem_config_str2list(cfg->local_zones);
	m += getmem_config_strlist(cfg->local_zones_nodefault);
#ifdef USE_IPSET
	m += getmem_config_strlist(cfg->local_zones_ipset);
#endif
	m += getmem_config_strlist(cfg->local_data);
	m += getmem_config_str3list(cfg->local_zone_overrides);
	m += getmem_config_strbytelist(cfg->local_zone_tags);
	m += getmem_config_strbytelist(cfg->acl_tags);
	m += getmem_config_str3list(cfg->acl_tag_actions);
	m += getmem_config_str3list(cfg->acl_tag_datas);
	m += getmem_config_str2list(cfg->acl_view);
	m += getmem_config_str2list(cfg->interface_actions);
	m += getmem_config_strbytelist(cfg->interface_tags);
	m += getmem_config_str3list(cfg->interface_tag_actions);
	m += getmem_config_str3list(cfg->interface_tag_datas);
	m += getmem_config_str2list(cfg->interface_view);
	m += getmem_config_strbytelist(cfg->respip_tags);
	m += getmem_config_str2list(cfg->respip_actions);
	m += getmem_config_str2list(cfg->respip_data);
	m += getmem_ifs(cfg->num_tags, cfg->tagname);
	m += getmem_config_strlist(cfg->control_ifs.first);
	m += getmem_str(cfg->server_key_file);
	m += getmem_str(cfg->server_cert_file);
	m += getmem_str(cfg->control_key_file);
	m += getmem_str(cfg->control_cert_file);
	m += getmem_config_strlist(cfg->python_script);
	m += getmem_config_strlist(cfg->dynlib_file);
	m += getmem_str(cfg->dns64_prefix);
	m += getmem_config_strlist(cfg->dns64_ignore_aaaa);
	m += getmem_str(cfg->nat64_prefix);
	m += getmem_str(cfg->dnstap_socket_path);
	m += getmem_str(cfg->dnstap_ip);
	m += getmem_str(cfg->dnstap_tls_server_name);
	m += getmem_str(cfg->dnstap_tls_cert_bundle);
	m += getmem_str(cfg->dnstap_tls_client_key_file);
	m += getmem_str(cfg->dnstap_tls_client_cert_file);
	m += getmem_str(cfg->dnstap_identity);
	m += getmem_str(cfg->dnstap_version);
	m += getmem_config_str2list(cfg->ratelimit_for_domain);
	m += getmem_config_str2list(cfg->ratelimit_below_domain);
	m += getmem_config_str2list(cfg->edns_client_strings);
	m += getmem_str(cfg->dnscrypt_provider);
	m += getmem_config_strlist(cfg->dnscrypt_secret_key);
	m += getmem_config_strlist(cfg->dnscrypt_provider_cert);
	m += getmem_config_strlist(cfg->dnscrypt_provider_cert_rotated);
#ifdef USE_IPSECMOD
	m += getmem_config_strlist(cfg->ipsecmod_whitelist);
	m += getmem_str(cfg->ipsecmod_hook);
#endif
#ifdef USE_CACHEDB
	m += getmem_str(cfg->cachedb_backend);
	m += getmem_str(cfg->cachedb_secret);
#ifdef USE_REDIS
	m += getmem_str(cfg->redis_server_host);
	m += getmem_str(cfg->redis_server_path);
	m += getmem_str(cfg->redis_server_password);
#endif
#endif
#ifdef USE_IPSET
	m += getmem_str(cfg->ipset_name_v4);
	m += getmem_str(cfg->ipset_name_v6);
#endif
	return m;
}

/** fast reload thread, print memory used by construct of items. */
static int
fr_printmem(struct fast_reload_thread* fr,
	struct config_file* newcfg, struct fast_reload_construct* ct)
{
	size_t mem = 0;
	if(fr_poll_for_quit(fr))
		return 1;
	mem += views_get_mem(ct->views);
	mem += forwards_get_mem(ct->fwds);
	mem += hints_get_mem(ct->hints);
	mem += sizeof(*ct->oldcfg);
	mem += config_file_getmem(newcfg);

	if(!fr_output_printf(fr, "memory use %d bytes\n", (int)mem))
		return 0;
	fr_send_notification(fr, fast_reload_notification_printout);

	return 1;
}

/** fast reload thread, construct from config the new items */
static int
fr_construct_from_config(struct fast_reload_thread* fr,
	struct config_file* newcfg, struct fast_reload_construct* ct)
{
	if(!(ct->views = views_create())) {
		fr_construct_clear(ct);
		return 0;
	}
	if(!views_apply_cfg(ct->views, newcfg)) {
		fr_construct_clear(ct);
		return 0;
	}
	if(fr_poll_for_quit(fr))
		return 1;

	if(!(ct->fwds = forwards_create())) {
		fr_construct_clear(ct);
		return 0;
	}
	if(!forwards_apply_cfg(ct->fwds, newcfg)) {
		fr_construct_clear(ct);
		return 0;
	}
	if(fr_poll_for_quit(fr))
		return 1;

	if(!(ct->hints = hints_create()))
		return 0;
	if(!hints_apply_cfg(ct->hints, newcfg)) {
		fr_construct_clear(ct);
		return 0;
	}
	if(fr_poll_for_quit(fr))
		return 1;

	if(!(ct->oldcfg = (struct config_file*)calloc(1,
		sizeof(*ct->oldcfg)))) {
		fr_construct_clear(ct);
		log_err("out of memory");
		return 0;
	}
	if(fr->fr_verb >= 2) {
		if(!fr_printmem(fr, newcfg, ct))
			return 0;
	}
	return 1;
}

/** fast reload thread, finish timers */
static int
fr_finish_time(struct fast_reload_thread* fr, struct timeval* time_start,
	struct timeval* time_read, struct timeval* time_construct,
	struct timeval* time_reload, struct timeval* time_end)
{
	struct timeval total, readtime, constructtime, reloadtime, deletetime;
	if(gettimeofday(time_end, NULL) < 0)
		log_err("gettimeofday: %s", strerror(errno));

	timeval_subtract(&total, time_end, time_start);
	timeval_subtract(&readtime, time_read, time_start);
	timeval_subtract(&constructtime, time_construct, time_read);
	timeval_subtract(&reloadtime, time_reload, time_construct);
	timeval_subtract(&deletetime, time_end, time_reload);
	if(!fr_output_printf(fr, "read disk  %3d.%6.6ds\n",
		(int)readtime.tv_sec, (int)readtime.tv_usec))
		return 0;
	if(!fr_output_printf(fr, "construct  %3d.%6.6ds\n",
		(int)constructtime.tv_sec, (int)constructtime.tv_usec))
		return 0;
	if(!fr_output_printf(fr, "reload     %3d.%6.6ds\n",
		(int)reloadtime.tv_sec, (int)reloadtime.tv_usec))
		return 0;
	if(!fr_output_printf(fr, "deletes    %3d.%6.6ds\n",
		(int)deletetime.tv_sec, (int)deletetime.tv_usec))
		return 0;
	if(!fr_output_printf(fr, "total time %3d.%6.6ds\n", (int)total.tv_sec,
		(int)total.tv_usec))
		return 0;
	fr_send_notification(fr, fast_reload_notification_printout);
	return 1;
}

#ifdef ATOMIC_POINTER_LOCK_FREE
/** Fast reload thread, if atomics are available, copy the config items
 * one by one with atomic store operations. */
static void
fr_atomic_copy_cfg(struct config_file* oldcfg, struct config_file* cfg,
	struct config_file* newcfg)
{
#define COPY_VAR(var) oldcfg->var = cfg->var; atomic_store(&cfg->var, newcfg->var); newcfg->var = 0;
	/* If config file items are missing from this list, they are
	 * not updated by fast-reload +p. */
	/* For missing items, the oldcfg item is not updated, still NULL,
	 * and the cfg stays the same. The newcfg item is untouched.
	 * The newcfg item is then deleted later. */
	/* Items that need synchronisation are omitted from the list.
	 * Use fast-reload without +p to update them together. */
	COPY_VAR(verbosity);
	COPY_VAR(stat_interval);
	COPY_VAR(stat_cumulative);
	COPY_VAR(stat_extended);
	COPY_VAR(stat_inhibit_zero);
	COPY_VAR(num_threads);
	COPY_VAR(port);
	COPY_VAR(do_ip4);
	COPY_VAR(do_ip6);
	COPY_VAR(do_nat64);
	COPY_VAR(prefer_ip4);
	COPY_VAR(prefer_ip6);
	COPY_VAR(do_udp);
	COPY_VAR(do_tcp);
	COPY_VAR(max_reuse_tcp_queries);
	COPY_VAR(tcp_reuse_timeout);
	COPY_VAR(tcp_auth_query_timeout);
	COPY_VAR(tcp_upstream);
	COPY_VAR(udp_upstream_without_downstream);
	COPY_VAR(tcp_mss);
	COPY_VAR(outgoing_tcp_mss);
	COPY_VAR(tcp_idle_timeout);
	COPY_VAR(do_tcp_keepalive);
	COPY_VAR(tcp_keepalive_timeout);
	COPY_VAR(sock_queue_timeout);
	COPY_VAR(proxy_protocol_port);
	COPY_VAR(ssl_service_key);
	COPY_VAR(ssl_service_pem);
	COPY_VAR(ssl_port);
	COPY_VAR(ssl_upstream);
	COPY_VAR(tls_cert_bundle);
	COPY_VAR(tls_win_cert);
	COPY_VAR(tls_additional_port);
	/* The first is used to walk throught the list but last is
	 * only used during config read. */
	COPY_VAR(tls_session_ticket_keys.first);
	COPY_VAR(tls_session_ticket_keys.last);
	COPY_VAR(tls_ciphers);
	COPY_VAR(tls_ciphersuites);
	COPY_VAR(tls_use_sni);
	COPY_VAR(https_port);
	COPY_VAR(http_endpoint);
	COPY_VAR(http_max_streams);
	COPY_VAR(http_query_buffer_size);
	COPY_VAR(http_response_buffer_size);
	COPY_VAR(http_nodelay);
	COPY_VAR(http_notls_downstream);
	COPY_VAR(outgoing_num_ports);
	COPY_VAR(outgoing_num_tcp);
	COPY_VAR(incoming_num_tcp);
	COPY_VAR(outgoing_avail_ports);
	COPY_VAR(edns_buffer_size);
	COPY_VAR(stream_wait_size);
	COPY_VAR(msg_buffer_size);
	COPY_VAR(msg_cache_size);
	COPY_VAR(msg_cache_slabs);
	COPY_VAR(num_queries_per_thread);
	COPY_VAR(jostle_time);
	COPY_VAR(rrset_cache_size);
	COPY_VAR(rrset_cache_slabs);
	COPY_VAR(host_ttl);
	COPY_VAR(infra_cache_slabs);
	COPY_VAR(infra_cache_numhosts);
	COPY_VAR(infra_cache_min_rtt);
	COPY_VAR(infra_cache_max_rtt);
	COPY_VAR(infra_keep_probing);
	COPY_VAR(delay_close);
	COPY_VAR(udp_connect);
	COPY_VAR(target_fetch_policy);
	COPY_VAR(fast_server_permil);
	COPY_VAR(fast_server_num);
	COPY_VAR(if_automatic);
	COPY_VAR(if_automatic_ports);
	COPY_VAR(so_rcvbuf);
	COPY_VAR(so_sndbuf);
	COPY_VAR(so_reuseport);
	COPY_VAR(ip_transparent);
	COPY_VAR(ip_freebind);
	COPY_VAR(ip_dscp);
	/* Not copied because the length and items could then not match.
	   num_ifs, ifs, num_out_ifs, out_ifs
	*/
	COPY_VAR(root_hints);
	COPY_VAR(stubs);
	COPY_VAR(forwards);
	COPY_VAR(auths);
	COPY_VAR(views);
	COPY_VAR(donotqueryaddrs);
#ifdef CLIENT_SUBNET
	COPY_VAR(client_subnet);
	COPY_VAR(client_subnet_zone);
	COPY_VAR(client_subnet_opcode);
	COPY_VAR(client_subnet_always_forward);
	COPY_VAR(max_client_subnet_ipv4);
	COPY_VAR(max_client_subnet_ipv6);
	COPY_VAR(min_client_subnet_ipv4);
	COPY_VAR(min_client_subnet_ipv6);
	COPY_VAR(max_ecs_tree_size_ipv4);
	COPY_VAR(max_ecs_tree_size_ipv6);
#endif
	COPY_VAR(acls);
	COPY_VAR(donotquery_localhost);
	COPY_VAR(tcp_connection_limits);
	COPY_VAR(harden_short_bufsize);
	COPY_VAR(harden_large_queries);
	COPY_VAR(harden_glue);
	COPY_VAR(harden_dnssec_stripped);
	COPY_VAR(harden_below_nxdomain);
	COPY_VAR(harden_referral_path);
	COPY_VAR(harden_algo_downgrade);
	COPY_VAR(harden_unknown_additional);
	COPY_VAR(use_caps_bits_for_id);
	COPY_VAR(caps_whitelist);
	COPY_VAR(private_address);
	COPY_VAR(private_domain);
	COPY_VAR(unwanted_threshold);
	COPY_VAR(max_ttl);
	COPY_VAR(min_ttl);
	COPY_VAR(max_negative_ttl);
	COPY_VAR(prefetch);
	COPY_VAR(prefetch_key);
	COPY_VAR(deny_any);
	COPY_VAR(chrootdir);
	COPY_VAR(username);
	COPY_VAR(directory);
	COPY_VAR(logfile);
	COPY_VAR(pidfile);
	COPY_VAR(use_syslog);
	COPY_VAR(log_time_ascii);
	COPY_VAR(log_queries);
	COPY_VAR(log_replies);
	COPY_VAR(log_tag_queryreply);
	COPY_VAR(log_local_actions);
	COPY_VAR(log_servfail);
	COPY_VAR(log_identity);
	COPY_VAR(log_destaddr);
	COPY_VAR(hide_identity);
	COPY_VAR(hide_version);
	COPY_VAR(hide_trustanchor);
	COPY_VAR(hide_http_user_agent);
	COPY_VAR(identity);
	COPY_VAR(version);
	COPY_VAR(http_user_agent);
	COPY_VAR(nsid_cfg_str);
	/* Not copied because the length and items could then not match.
	nsid;
	nsid_len;
	*/
	COPY_VAR(module_conf);
	COPY_VAR(trust_anchor_file_list);
	COPY_VAR(trust_anchor_list);
	COPY_VAR(auto_trust_anchor_file_list);
	COPY_VAR(trusted_keys_file_list);
	COPY_VAR(domain_insecure);
	COPY_VAR(trust_anchor_signaling);
	COPY_VAR(root_key_sentinel);
	COPY_VAR(val_date_override);
	COPY_VAR(val_sig_skew_min);
	COPY_VAR(val_sig_skew_max);
	COPY_VAR(val_max_restart);
	COPY_VAR(bogus_ttl);
	COPY_VAR(val_clean_additional);
	COPY_VAR(val_log_level);
	COPY_VAR(val_log_squelch);
	COPY_VAR(val_permissive_mode);
	COPY_VAR(aggressive_nsec);
	COPY_VAR(ignore_cd);
	COPY_VAR(disable_edns_do);
	COPY_VAR(serve_expired);
	COPY_VAR(serve_expired_ttl);
	COPY_VAR(serve_expired_ttl_reset);
	COPY_VAR(serve_expired_reply_ttl);
	COPY_VAR(serve_expired_client_timeout);
	COPY_VAR(ede_serve_expired);
	COPY_VAR(serve_original_ttl);
	COPY_VAR(val_nsec3_key_iterations);
	COPY_VAR(zonemd_permissive_mode);
	COPY_VAR(add_holddown);
	COPY_VAR(del_holddown);
	COPY_VAR(keep_missing);
	COPY_VAR(permit_small_holddown);
	COPY_VAR(key_cache_size);
	COPY_VAR(key_cache_slabs);
	COPY_VAR(neg_cache_size);
	COPY_VAR(local_zones);
	COPY_VAR(local_zones_nodefault);
#ifdef USE_IPSET
	COPY_VAR(local_zones_ipset);
#endif
	COPY_VAR(local_zones_disable_default);
	COPY_VAR(local_data);
	COPY_VAR(local_zone_overrides);
	COPY_VAR(unblock_lan_zones);
	COPY_VAR(insecure_lan_zones);
	/* These reference tags
	COPY_VAR(local_zone_tags);
	COPY_VAR(acl_tags);
	COPY_VAR(acl_tag_actions);
	COPY_VAR(acl_tag_datas);
	*/
	COPY_VAR(acl_view);
	COPY_VAR(interface_actions);
	/* These reference tags
	COPY_VAR(interface_tags);
	COPY_VAR(interface_tag_actions);
	COPY_VAR(interface_tag_datas);
	*/
	COPY_VAR(interface_view);
	/* This references tags
	COPY_VAR(respip_tags);
	*/
	COPY_VAR(respip_actions);
	COPY_VAR(respip_data);
	/* Not copied because the length and items could then not match.
	 * also the respip module keeps a pointer to the array in its state.
	   tagname, num_tags
	*/
	COPY_VAR(remote_control_enable);
	/* The first is used to walk throught the list but last is
	 * only used during config read. */
	COPY_VAR(control_ifs.first);
	COPY_VAR(control_ifs.last);
	COPY_VAR(control_use_cert);
	COPY_VAR(control_port);
	COPY_VAR(server_key_file);
	COPY_VAR(server_cert_file);
	COPY_VAR(control_key_file);
	COPY_VAR(control_cert_file);
	COPY_VAR(python_script);
	COPY_VAR(dynlib_file);
	COPY_VAR(use_systemd);
	COPY_VAR(do_daemonize);
	COPY_VAR(minimal_responses);
	COPY_VAR(rrset_roundrobin);
	COPY_VAR(unknown_server_time_limit);
	COPY_VAR(max_udp_size);
	COPY_VAR(dns64_prefix);
	COPY_VAR(dns64_synthall);
	COPY_VAR(dns64_ignore_aaaa);
	COPY_VAR(nat64_prefix);
	COPY_VAR(dnstap);
	COPY_VAR(dnstap_bidirectional);
	COPY_VAR(dnstap_socket_path);
	COPY_VAR(dnstap_ip);
	COPY_VAR(dnstap_tls);
	COPY_VAR(dnstap_tls_server_name);
	COPY_VAR(dnstap_tls_cert_bundle);
	COPY_VAR(dnstap_tls_client_key_file);
	COPY_VAR(dnstap_tls_client_cert_file);
	COPY_VAR(dnstap_send_identity);
	COPY_VAR(dnstap_send_version);
	COPY_VAR(dnstap_identity);
	COPY_VAR(dnstap_version);
	COPY_VAR(dnstap_log_resolver_query_messages);
	COPY_VAR(dnstap_log_resolver_response_messages);
	COPY_VAR(dnstap_log_client_query_messages);
	COPY_VAR(dnstap_log_client_response_messages);
	COPY_VAR(dnstap_log_forwarder_query_messages);
	COPY_VAR(dnstap_log_forwarder_response_messages);
	COPY_VAR(disable_dnssec_lame_check);
	COPY_VAR(ip_ratelimit);
	COPY_VAR(ip_ratelimit_cookie);
	COPY_VAR(ip_ratelimit_slabs);
	COPY_VAR(ip_ratelimit_size);
	COPY_VAR(ip_ratelimit_factor);
	COPY_VAR(ip_ratelimit_backoff);
	COPY_VAR(ratelimit);
	COPY_VAR(ratelimit_slabs);
	COPY_VAR(ratelimit_size);
	COPY_VAR(ratelimit_for_domain);
	COPY_VAR(ratelimit_below_domain);
	COPY_VAR(ratelimit_factor);
	COPY_VAR(ratelimit_backoff);
	COPY_VAR(outbound_msg_retry);
	COPY_VAR(max_sent_count);
	COPY_VAR(max_query_restarts);
	COPY_VAR(qname_minimisation);
	COPY_VAR(qname_minimisation_strict);
	COPY_VAR(shm_enable);
	COPY_VAR(shm_key);
	COPY_VAR(edns_client_strings);
	COPY_VAR(edns_client_string_opcode);
	COPY_VAR(dnscrypt);
	COPY_VAR(dnscrypt_port);
	COPY_VAR(dnscrypt_provider);
	COPY_VAR(dnscrypt_secret_key);
	COPY_VAR(dnscrypt_provider_cert);
	COPY_VAR(dnscrypt_provider_cert_rotated);
	COPY_VAR(dnscrypt_shared_secret_cache_size);
	COPY_VAR(dnscrypt_shared_secret_cache_slabs);
	COPY_VAR(dnscrypt_nonce_cache_size);
	COPY_VAR(dnscrypt_nonce_cache_slabs);
	COPY_VAR(pad_responses);
	COPY_VAR(pad_responses_block_size);
	COPY_VAR(pad_queries);
	COPY_VAR(pad_queries_block_size);
#ifdef USE_IPSECMOD
	COPY_VAR(ipsecmod_enabled);
	COPY_VAR(ipsecmod_whitelist);
	COPY_VAR(ipsecmod_hook);
	COPY_VAR(ipsecmod_ignore_bogus);
	COPY_VAR(ipsecmod_max_ttl);
	COPY_VAR(ipsecmod_strict);
#endif
#ifdef USE_CACHEDB
	COPY_VAR(cachedb_backend);
	COPY_VAR(cachedb_secret);
	COPY_VAR(cachedb_no_store);
#ifdef USE_REDIS
	COPY_VAR(redis_server_host);
	COPY_VAR(redis_server_port);
	COPY_VAR(redis_server_path);
	COPY_VAR(redis_server_password);
	COPY_VAR(redis_timeout);
	COPY_VAR(redis_expire_records);
	COPY_VAR(redis_logical_db);
#endif
#endif
	COPY_VAR(do_answer_cookie);
	/* Not copied because the length and content could then not match.
	   cookie_secret[40], cookie_secret_len
	*/
#ifdef USE_IPSET
	COPY_VAR(ipset_name_v4);
	COPY_VAR(ipset_name_v6);
#endif
	COPY_VAR(ede);
}
#endif /* ATOMIC_POINTER_LOCK_FREE */

/** fast reload thread, reload config with putting the new config items
 * in place and swapping out the old items. */
static int
fr_reload_config(struct fast_reload_thread* fr, struct config_file* newcfg,
	struct fast_reload_construct* ct)
{
	struct daemon* daemon = fr->worker->daemon;
	struct module_env* env = daemon->env;

	/* These are constructed in the fr_construct_from_config routine. */
	log_assert(ct->oldcfg);
	log_assert(ct->fwds);
	log_assert(ct->hints);

	/* Grab big locks to satisfy lock conditions. */
	lock_rw_wrlock(&ct->views->lock);
	lock_rw_wrlock(&daemon->views->lock);
	lock_rw_wrlock(&ct->fwds->lock);
	lock_rw_wrlock(&ct->hints->lock);
	lock_rw_wrlock(&env->fwds->lock);
	lock_rw_wrlock(&env->hints->lock);

#ifdef ATOMIC_POINTER_LOCK_FREE
	if(fr->fr_nopause) {
		fr_atomic_copy_cfg(ct->oldcfg, env->cfg, newcfg);
	} else {
#endif
		/* Store old config elements. */
		*ct->oldcfg = *env->cfg;
		/* Insert new config elements. */
		*env->cfg = *newcfg;
#ifdef ATOMIC_POINTER_LOCK_FREE
	}
#endif

	if(env->cfg->log_identity || ct->oldcfg->log_identity) {
		/* pick up new log_identity string to use for log output. */
		log_ident_set_or_default(env->cfg->log_identity);
	}
	/* the newcfg elements are in env->cfg, so should not be freed here. */
#ifdef ATOMIC_POINTER_LOCK_FREE
	/* if used, the routine that copies the config has zeroed items. */
	if(!fr->fr_nopause)
#endif
		memset(newcfg, 0, sizeof(*newcfg));

	/* Quickly swap the tree roots themselves with the already allocated
	 * elements. This is a quick swap operation on the pointer.
	 * The other threads are stopped and locks are held, so that a
	 * consistent view of the configuration, before, and after, exists
	 * towards the state machine for query resolution. */
	forwards_swap_tree(env->fwds, ct->fwds);
	hints_swap_tree(env->hints, ct->hints);
	views_swap_tree(daemon->views, ct->views);

	/* Set globals with new config. */
	config_apply(env->cfg);

	lock_rw_unlock(&ct->views->lock);
	lock_rw_unlock(&daemon->views->lock);
	lock_rw_unlock(&env->fwds->lock);
	lock_rw_unlock(&env->hints->lock);
	lock_rw_unlock(&ct->fwds->lock);
	lock_rw_unlock(&ct->hints->lock);

	return 1;
}

/** fast reload, poll for ack incoming. */
static void
fr_poll_for_ack(struct fast_reload_thread* fr)
{
	int loopexit = 0, bcount = 0;
	uint32_t cmd;
	ssize_t ret;

	if(fr->need_to_quit)
		return;
	/* Is there data? */
	if(!sock_poll_timeout(fr->commpair[1], -1, 1, 0, NULL)) {
		log_err("fr_poll_for_ack: poll failed");
		return;
	}

	/* Read the data */
	while(1) {
		if(++loopexit > IPC_LOOP_MAX) {
			log_err("fr_poll_for_ack: recv loops %s",
				sock_strerror(errno));
			return;
		}
		ret = recv(fr->commpair[1], ((char*)&cmd)+bcount,
			sizeof(cmd)-bcount, 0);
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
				errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
				|| errno == EWOULDBLOCK
#  endif
#else
				WSAGetLastError() == WSAEINTR ||
				WSAGetLastError() == WSAEINPROGRESS ||
				WSAGetLastError() == WSAEWOULDBLOCK
#endif
				)
				continue; /* Try again. */
			log_err("fr_poll_for_ack: recv: %s",
				sock_strerror(errno));
			return;
		} else if(ret+(ssize_t)bcount != sizeof(cmd)) {
			bcount += ret;
			if((size_t)bcount < sizeof(cmd))
				continue;
		}
		break;
	}
	if(cmd == fast_reload_notification_exit) {
		fr->need_to_quit = 1;
		verbose(VERB_ALGO, "fast reload wait for ack: "
			"exit notification received");
		return;
	}
	if(cmd != fast_reload_notification_reload_ack) {
		verbose(VERB_ALGO, "fast reload wait for ack: "
			"wrong notification %d", (int)cmd);
	}
}

/** fast reload thread, reload ipc communication to stop and start threads. */
static int
fr_reload_ipc(struct fast_reload_thread* fr, struct config_file* newcfg,
	struct fast_reload_construct* ct)
{
	int result = 1;
	if(!fr->fr_nopause) {
		fr_send_notification(fr, fast_reload_notification_reload_stop);
		fr_poll_for_ack(fr);
	}
	if(!fr_reload_config(fr, newcfg, ct)) {
		result = 0;
	}
	if(!fr->fr_nopause) {
		fr_send_notification(fr, fast_reload_notification_reload_start);
	}
	return result;
}

/** fast reload thread, load config */
static int
fr_load_config(struct fast_reload_thread* fr, struct timeval* time_read,
	struct timeval* time_construct, struct timeval* time_reload)
{
	struct fast_reload_construct ct;
	struct config_file* newcfg = NULL;
	memset(&ct, 0, sizeof(ct));

	/* Read file. */
	if(!fr_read_config(fr, &newcfg))
		return 0;
	if(gettimeofday(time_read, NULL) < 0)
		log_err("gettimeofday: %s", strerror(errno));
	if(fr_poll_for_quit(fr)) {
		config_delete(newcfg);
		return 1;
	}

	/* Construct items. */
	if(!fr_construct_from_config(fr, newcfg, &ct)) {
		config_delete(newcfg);
		if(!fr_output_printf(fr, "Could not construct from the "
			"config, check for errors with unbound-checkconf, or "
			"out of memory.\n"))
			return 0;
		fr_send_notification(fr, fast_reload_notification_printout);
		return 0;
	}
	if(gettimeofday(time_construct, NULL) < 0)
		log_err("gettimeofday: %s", strerror(errno));
	if(fr_poll_for_quit(fr)) {
		config_delete(newcfg);
		fr_construct_clear(&ct);
		return 1;
	}

	/* Reload server. */
	if(!fr_reload_ipc(fr, newcfg, &ct)) {
		config_delete(newcfg);
		fr_construct_clear(&ct);
		if(!fr_output_printf(fr, "error: reload failed\n"))
			return 0;
		fr_send_notification(fr, fast_reload_notification_printout);
		return 0;
	}
	if(gettimeofday(time_reload, NULL) < 0)
		log_err("gettimeofday: %s", strerror(errno));

	/* Delete old. */
	if(fr_poll_for_quit(fr)) {
		config_delete(newcfg);
		fr_construct_clear(&ct);
		return 1;
	}
	if(fr->fr_nopause) {
		/* Poll every thread, with a no-work poll item over the
		 * command pipe. This makes the worker thread surely move
		 * to deal with that event, and thus the thread is no longer
		 * holding, eg. a string item from the old config struct.
		 * And then the old config struct can safely be deleted.
		 * Only needed when nopause is used, because without that
		 * the worker threads are already waiting on a command pipe
		 * item. This nopause command pipe item does not take work,
		 * it returns immediately, so it does not delay the workers.
		 * They can be polled one at a time. But its processing causes
		 * the worker to have released data items from old config. */
		fr_send_notification(fr,
			fast_reload_notification_reload_nopause_poll);
		fr_poll_for_ack(fr);
	}
	config_delete(newcfg);
	fr_construct_clear(&ct);
	return 1;
}

/** fast reload thread. the thread main function */
static void* fast_reload_thread_main(void* arg)
{
	struct fast_reload_thread* fast_reload_thread = (struct fast_reload_thread*)arg;
	struct timeval time_start, time_read, time_construct, time_reload,
		time_end;
	log_thread_set(&fast_reload_thread->threadnum);

	verbose(VERB_ALGO, "start fast reload thread");
	if(fast_reload_thread->fr_verb >= 1) {
		fr_init_time(&time_start, &time_read, &time_construct,
			&time_reload, &time_end);
		if(fr_poll_for_quit(fast_reload_thread))
			goto done;
	}

	/* print output to the client */
	if(fast_reload_thread->fr_verb >= 1) {
		if(!fr_output_printf(fast_reload_thread, "thread started\n"))
			goto done_error;
		fr_send_notification(fast_reload_thread,
			fast_reload_notification_printout);
		if(fr_poll_for_quit(fast_reload_thread))
			goto done;
	}

	if(!fr_load_config(fast_reload_thread, &time_read, &time_construct,
		&time_reload))
		goto done_error;
	if(fr_poll_for_quit(fast_reload_thread))
		goto done;

	if(fast_reload_thread->fr_verb >= 1) {
		if(!fr_finish_time(fast_reload_thread, &time_start, &time_read,
			&time_construct, &time_reload, &time_end))
			goto done_error;
		if(fr_poll_for_quit(fast_reload_thread))
			goto done;
	}

	if(!fr_output_printf(fast_reload_thread, "ok\n"))
		goto done_error;
	fr_send_notification(fast_reload_thread,
		fast_reload_notification_printout);
	verbose(VERB_ALGO, "stop fast reload thread");
	/* If this is not an exit due to quit earlier, send regular done. */
	if(!fast_reload_thread->need_to_quit)
		fr_send_notification(fast_reload_thread,
			fast_reload_notification_done);
	/* If during the fast_reload_notification_done send,
	 * fast_reload_notification_exit was received, ack it. If the
	 * thread is exiting due to quit received earlier, also ack it.*/
done:
	if(fast_reload_thread->need_to_quit)
		fr_send_notification(fast_reload_thread,
			fast_reload_notification_exited);
	return NULL;
done_error:
	verbose(VERB_ALGO, "stop fast reload thread with done_error");
	fr_send_notification(fast_reload_thread,
		fast_reload_notification_done_error);
	return NULL;
}
#endif /* !THREADS_DISABLED */

/** create a socketpair for bidirectional communication, false on failure */
static int
create_socketpair(int* pair, struct ub_randstate* rand)
{
#ifndef USE_WINSOCK
	if(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1) {
		log_err("socketpair: %s", strerror(errno));
		return 0;
	}
	(void)rand;
#else
	struct sockaddr_in addr, baddr, accaddr, connaddr;
	socklen_t baddrlen, accaddrlen, connaddrlen;
	uint8_t localhost[] = {127, 0, 0, 1};
	uint8_t nonce[16], recvnonce[16];
	size_t i;
	int lst, pollin_event, bcount, loopcount;
	int connect_poll_timeout = 200; /* msec to wait for connection */
	ssize_t ret;
	pair[0] = -1;
	pair[1] = -1;
	for(i=0; i<sizeof(nonce); i++) {
		nonce[i] = ub_random_max(rand, 256);
	}
	lst = socket(AF_INET, SOCK_STREAM, 0);
	if(lst == -1) {
		log_err("create_socketpair: socket: %s", sock_strerror(errno));
		return 0;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	memcpy(&addr.sin_addr, localhost, 4);
	if(bind(lst, (struct sockaddr*)&addr, (socklen_t)sizeof(addr))
		== -1) {
		log_err("create socketpair: bind: %s", sock_strerror(errno));
		sock_close(lst);
		return 0;
	}
	if(listen(lst, 12) == -1) {
		log_err("create socketpair: listen: %s", sock_strerror(errno));
		sock_close(lst);
		return 0;
	}

	pair[1] = socket(AF_INET, SOCK_STREAM, 0);
	if(pair[1] == -1) {
		log_err("create socketpair: socket: %s", sock_strerror(errno));
		sock_close(lst);
		return 0;
	}
	baddrlen = (socklen_t)sizeof(baddr);
	if(getsockname(lst, (struct sockaddr*)&baddr, &baddrlen) == -1) {
		log_err("create socketpair: getsockname: %s",
			sock_strerror(errno));
		sock_close(lst);
		sock_close(pair[1]);
		pair[1] = -1;
		return 0;
	}
	if(baddrlen > (socklen_t)sizeof(baddr)) {
		log_err("create socketpair: getsockname returned addr too big");
		sock_close(lst);
		sock_close(pair[1]);
		pair[1] = -1;
		return 0;
	}
	/* the socket is blocking */
	if(connect(pair[1], (struct sockaddr*)&baddr, baddrlen) == -1) {
		log_err("create socketpair: connect: %s",
			sock_strerror(errno));
		sock_close(lst);
		sock_close(pair[1]);
		pair[1] = -1;
		return 0;
	}
	if(!sock_poll_timeout(lst, connect_poll_timeout, 1, 0, &pollin_event)) {
		log_err("create socketpair: poll for accept failed: %s",
			sock_strerror(errno));
		sock_close(lst);
		sock_close(pair[1]);
		pair[1] = -1;
		return 0;
	}
	if(!pollin_event) {
		log_err("create socketpair: poll timeout for accept");
		sock_close(lst);
		sock_close(pair[1]);
		pair[1] = -1;
		return 0;
	}
	accaddrlen = (socklen_t)sizeof(accaddr);
	pair[0] = accept(lst, &accaddr, &accaddrlen);
	if(pair[0] == -1) {
		log_err("create socketpair: accept: %s", sock_strerror(errno));
		sock_close(lst);
		sock_close(pair[1]);
		pair[1] = -1;
		return 0;
	}
	if(accaddrlen > (socklen_t)sizeof(accaddr)) {
		log_err("create socketpair: accept returned addr too big");
		sock_close(lst);
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}
	if(accaddr.sin_family != AF_INET ||
	   memcmp(localhost, &accaddr.sin_addr, 4) != 0) {
		log_err("create socketpair: accept from wrong address");
		sock_close(lst);
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}
	connaddrlen = (socklen_t)sizeof(connaddr);
	if(getsockname(pair[1], (struct sockaddr*)&connaddr, &connaddrlen)
		== -1) {
		log_err("create socketpair: getsockname connectedaddr: %s",
			sock_strerror(errno));
		sock_close(lst);
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}
	if(connaddrlen > (socklen_t)sizeof(connaddr)) {
		log_err("create socketpair: getsockname connectedaddr returned addr too big");
		sock_close(lst);
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}
	if(connaddr.sin_family != AF_INET ||
	   memcmp(localhost, &connaddr.sin_addr, 4) != 0) {
		log_err("create socketpair: getsockname connectedaddr returned wrong address");
		sock_close(lst);
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}
	if(accaddr.sin_port != connaddr.sin_port) {
		log_err("create socketpair: accept from wrong port");
		sock_close(lst);
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}
	sock_close(lst);

	loopcount = 0;
	bcount = 0;
	while(1) {
		if(++loopcount > IPC_LOOP_MAX) {
			log_err("create socketpair: send failed due to loop");
			sock_close(pair[0]);
			sock_close(pair[1]);
			pair[0] = -1;
			pair[1] = -1;
			return 0;
		}
		ret = send(pair[1], nonce+bcount, sizeof(nonce)-bcount, 0);
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
				errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
				|| errno == EWOULDBLOCK
#  endif
#else
				WSAGetLastError() == WSAEINTR ||
				WSAGetLastError() == WSAEINPROGRESS ||
				WSAGetLastError() == WSAEWOULDBLOCK
#endif
				)
				continue; /* Try again. */
			log_err("create socketpair: send: %s", sock_strerror(errno));
			sock_close(pair[0]);
			sock_close(pair[1]);
			pair[0] = -1;
			pair[1] = -1;
			return 0;
		} else if(ret+(ssize_t)bcount != sizeof(nonce)) {
			bcount += ret;
			if((size_t)bcount < sizeof(nonce))
				continue;
		}
		break;
	}

	if(!sock_poll_timeout(pair[0], connect_poll_timeout, 1, 0, &pollin_event)) {
		log_err("create socketpair: poll failed: %s",
			sock_strerror(errno));
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}
	if(!pollin_event) {
		log_err("create socketpair: poll timeout for recv");
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}

	loopcount = 0;
	bcount = 0;
	while(1) {
		if(++loopcount > IPC_LOOP_MAX) {
			log_err("create socketpair: recv failed due to loop");
			sock_close(pair[0]);
			sock_close(pair[1]);
			pair[0] = -1;
			pair[1] = -1;
			return 0;
		}
		ret = recv(pair[0], recvnonce+bcount, sizeof(nonce)-bcount, 0);
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
				errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
				|| errno == EWOULDBLOCK
#  endif
#else
				WSAGetLastError() == WSAEINTR ||
				WSAGetLastError() == WSAEINPROGRESS ||
				WSAGetLastError() == WSAEWOULDBLOCK
#endif
				)
				continue; /* Try again. */
			log_err("create socketpair: recv: %s", sock_strerror(errno));
			sock_close(pair[0]);
			sock_close(pair[1]);
			pair[0] = -1;
			pair[1] = -1;
			return 0;
		} else if(ret == 0) {
			log_err("create socketpair: stream closed");
			sock_close(pair[0]);
			sock_close(pair[1]);
			pair[0] = -1;
			pair[1] = -1;
			return 0;
		} else if(ret+(ssize_t)bcount != sizeof(nonce)) {
			bcount += ret;
			if((size_t)bcount < sizeof(nonce))
				continue;
		}
		break;
	}

	if(memcmp(nonce, recvnonce, sizeof(nonce)) != 0) {
		log_err("create socketpair: recv wrong nonce");
		sock_close(pair[0]);
		sock_close(pair[1]);
		pair[0] = -1;
		pair[1] = -1;
		return 0;
	}
#endif
	return 1;
}

/** fast reload thread. setup the thread info */
static int
fast_reload_thread_setup(struct worker* worker, int fr_verb, int fr_nopause,
	int fr_drop_mesh)
{
	struct fast_reload_thread* fr;
	int numworkers = worker->daemon->num;
	worker->daemon->fast_reload_thread = (struct fast_reload_thread*)
		calloc(1, sizeof(*worker->daemon->fast_reload_thread));
	if(!worker->daemon->fast_reload_thread)
		return 0;
	fr = worker->daemon->fast_reload_thread;
	fr->fr_verb = fr_verb;
	fr->fr_nopause = fr_nopause;
	fr->fr_drop_mesh = fr_drop_mesh;
	worker->daemon->fast_reload_drop_mesh = fr->fr_drop_mesh;
	/* The thread id printed in logs, numworker+1 is the dnstap thread.
	 * This is numworkers+2. */
	fr->threadnum = numworkers+2;
	fr->commpair[0] = -1;
	fr->commpair[1] = -1;
	fr->commreload[0] = -1;
	fr->commreload[1] = -1;
	if(!create_socketpair(fr->commpair, worker->daemon->rand)) {
		free(fr);
		worker->daemon->fast_reload_thread = NULL;
		return 0;
	}
	fr->worker = worker;
	fr->fr_output = (struct config_strlist_head*)calloc(1,
		sizeof(*fr->fr_output));
	if(!fr->fr_output) {
		sock_close(fr->commpair[0]);
		sock_close(fr->commpair[1]);
		free(fr);
		worker->daemon->fast_reload_thread = NULL;
		return 0;
	}
	if(!create_socketpair(fr->commreload, worker->daemon->rand)) {
		sock_close(fr->commpair[0]);
		sock_close(fr->commpair[1]);
		sock_close(fr->commreload[0]);
		sock_close(fr->commreload[1]);
		free(fr->fr_output);
		free(fr);
		worker->daemon->fast_reload_thread = NULL;
		return 0;
	}
	lock_basic_init(&fr->fr_output_lock);
	lock_protect(&fr->fr_output_lock, fr->fr_output,
		sizeof(*fr->fr_output));
	return 1;
}

/** fast reload thread. desetup and delete the thread info. */
static void
fast_reload_thread_desetup(struct fast_reload_thread* fast_reload_thread)
{
	if(!fast_reload_thread)
		return;
	if(fast_reload_thread->service_event &&
		fast_reload_thread->service_event_is_added) {
		ub_event_del(fast_reload_thread->service_event);
		fast_reload_thread->service_event_is_added = 0;
	}
	if(fast_reload_thread->service_event)
		ub_event_free(fast_reload_thread->service_event);
	sock_close(fast_reload_thread->commpair[0]);
	sock_close(fast_reload_thread->commpair[1]);
	sock_close(fast_reload_thread->commreload[0]);
	sock_close(fast_reload_thread->commreload[1]);
	if(fast_reload_thread->printq) {
		fr_main_perform_printout(fast_reload_thread);
		/* If it is empty now, there is nothing to print on fd. */
		if(fr_printq_empty(fast_reload_thread->printq)) {
			fr_printq_delete(fast_reload_thread->printq);
		} else {
			/* Keep the printq around to printout the remaining
			 * text to the remote client. Until it is done, it
			 * sits on a list, that is in the daemon struct.
			 * The event can then spool the remaining text to the
			 * remote client and eventually delete itself from the
			 * callback. */
			fr_printq_list_insert(fast_reload_thread->printq,
				fast_reload_thread->worker->daemon);
			fast_reload_thread->printq = NULL;
		}
	}
	lock_basic_destroy(&fast_reload_thread->fr_output_lock);
	if(fast_reload_thread->fr_output) {
		config_delstrlist(fast_reload_thread->fr_output->first);
		free(fast_reload_thread->fr_output);
	}
	free(fast_reload_thread);
}

/**
 * Fast reload thread, send a command to the thread. Blocking on timeout.
 * It handles received input from the thread, if any is received.
 */
static void
fr_send_cmd_to(struct fast_reload_thread* fr,
	enum fast_reload_notification status, int check_cmds, int blocking)
{
	int outevent, loopexit = 0, bcount = 0;
	uint32_t cmd;
	ssize_t ret;
	verbose(VERB_ALGO, "send notification to fast reload thread: %s",
		fr_notification_to_string(status));
	cmd = status;
	while(1) {
		if(++loopexit > IPC_LOOP_MAX) {
			log_err("send notification to fast reload: could not send notification: loop");
			return;
		}
		if(check_cmds)
			fr_check_cmd_from_thread(fr);
		/* wait for socket to become writable */
		if(!sock_poll_timeout(fr->commpair[0],
			(blocking?-1:IPC_NOTIFICATION_WAIT),
			0, 1, &outevent)) {
			log_err("send notification to fast reload: poll failed");
			return;
		}
		if(!outevent)
			continue;
		ret = send(fr->commpair[0], ((char*)&cmd)+bcount,
			sizeof(cmd)-bcount, 0);
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
				errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
				|| errno == EWOULDBLOCK
#  endif
#else
				WSAGetLastError() == WSAEINTR ||
				WSAGetLastError() == WSAEINPROGRESS ||
				WSAGetLastError() == WSAEWOULDBLOCK
#endif
				)
				continue; /* Try again. */
			log_err("send notification to fast reload: send: %s",
				sock_strerror(errno));
			return;
		} else if(ret+(ssize_t)bcount != sizeof(cmd)) {
			bcount += ret;
			if((size_t)bcount < sizeof(cmd))
				continue;
		}
		break;
	}
}

/** Fast reload, the main thread handles that the fast reload thread has
 * exited. */
static void
fr_main_perform_done(struct fast_reload_thread* fr)
{
	struct worker* worker = fr->worker;
	verbose(VERB_ALGO, "join with fastreload thread");
	ub_thread_join(fr->tid);
	verbose(VERB_ALGO, "joined with fastreload thread");
	fast_reload_thread_desetup(fr);
	worker->daemon->fast_reload_thread = NULL;
}

/** Append strlist after strlist */
static void
cfg_strlist_append_listhead(struct config_strlist_head* list,
	struct config_strlist_head* more)
{
	if(!more->first)
		return;
	if(list->last)
		list->last->next = more->first;
	else
		list->first = more->first;
	list->last = more->last;
}

/** Fast reload, the remote control thread handles that the fast reload thread
 * has output to be printed, on the linked list that is locked. */
static void
fr_main_perform_printout(struct fast_reload_thread* fr)
{
	struct config_strlist_head out;

	/* Fetch the list of items to be printed */
	lock_basic_lock(&fr->fr_output_lock);
	out.first = fr->fr_output->first;
	out.last = fr->fr_output->last;
	fr->fr_output->first = NULL;
	fr->fr_output->last = NULL;
	lock_basic_unlock(&fr->fr_output_lock);

	if(!fr->printq || !fr->printq->client_cp) {
		/* There is no output socket, delete it. */
		config_delstrlist(out.first);
		return;
	}

	/* Put them on the output list, not locked because the list
	 * producer and consumer are both owned by the remote control thread,
	 * it moves the items to the list for printing in the event callback
	 * for the client_cp. */
	cfg_strlist_append_listhead(fr->printq->to_print, &out);

	/* Set the client_cp to output if not already */
	if(!fr->printq->client_cp->event_added)
		comm_point_listen_for_rw(fr->printq->client_cp, 0, 1);
}

/** fast reload, receive ack from workers that they are waiting, run
 * by the mainthr after sending them reload_stop. */
static void
fr_read_ack_from_workers(struct fast_reload_thread* fr)
{
	struct daemon* daemon = fr->worker->daemon;
	/* Every worker sends one byte, wait for num-1 bytes. */
	int count=0, total=daemon->num-1;
	while(count < total) {
		uint8_t r;
		ssize_t ret;
		ret = recv(fr->commreload[0], &r, 1, 0);
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
                                errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
                                || errno == EWOULDBLOCK
#  endif
#else
                                WSAGetLastError() == WSAEINTR ||
                                WSAGetLastError() == WSAEINPROGRESS ||
                                WSAGetLastError() == WSAEWOULDBLOCK
#endif
                                )
				continue; /* Try again */
			log_err("worker reload ack: recv failed: %s",
				sock_strerror(errno));
			return;
		}
		count++;
		verbose(VERB_ALGO, "worker reload ack from (uint8_t)%d",
			(int)r);
	}
}

/** fast reload, poll for reload_start in mainthr waiting on a notification
 * from the fast reload thread. */
static void
fr_poll_for_reload_start(struct fast_reload_thread* fr)
{
	int loopexit = 0, bcount = 0;
	uint32_t cmd;
	ssize_t ret;

	/* Is there data? */
	if(!sock_poll_timeout(fr->commpair[0], -1, 1, 0, NULL)) {
		log_err("fr_poll_for_reload_start: poll failed");
		return;
	}

	/* Read the data */
	while(1) {
		if(++loopexit > IPC_LOOP_MAX) {
			log_err("fr_poll_for_reload_start: recv loops %s",
				sock_strerror(errno));
			return;
		}
		ret = recv(fr->commpair[0], ((char*)&cmd)+bcount,
			sizeof(cmd)-bcount, 0);
		if(ret == -1) {
			if(
#ifndef USE_WINSOCK
				errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
				|| errno == EWOULDBLOCK
#  endif
#else
				WSAGetLastError() == WSAEINTR ||
				WSAGetLastError() == WSAEINPROGRESS ||
				WSAGetLastError() == WSAEWOULDBLOCK
#endif
				)
				continue; /* Try again. */
			log_err("fr_poll_for_reload_start: recv: %s",
				sock_strerror(errno));
			return;
		} else if(ret+(ssize_t)bcount != sizeof(cmd)) {
			bcount += ret;
			if((size_t)bcount < sizeof(cmd))
				continue;
		}
		break;
	}
	if(cmd != fast_reload_notification_reload_start) {
		verbose(VERB_ALGO, "fast reload wait for ack: "
			"wrong notification %d", (int)cmd);
	}
}


/** fast reload thread, handle reload_stop notification, send reload stop
 * to other threads over IPC and collect their ack. When that is done,
 * ack to the caller, the fast reload thread, and wait for it to send start. */
static void
fr_main_perform_reload_stop(struct fast_reload_thread* fr)
{
	struct daemon* daemon = fr->worker->daemon;
	int i;

	/* Send reload_stop to other threads. */
	for(i=0; i<daemon->num; i++) {
		if(i == fr->worker->thread_num)
			continue; /* Do not send to ourselves. */
		worker_send_cmd(daemon->workers[i], worker_cmd_reload_stop);
	}

	/* Wait for the other threads to ack. */
	fr_read_ack_from_workers(fr);

	/* Send ack to fast reload thread. */
	fr_send_cmd_to(fr, fast_reload_notification_reload_ack, 0, 1);

	/* Wait for reload_start from fast reload thread to resume. */
	fr_poll_for_reload_start(fr);

	/* Send reload_start to other threads */
	for(i=0; i<daemon->num; i++) {
		if(i == fr->worker->thread_num)
			continue; /* Do not send to ourselves. */
		worker_send_cmd(daemon->workers[i], worker_cmd_reload_start);
	}

	if(fr->worker->daemon->fast_reload_drop_mesh) {
		verbose(VERB_ALGO, "worker: drop mesh queries after reload");
		mesh_delete_all(fr->worker->env.mesh);
	}
	verbose(VERB_ALGO, "worker resume after reload");
}

/** Fast reload, the main thread performs the nopause poll. It polls every
 * other worker thread briefly over the command pipe ipc. The command takes
 * no time for the worker, it can return immediately. After that it sends
 * an acknowledgement to the fastreload thread. */
static void
fr_main_perform_reload_nopause_poll(struct fast_reload_thread* fr)
{
	struct daemon* daemon = fr->worker->daemon;
	int i;

	/* Send the reload_poll to other threads. They can respond
	 * one at a time. */
	for(i=0; i<daemon->num; i++) {
		if(i == fr->worker->thread_num)
			continue; /* Do not send to ourselves. */
		worker_send_cmd(daemon->workers[i], worker_cmd_reload_poll);
	}

	/* Wait for the other threads to ack. */
	fr_read_ack_from_workers(fr);

	/* Send ack to fast reload thread. */
	fr_send_cmd_to(fr, fast_reload_notification_reload_ack, 0, 1);
}

/** Fast reload, perform the command received from the fast reload thread */
static void
fr_main_perform_cmd(struct fast_reload_thread* fr,
	enum fast_reload_notification status)
{
	verbose(VERB_ALGO, "main perform fast reload status: %s",
		fr_notification_to_string(status));
	if(status == fast_reload_notification_printout) {
		fr_main_perform_printout(fr);
	} else if(status == fast_reload_notification_done ||
		status == fast_reload_notification_done_error ||
		status == fast_reload_notification_exited) {
		fr_main_perform_done(fr);
	} else if(status == fast_reload_notification_reload_stop) {
		fr_main_perform_reload_stop(fr);
	} else if(status == fast_reload_notification_reload_nopause_poll) {
		fr_main_perform_reload_nopause_poll(fr);
	} else {
		log_err("main received unknown status from fast reload: %d %s",
			(int)status, fr_notification_to_string(status));
	}
}

/** Fast reload, handle command from fast reload to the main thread. */
static void
fr_main_handle_cmd(struct fast_reload_thread* fr)
{
	enum fast_reload_notification status;
	ssize_t ret;
	ret = recv(fr->commpair[0],
		((char*)&fr->service_read_cmd)+fr->service_read_cmd_count,
		sizeof(fr->service_read_cmd)-fr->service_read_cmd_count, 0);
	if(ret == -1) {
		if(
#ifndef USE_WINSOCK
			errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
			|| errno == EWOULDBLOCK
#  endif
#else
			WSAGetLastError() == WSAEINTR ||
			WSAGetLastError() == WSAEINPROGRESS ||
			WSAGetLastError() == WSAEWOULDBLOCK
#endif
			)
			return; /* Continue later. */
		log_err("read cmd from fast reload thread, recv: %s",
			sock_strerror(errno));
		return;
	} else if(ret == 0) {
		verbose(VERB_ALGO, "closed connection from fast reload thread");
		fr->service_read_cmd_count = 0;
		/* handle this like an error */
		fr->service_read_cmd = fast_reload_notification_done_error;
	} else if(ret + (ssize_t)fr->service_read_cmd_count <
		(ssize_t)sizeof(fr->service_read_cmd)) {
		fr->service_read_cmd_count += ret;
		/* Continue later. */
		return;
	}
	status = fr->service_read_cmd;
	fr->service_read_cmd = 0;
	fr->service_read_cmd_count = 0;
	fr_main_perform_cmd(fr, status);
}

/** Fast reload, poll for and handle cmd from fast reload thread. */
static void
fr_check_cmd_from_thread(struct fast_reload_thread* fr)
{
	int inevent = 0;
	struct worker* worker = fr->worker;
	/* Stop in case the thread has exited, or there is no read event. */
	while(worker->daemon->fast_reload_thread) {
		if(!sock_poll_timeout(fr->commpair[0], 0, 1, 0, &inevent)) {
			log_err("check for cmd from fast reload thread: "
				"poll failed");
			return;
		}
		if(!inevent)
			return;
		fr_main_handle_cmd(fr);
	}
}

void fast_reload_service_cb(int ATTR_UNUSED(fd), short ATTR_UNUSED(bits),
	void* arg)
{
	struct fast_reload_thread* fast_reload_thread =
		(struct fast_reload_thread*)arg;
	struct worker* worker = fast_reload_thread->worker;

	/* Read and handle the command */
	fr_main_handle_cmd(fast_reload_thread);
	if(worker->daemon->fast_reload_thread != NULL) {
		/* If not exited, see if there are more pending statuses
		 * from the fast reload thread. */
		fr_check_cmd_from_thread(fast_reload_thread);
	}
}

#ifdef HAVE_SSL
/** fast reload, send client item over SSL. Returns number of bytes
 * printed, 0 on wait later, or -1 on failure. */
static int
fr_client_send_item_ssl(struct fast_reload_printq* printq)
{
	int r;
	ERR_clear_error();
	r = SSL_write(printq->remote.ssl,
		printq->client_item+printq->client_byte_count,
		printq->client_len - printq->client_byte_count);
	if(r <= 0) {
		int want = SSL_get_error(printq->remote.ssl, r);
		if(want == SSL_ERROR_ZERO_RETURN) {
			log_err("fast_reload print to remote client: "
				"SSL_write says connection closed.");
			return -1;
		} else if(want == SSL_ERROR_WANT_READ) {
			/* wait for read condition */
			printq->client_cp->ssl_shake_state = comm_ssl_shake_hs_read;
			comm_point_listen_for_rw(printq->client_cp, 1, 0);
			return 0;
		} else if(want == SSL_ERROR_WANT_WRITE) {
#ifdef USE_WINSOCK
			ub_winsock_tcp_wouldblock(printq->client_cp->ev->ev, UB_EV_WRITE);
#endif
			return 0; /* write more later */
		} else if(want == SSL_ERROR_SYSCALL) {
#ifdef EPIPE
			if(errno == EPIPE && verbosity < 2) {
				/* silence 'broken pipe' */
				return -1;
			}
#endif
			if(errno != 0)
				log_err("fast_reload print to remote client: "
					"SSL_write syscall: %s",
					sock_strerror(errno));
			return -1;
		}
		log_crypto_err_io("fast_reload print to remote client: "
			"could not SSL_write", want);
		return -1;
	}
	return r;
}
#endif /* HAVE_SSL */

/** fast reload, send client item for fd, returns bytes sent, or 0 for wait
 * later, or -1 on failure. */
static int
fr_client_send_item_fd(struct fast_reload_printq* printq)
{
	int r;
	r = (int)send(printq->remote.fd,
		printq->client_item+printq->client_byte_count,
		printq->client_len - printq->client_byte_count, 0);
	if(r == -1) {
		if(
#ifndef USE_WINSOCK
			errno == EINTR || errno == EAGAIN
#  ifdef EWOULDBLOCK
			|| errno == EWOULDBLOCK
#  endif
#else
			WSAGetLastError() == WSAEINTR ||
			WSAGetLastError() == WSAEINPROGRESS ||
			WSAGetLastError() == WSAEWOULDBLOCK
#endif
			) {
#ifdef USE_WINSOCK
			ub_winsock_tcp_wouldblock(printq->client_cp->ev->ev, UB_EV_WRITE);
#endif
			return 0; /* Try again. */
		}
		log_err("fast_reload print to remote client: send failed: %s",
			sock_strerror(errno));
		return -1;
	}
	return r;
}

/** fast reload, send current client item. false on failure or wait later. */
static int
fr_client_send_item(struct fast_reload_printq* printq)
{
	int r;
#ifdef HAVE_SSL
	if(printq->remote.ssl) {
		r = fr_client_send_item_ssl(printq);
	} else {
#endif
		r = fr_client_send_item_fd(printq);
#ifdef HAVE_SSL
	}
#endif
	if(r == 0) {
		/* Wait for later. */
		return 0;
	} else if(r == -1) {
		/* It failed, close comm point and stop sending. */
		fr_printq_remove(printq);
		return 0;
	}
	printq->client_byte_count += r;
	if(printq->client_byte_count < printq->client_len)
		return 0; /* Print more later. */
	return 1;
}

/** fast reload, pick up the next item to print */
static void
fr_client_pickup_next_item(struct fast_reload_printq* printq)
{
	struct config_strlist* item;
	/* Pop first off the list. */
	if(!printq->to_print->first) {
		printq->client_item = NULL;
		printq->client_len = 0;
		printq->client_byte_count = 0;
		return;
	}
	item = printq->to_print->first;
	if(item->next) {
		printq->to_print->first = item->next;
	} else {
		printq->to_print->first = NULL;
		printq->to_print->last = NULL;
	}
	item->next = NULL;
	printq->client_len = 0;
	printq->client_byte_count = 0;
	printq->client_item = item->str;
	item->str = NULL;
	free(item);
	/* The len is the number of bytes to print out, and thus excludes
	 * the terminator zero. */
	if(printq->client_item)
		printq->client_len = (int)strlen(printq->client_item);
}

int fast_reload_client_callback(struct comm_point* ATTR_UNUSED(c), void* arg,
	int err, struct comm_reply* ATTR_UNUSED(rep))
{
	struct fast_reload_printq* printq = (struct fast_reload_printq*)arg;
	if(!printq->client_cp) {
		fr_printq_remove(printq);
		return 0; /* the output is closed and deleted */
	}
	if(err != NETEVENT_NOERROR) {
		verbose(VERB_ALGO, "fast reload client: error, close it");
		fr_printq_remove(printq);
		return 0;
	}
#ifdef HAVE_SSL
	if(printq->client_cp->ssl_shake_state == comm_ssl_shake_hs_read) {
		/* read condition satisfied back to writing */
		comm_point_listen_for_rw(printq->client_cp, 0, 1);
		printq->client_cp->ssl_shake_state = comm_ssl_shake_none;
	}
#endif /* HAVE_SSL */

	/* Pickup an item if there are none */
	if(!printq->client_item) {
		fr_client_pickup_next_item(printq);
	}
	if(!printq->client_item) {
		if(printq->in_list) {
			/* Nothing more to print, it can be removed. */
			fr_printq_remove(printq);
			return 0;
		}
		/* Done with printing for now. */
		comm_point_stop_listening(printq->client_cp);
		return 0;
	}

	/* Try to print out a number of items, if they can print in full. */
	while(printq->client_item) {
		/* Send current item, if any. */
		if(printq->client_item && printq->client_len != 0 &&
			printq->client_byte_count < printq->client_len) {
			if(!fr_client_send_item(printq))
				return 0;
		}

		/* The current item is done. */
		if(printq->client_item) {
			free(printq->client_item);
			printq->client_item = NULL;
			printq->client_len = 0;
			printq->client_byte_count = 0;
		}
		if(!printq->to_print->first) {
			if(printq->in_list) {
				/* Nothing more to print, it can be removed. */
				fr_printq_remove(printq);
				return 0;
			}
			/* Done with printing for now. */
			comm_point_stop_listening(printq->client_cp);
			return 0;
		}
		fr_client_pickup_next_item(printq);
	}

	return 0;
}

#ifndef THREADS_DISABLED
/** fast reload printq create */
static struct fast_reload_printq*
fr_printq_create(struct comm_point* c, struct worker* worker)
{
	struct fast_reload_printq* printq = calloc(1, sizeof(*printq));
	if(!printq)
		return NULL;
	printq->to_print = calloc(1, sizeof(*printq->to_print));
	if(!printq->to_print) {
		free(printq);
		return NULL;
	}
	printq->worker = worker;
	printq->client_cp = c;
	printq->client_cp->callback = fast_reload_client_callback;
	printq->client_cp->cb_arg = printq;
	return printq;
}
#endif /* !THREADS_DISABLED */

/** fast reload printq delete */
static void
fr_printq_delete(struct fast_reload_printq* printq)
{
	if(!printq)
		return;
#ifdef HAVE_SSL
	if(printq->remote.ssl) {
		SSL_shutdown(printq->remote.ssl);
		SSL_free(printq->remote.ssl);
	}
#endif
	comm_point_delete(printq->client_cp);
	if(printq->to_print) {
		config_delstrlist(printq->to_print->first);
		free(printq->to_print);
	}
	free(printq);
}

/** fast reload printq, returns true if the list is empty and no item */
static int
fr_printq_empty(struct fast_reload_printq* printq)
{
	if(printq->to_print->first == NULL && printq->client_item == NULL)
		return 1;
	return 0;
}

/** fast reload printq, insert onto list */
static void
fr_printq_list_insert(struct fast_reload_printq* printq, struct daemon* daemon)
{
	if(printq->in_list)
		return;
	printq->next = daemon->fast_reload_printq_list;
	if(printq->next)
		printq->next->prev = printq;
	printq->prev = NULL;
	printq->in_list = 1;
	daemon->fast_reload_printq_list = printq;
}

/** fast reload printq delete list */
void
fast_reload_printq_list_delete(struct fast_reload_printq* list)
{
	struct fast_reload_printq* printq = list, *next;
	while(printq) {
		next = printq->next;
		fr_printq_delete(printq);
		printq = next;
	}
}

/** fast reload printq remove the item from the printq list */
static void
fr_printq_list_remove(struct fast_reload_printq* printq)
{
	struct daemon* daemon = printq->worker->daemon;
	if(printq->prev == NULL)
		daemon->fast_reload_printq_list = printq->next;
	else	printq->prev->next = printq->next;
	if(printq->next)
		printq->next->prev = printq->prev;
	printq->in_list = 0;
}

/** fast reload printq, remove the printq when no longer needed,
 * like the stream is closed. */
static void
fr_printq_remove(struct fast_reload_printq* printq)
{
	if(!printq)
		return;
	if(printq->worker->daemon->fast_reload_thread &&
		printq->worker->daemon->fast_reload_thread->printq == printq)
		printq->worker->daemon->fast_reload_thread->printq = NULL;
	if(printq->in_list)
		fr_printq_list_remove(printq);
	fr_printq_delete(printq);
}

/** fast reload thread, send stop command to the thread, from the main thread.
 */
static void
fr_send_stop(struct fast_reload_thread* fr)
{
	fr_send_cmd_to(fr, fast_reload_notification_exit, 1, 0);
}

void
fast_reload_thread_start(RES* ssl, struct worker* worker, struct rc_state* s,
	int fr_verb, int fr_nopause, int fr_drop_mesh)
{
	if(worker->daemon->fast_reload_thread) {
		log_err("fast reload thread already running");
		return;
	}
	if(!fast_reload_thread_setup(worker, fr_verb, fr_nopause,
		fr_drop_mesh)) {
		if(!ssl_printf(ssl, "error could not setup thread\n"))
			return;
		return;
	}
	worker->daemon->fast_reload_thread->started = 1;

#ifndef THREADS_DISABLED
	/* Setup command listener in remote servicing thread */
	/* The listener has to be nonblocking, so the the remote servicing
	 * thread can continue to service DNS queries, the fast reload
	 * thread is going to read the config from disk and apply it. */
	/* The commpair[1] element can stay blocking, it is used by the
	 * fast reload thread to communicate back. The thread needs to wait
	 * at these times, when it has to check briefly it can use poll. */
	fd_set_nonblock(worker->daemon->fast_reload_thread->commpair[0]);
	worker->daemon->fast_reload_thread->service_event = ub_event_new(
		comm_base_internal(worker->base),
		worker->daemon->fast_reload_thread->commpair[0],
		UB_EV_READ | UB_EV_PERSIST, fast_reload_service_cb,
		worker->daemon->fast_reload_thread);
	if(!worker->daemon->fast_reload_thread->service_event) {
		fast_reload_thread_desetup(worker->daemon->fast_reload_thread);
		if(!ssl_printf(ssl, "error out of memory\n"))
			return;
		return;
	}
	if(ub_event_add(worker->daemon->fast_reload_thread->service_event,
		NULL) != 0) {
		fast_reload_thread_desetup(worker->daemon->fast_reload_thread);
		if(!ssl_printf(ssl, "error out of memory adding service event\n"))
			return;
		return;
	}
	worker->daemon->fast_reload_thread->service_event_is_added = 1;

	/* Setup the comm point to the remote control client as an event
	 * on the remote servicing thread, which it already is.
	 * It needs a new callback to service it. */
	log_assert(s);
	state_list_remove_elem(&s->rc->busy_list, s->c);
	s->rc->active --;
	/* Set the comm point file descriptor to nonblocking. So that
	 * printout to the remote control client does not block the
	 * server thread from servicing DNS queries. */
	fd_set_nonblock(s->c->fd);
	worker->daemon->fast_reload_thread->printq = fr_printq_create(s->c,
		worker);
	if(!worker->daemon->fast_reload_thread->printq) {
		fast_reload_thread_desetup(worker->daemon->fast_reload_thread);
		if(!ssl_printf(ssl, "error out of memory create printq\n"))
			return;
		return;
	}
	worker->daemon->fast_reload_thread->printq->remote = *ssl;
	s->rc = NULL; /* move away the rc state */
	/* Nothing to print right now, so no need to have it active. */
	comm_point_stop_listening(worker->daemon->fast_reload_thread->printq->client_cp);

	/* Start fast reload thread */
	ub_thread_create(&worker->daemon->fast_reload_thread->tid,
		fast_reload_thread_main, worker->daemon->fast_reload_thread);
#else
	(void)s;
#endif
}

void
fast_reload_thread_stop(struct fast_reload_thread* fast_reload_thread)
{
	struct worker* worker = fast_reload_thread->worker;
	if(!fast_reload_thread)
		return;
	fr_send_stop(fast_reload_thread);
	if(worker->daemon->fast_reload_thread != NULL) {
		/* If it did not exit yet, join with the thread now. It is
		 * going to exit because the exit command is sent to it. */
		fr_main_perform_done(fast_reload_thread);
	}
}
