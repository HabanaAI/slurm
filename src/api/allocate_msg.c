/*****************************************************************************\
 *  allocate_msg.c - Message handler for communication with with
 *                       the slurmctld during an allocation.
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>.
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "slurm/slurm.h"

#include "src/common/eio.h"
#include "src/common/fd.h"
#include "src/common/forward.h"
#include "src/common/half_duplex.h"
#include "src/common/net.h"
#include "src/common/macros.h"
#include "src/common/read_config.h"
#include "src/interfaces/auth.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_common.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"

#include "src/interfaces/conn.h"

struct allocation_msg_thread {
	slurm_allocation_callbacks_t callback;
	eio_handle_t *handle;
	pthread_t id;
};

static void _handle_msg(void *arg, slurm_msg_t *msg);
static pthread_mutex_t msg_thr_start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t msg_thr_start_cond = PTHREAD_COND_INITIALIZER;
static bool msg_thr_start_done = false;
static struct io_operations message_socket_ops = {
	.readable = &eio_message_socket_readable,
	.handle_read = &eio_message_socket_accept,
	.handle_msg = &_handle_msg
};

static void *_msg_thr_internal(void *arg)
{
	int signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM,
			 SIGUSR1, SIGUSR2, 0};

	debug("Entering _msg_thr_internal");
	xsignal_block(signals);
	slurm_mutex_lock(&msg_thr_start_lock);
	slurm_cond_signal(&msg_thr_start_cond);
	msg_thr_start_done = true;
	slurm_mutex_unlock(&msg_thr_start_lock);
	eio_handle_mainloop((eio_handle_t *)arg);
	debug("Leaving _msg_thr_internal");

	return NULL;
}

extern allocation_msg_thread_t *slurm_allocation_msg_thr_create(
	uint16_t *port,
	const slurm_allocation_callbacks_t *callbacks)
{
	int sock = -1;
	eio_obj_t *obj;
	struct allocation_msg_thread *msg_thr = NULL;
	int cc;
	uint16_t *ports;

	debug("Entering slurm_allocation_msg_thr_create()");

	msg_thr = (struct allocation_msg_thread *)xmalloc(
		sizeof(struct allocation_msg_thread));

	/* Initialize the callback pointers */
	if (callbacks != NULL) {
		/* copy the user specified callback pointers */
		memcpy(&(msg_thr->callback), callbacks,
		       sizeof(slurm_allocation_callbacks_t));
	} else {
		/* set all callbacks to NULL */
		memset(&(msg_thr->callback), 0,
		       sizeof(slurm_allocation_callbacks_t));
	}

	ports = slurm_get_srun_port_range();
	if (ports)
		cc = net_stream_listen_ports(&sock, port, ports, false);
	else
		cc = net_stream_listen(&sock, port);
	if (cc < 0) {
		error("unable to initialize step launch listening socket: %m");
		xfree(msg_thr);
		return NULL;
	}
	debug("port from net_stream_listen is %hu", *port);
	obj = eio_obj_create(sock, &message_socket_ops, (void *)msg_thr);

	msg_thr->handle = eio_handle_create(slurm_conf.eio_timeout);
	if (!msg_thr->handle) {
		error("failed to create eio handle");
		xfree(msg_thr);
		return NULL;
	}
	eio_new_initial_obj(msg_thr->handle, obj);
	slurm_mutex_lock(&msg_thr_start_lock);
	slurm_thread_create(&msg_thr->id, _msg_thr_internal, msg_thr->handle);
	while (!msg_thr_start_done) {
		/*
		 * Wait until the message thread has blocked signals
		 * before continuing.
		 */
		slurm_cond_wait(&msg_thr_start_cond, &msg_thr_start_lock);
	}
	slurm_mutex_unlock(&msg_thr_start_lock);

	return (allocation_msg_thread_t *)msg_thr;
}

extern void slurm_allocation_msg_thr_destroy(
	allocation_msg_thread_t *arg)
{
	struct allocation_msg_thread *msg_thr =
		(struct allocation_msg_thread *)arg;
	if (msg_thr == NULL)
		return;

	debug2("slurm_allocation_msg_thr_destroy: clearing up message thread");
	eio_signal_shutdown(msg_thr->handle);
	slurm_thread_join(msg_thr->id);
	eio_handle_destroy(msg_thr->handle);
	xfree(msg_thr);
}

static void _handle_node_fail(struct allocation_msg_thread *msg_thr,
			      slurm_msg_t *msg)
{
	srun_node_fail_msg_t *nf = msg->data;

	if (msg_thr->callback.node_fail != NULL)
		(msg_thr->callback.node_fail)(nf);
}

/*
 * Job has been notified of it's approaching time limit.
 * Job will be killed shortly after timeout.
 * This RPC can arrive multiple times with the same or updated timeouts.
 */
static void _handle_timeout(struct allocation_msg_thread *msg_thr,
			    slurm_msg_t *msg)
{
	srun_timeout_msg_t *to = msg->data;

	debug3("received timeout message");

	if (msg_thr->callback.timeout != NULL)
		(msg_thr->callback.timeout)(to);
}

static void _handle_user_msg(struct allocation_msg_thread *msg_thr,
			     slurm_msg_t *msg)
{
	srun_user_msg_t *um = msg->data;
	debug3("received user message");

	if (msg_thr->callback.user_msg != NULL)
		(msg_thr->callback.user_msg)(um);
}

static void _handle_ping(struct allocation_msg_thread *msg_thr,
			     slurm_msg_t *msg)
{
	debug3("received ping message");
	slurm_send_rc_msg(msg, SLURM_SUCCESS);
}

static void _handle_job_complete(struct allocation_msg_thread *msg_thr,
				 slurm_msg_t *msg)
{
	srun_job_complete_msg_t *comp = msg->data;
	debug3("job complete message received");

	if (msg_thr->callback.job_complete != NULL)
		(msg_thr->callback.job_complete)(comp);
}

static void _handle_suspend(struct allocation_msg_thread *msg_thr,
			    slurm_msg_t *msg)
{
	suspend_msg_t *sus_msg = msg->data;
	debug3("received suspend message");

	if (msg_thr->callback.job_suspend != NULL)
		(msg_thr->callback.job_suspend)(sus_msg);
}

static void _net_forward(struct allocation_msg_thread *msg_thr,
			 slurm_msg_t *forward_msg)
{
	net_forward_msg_t *msg = forward_msg->data;
	int *local, *remote;

	local = xmalloc(sizeof(*local));
	remote = xmalloc(sizeof(*remote));

	*remote = conn_g_get_fd(forward_msg->tls_conn);
	net_set_nodelay(*remote, true, NULL);

	if (msg->port) {
		/* connect to host and given tcp port */
		slurm_addr_t local_addr;
		memset(&local_addr, 0, sizeof(local_addr));
		slurm_set_addr(&local_addr, msg->port, msg->target);

		*local = slurm_open_stream(&local_addr, false);
		if (*local == -1) {
			error("%s: failed to open x11 port `%s:%d`: %m",
			      __func__, msg->target, msg->port);
			goto error;
		}
		net_set_nodelay(*local, true, NULL);
	} else if (msg->target) {
		int rc;

		/* connect to local unix socket */
		if ((rc = slurm_open_unix_stream(msg->target, 0, local))) {
			error("%s: failed to open x11 display on `%s`: %s",
			      __func__, msg->target, slurm_strerror(rc));
			goto error;
		}
	}

	/*
	 * Setup is successful, let the remote end know. This must happen
	 * before eio takes over managing the rest of the traffic on the port.
	 */
	slurm_send_rc_msg(forward_msg, SLURM_SUCCESS);

	if (half_duplex_add_objs_to_handle(msg_thr->handle, local, remote,
					   forward_msg->tls_conn)) {
		goto error;
	}

	/* prevent the upstream call path from closing the connection */
	forward_msg->tls_conn = NULL;

	return;

error:
	slurm_send_rc_msg(forward_msg, SLURM_ERROR);
	xfree(local);
	xfree(remote);
}

static void
_handle_msg(void *arg, slurm_msg_t *msg)
{
	struct allocation_msg_thread *msg_thr =
		(struct allocation_msg_thread *)arg;
	uid_t req_uid;
	uid_t uid = getuid();

	req_uid = auth_g_get_uid(msg->auth_cred);

	if ((req_uid != slurm_conf.slurm_user_id) && (req_uid != 0) &&
	    (req_uid != uid)) {
		error ("Security violation, slurm message from uid %u",
		       req_uid);
		return;
	}

	switch (msg->msg_type) {
	case SRUN_PING:
		_handle_ping(msg_thr, msg);
		break;
	case SRUN_JOB_COMPLETE:
		_handle_job_complete(msg_thr, msg);
		break;
	case SRUN_TIMEOUT:
		_handle_timeout(msg_thr, msg);
		break;
	case SRUN_USER_MSG:
		_handle_user_msg(msg_thr, msg);
		break;
	case SRUN_NODE_FAIL:
		_handle_node_fail(msg_thr, msg);
		break;
	case SRUN_REQUEST_SUSPEND:
		_handle_suspend(msg_thr, msg);
		break;
	case SRUN_NET_FORWARD:
		debug2("received network forwarding RPC");
		_net_forward(msg_thr, msg);
		break;
	default:
		error("%s: received spurious message type: %s",
		      __func__, rpc_num2string(msg->msg_type));
		break;
	}
	return;
}
