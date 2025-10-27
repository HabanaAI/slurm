/*****************************************************************************\
 *  gres_sched.c - Scheduling functions used by cons_tres
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *  Derived in large part from code previously in common/gres.c
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

#include "src/common/slurm_xlator.h"

#include "gres_sched.h"

#include "src/common/xstring.h"

typedef struct {
	uint16_t *actual_cores_per_sock;
	bitstr_t *avail_core; /* Core bitmap of available cores on this node */
	uint16_t *avail_cores_per_sock; /* # of cores per socket available */
	uint16_t *avail_cpus; /* CPUs currently available on this node */
	uint16_t cores_per_socket; /* # of cores on each socket on the node */
	uint16_t cpus_per_core; /* Number of threads per core on the node */
	uint16_t cr_type; /* Allocation type (sockets, cores, etc.) */
	uint16_t gres_cpus;
	uint16_t min_cpus; /* Minimum cpus required on this node */
	int node_i; /* Index of the current node */
	uint16_t res_cores_per_gpu; /* Number of restricted cores per gpu */
	int sockets; /* Number of sockets on the node */
	list_t *sock_gres_list; /* list of sock_gres_t */
	uint64_t tot_cores;
} foreach_gres_add_args_t;

typedef struct {
	list_t **consec_gres;
	list_t *sock_gres_list;
} foreach_gres_consec_args_t;

static int _foreach_gres_str(void *x, void *arg)
{
	sock_gres_t *sock_data = x;
	char **out_str = arg;
	gres_job_state_t *gres_js;
	char *sep;

	if (!sock_data->gres_state_job) { /* Should never happen */
		error("%s: sock_data has no gres_state_job. This should never happen.",
		      __func__);
		return 0;
	}
	gres_js = sock_data->gres_state_job->gres_data;
	if (*out_str)
		sep = ",";
	else
		sep = "GRES:";
	if (gres_js->type_name) {
		xstrfmtcat(*out_str, "%s%s:%s:%"PRIu64, sep,
			   sock_data->gres_state_job->gres_name,
			   gres_js->type_name, sock_data->total_cnt);
	} else {
		xstrfmtcat(*out_str, "%s%s:%"PRIu64, sep,
			   sock_data->gres_state_job->gres_name,
			   sock_data->total_cnt);
	}

	return 0;
}

/*
 * Given a list of sock_gres_t entries, return a string identifying the
 * count of each GRES available on this set of nodes
 * IN sock_gres_list - count of GRES available in this group of nodes
 * RET xfree the returned string
 */
extern char *gres_sched_str(list_t *sock_gres_list)
{
	char *out_str = NULL;

	if (!sock_gres_list)
		return NULL;

	(void) list_for_each(sock_gres_list, _foreach_gres_str, &out_str);

	return out_str;
}

static int _foreach_gres_init(void *x, void *arg)
{
	gres_state_t *gres_state_job = x;
	bool *rc = arg;
	gres_job_state_t *gres_js = gres_state_job->gres_data;

	if (!gres_js->gres_per_job)
		return 0;
	gres_js->total_gres = 0;
	*rc = true;

	return 0;
}

/*
 * Clear GRES allocation info for all job GRES at start of scheduling cycle
 * Return TRUE if any gres_per_job constraints to satisfy
 */
extern bool gres_sched_init(list_t *job_gres_list)
{
	bool rc = false;

	if (!job_gres_list)
		return rc;

	(void) list_for_each(job_gres_list, _foreach_gres_init, &rc);

	return rc;
}

/* Note - key is not used */
static int _is_gres_per_job_met(void *x, void *key)
{
	gres_state_t *gres_state_job = x;
	gres_job_state_t *gres_js = gres_state_job->gres_data;

	if (gres_js->gres_per_job &&
	    (gres_js->gres_per_job > gres_js->total_gres))
		return -1; /* break out of list_find_first */

	return 0;
}

/*
 * Return TRUE if all gres_per_job specifications are satisfied
 */
extern bool gres_sched_test(list_t *job_gres_list, uint32_t job_id)
{
	if (!job_gres_list)
		return true;

	return !list_find_first(job_gres_list, _is_gres_per_job_met, NULL);
}

static void _gres_per_job_reduce_res_cores(bitstr_t *avail_core,
					   uint16_t *avail_cores_per_sock,
					   uint16_t *actual_cores_per_sock,
					   uint64_t *tot_cores,
					   uint16_t *avail_cpus,
					   uint64_t *gres_limit,
					   gres_job_state_t *gres_js,
					   uint16_t res_cores_per_gpu,
					   int sockets,
					   uint16_t cores_per_socket,
					   uint16_t cpus_per_core,
					   uint16_t cr_type,
					   int node_i)
{
	bitstr_t *res_cores;
	uint16_t tot_res_core;
	uint64_t max_res_cores = 0;
	int i = (sockets * cores_per_socket) - 1;
	bool done = false;
	int cnt;

	if (cr_type & SELECT_SOCKET)
		return;
	if (!gres_js->res_gpu_cores ||
	    !gres_js->res_gpu_cores[node_i])
		return;

	max_res_cores = *gres_limit * res_cores_per_gpu;
	res_cores = bit_copy(gres_js->res_gpu_cores[node_i]);
	bit_and(res_cores, avail_core);
	tot_res_core = bit_set_count(res_cores);

	if (tot_res_core <= max_res_cores) {
		FREE_NULL_BITMAP(res_cores);
		return;
	}

	while (!done) {
		while (tot_res_core > max_res_cores) {
			int s;
			/*
			* Must remove restricted cores from the end of the
			* bitmap first since cores are picked from front to
			* back. This helps the needed restricted cores get
			* picked.
			*/
			i  = bit_fls_from_bit(res_cores, i);
			if (i < 0)
				break; /* This should never happen */
			bit_clear(avail_core, i);
			tot_res_core--;

			s = i / cores_per_socket;
			actual_cores_per_sock[s]--;
			(*tot_cores)--;
			if (actual_cores_per_sock[s] <
			    avail_cores_per_sock[s])
				avail_cores_per_sock[s]--;
			i--;
		}
		cnt = *tot_cores * cpus_per_core;
		if (cnt < *avail_cpus)
			*avail_cpus = cnt;
		if (gres_js->cpus_per_gres) {
			uint64_t new_gres_limit =
				*avail_cpus / gres_js->cpus_per_gres;
			if (new_gres_limit < *gres_limit) {
				*gres_limit = new_gres_limit;
				max_res_cores = *gres_limit * res_cores_per_gpu;
			} else
				done = true;
		} else
			done = true;
	}
	FREE_NULL_BITMAP(res_cores);
}

static int _foreach_gres_add(void *x, void *arg)
{
	foreach_gres_add_args_t *args = arg;
	gres_state_t *gres_state_job = x;
	gres_job_state_t *gres_js = gres_state_job->gres_data;
	sock_gres_t *sock_data;
	uint64_t gres_limit;
	uint64_t min_gres;

	if (!gres_js->gres_per_job) /* Don't care about totals */
		return 0;
	sock_data =
		list_find_first(args->sock_gres_list,
				gres_find_sock_by_job_state, gres_state_job);
	if (!sock_data) /* None of this GRES available */
		return 0;
	if (gres_js->cpus_per_gres) {
		gres_limit = *(args->avail_cpus) / gres_js->cpus_per_gres;
		gres_limit = MIN(gres_limit, sock_data->total_cnt);
		args->gres_cpus = MAX(args->gres_cpus,
				      gres_limit * gres_js->cpus_per_gres);
	} else
		gres_limit = sock_data->total_cnt;

	min_gres = MAX(gres_js->gres_per_node, 1);
	if (gres_js->gres_per_task ||
	    (gres_js->ntasks_per_gres &&
	     (gres_js->ntasks_per_gres != NO_VAL16))) {
		/*
		* Already assumed a number of gres tasks
		* on this node.
		*/
		min_gres = gres_limit;
	}
	if (gres_js->gres_per_job > gres_js->total_gres) {
		gres_limit = MIN((gres_js->gres_per_job - gres_js->total_gres),
				 gres_limit);
	}
	gres_limit = MAX(gres_limit, min_gres);

	if ((gres_find_gpu_or_alt(gres_state_job, NULL)) &&
	    args->res_cores_per_gpu) {
		if (!args->actual_cores_per_sock) {
			args->actual_cores_per_sock =
				xcalloc(args->sockets, sizeof(uint16_t));
			for (int s = 0; s < args->sockets; s++) {
				int start_core = s * args->cores_per_socket;
				int end_core =
					start_core + args->cores_per_socket;
				args->actual_cores_per_sock[s] =
					bit_set_count_range(args->avail_core,
							    start_core,
							    end_core);
				args->tot_cores +=
					args->actual_cores_per_sock[s];
			}
		}

		_gres_per_job_reduce_res_cores(
			args->avail_core, args->avail_cores_per_sock,
			args->actual_cores_per_sock, &args->tot_cores,
			args->avail_cpus, &gres_limit, gres_js,
			args->res_cores_per_gpu, args->sockets,
			args->cores_per_socket, args->cpus_per_core,
			args->cr_type, args->node_i);
		if ((gres_limit < min_gres) ||
		    (args->min_cpus > *(args->avail_cpus)))
			return -1;
	}

	sock_data->total_cnt = gres_limit;
	gres_js->total_gres += gres_limit;

	return 0;
}

/*
 * Update a job's total_gres counter as we add a node to potential allocation
 * IN/OUT avail_cpus - CPUs currently available on this node
 * IN/OUT avail_core - Core bitmap of currently available cores on this node
 * IN/OUT avail_cores_per_sock - Number of cores per socket available
 * IN/OUT sock_gres_list - Per socket GRES availability on this node
 *			   (sock_gres_t). Updates total_cnt
 * IN job_gres_list - list of job's GRES requirements (gres_state_job_t)
 * IN res_cores_per_gpu - Number of restricted cores per gpu
 * IN sockets - Number of sockets on the node
 * IN cores_per_socket - Number of cores on each socket on the node
 * IN cpus_per_core - Number of threads per core on the node
 * IN cr_type - Allocation type (sockets, cores, etc.)
 * IN min_cpus - Minimum cpus required on this node
 * IN node_i - Index of the current node
 */
extern bool gres_sched_add(uint16_t *avail_cpus,
			   bitstr_t *avail_core,
			   uint16_t *avail_cores_per_sock,
			   list_t *sock_gres_list,
			   list_t *job_gres_list,
			   uint16_t res_cores_per_gpu,
			   int sockets,
			   uint16_t cores_per_socket,
			   uint16_t cpus_per_core,
			   uint16_t cr_type,
			   uint16_t min_cpus,
			   int node_i)
{
	bool rc = true;
	foreach_gres_add_args_t args = {
		.avail_core = avail_core,
		.avail_cores_per_sock = avail_cores_per_sock,
		.avail_cpus = avail_cpus,
		.cores_per_socket = cores_per_socket,
		.cpus_per_core = cpus_per_core,
		.cr_type = cr_type,
		.min_cpus = min_cpus,
		.node_i = node_i,
		.res_cores_per_gpu = res_cores_per_gpu,
		.sockets = sockets,
		.sock_gres_list = sock_gres_list,
	};

	if (!job_gres_list || !(*avail_cpus))
		return true;

	rc = !list_find_first(job_gres_list, _foreach_gres_add, &args);
	if (rc && args.gres_cpus && (args.gres_cpus < *avail_cpus) &&
	    (args.gres_cpus > args.min_cpus))
		*avail_cpus = args.gres_cpus;

	xfree(args.actual_cores_per_sock);
	return rc;
}

static int _foreach_gres_consec(void *x, void *arg)
{
	foreach_gres_consec_args_t *args = arg;
	gres_state_t *gres_state_job = x;
	gres_job_state_t *gres_js = gres_state_job->gres_data;
	sock_gres_t *sock_data, *consec_data;

	if (!gres_js->gres_per_job) /* Don't care about totals */
		return 0;
	sock_data =
		list_find_first(args->sock_gres_list,
				gres_find_sock_by_job_state, gres_state_job);
	if (!sock_data) /* None of this GRES available */
		return 0;
	if (*(args->consec_gres) == NULL)
		*(args->consec_gres) = list_create(gres_sock_delete);
	consec_data =
		list_find_first(*(args->consec_gres),
				gres_find_sock_by_job_state, gres_state_job);
	if (!consec_data) {
		consec_data = xmalloc(sizeof(sock_gres_t));
		consec_data->gres_state_job = gres_state_job;
		list_append(*(args->consec_gres), consec_data);
	}
	consec_data->total_cnt += sock_data->total_cnt;

	return 0;
}

/*
 * Create/update list GRES that can be made available on the specified node
 * IN/OUT consec_gres - list of sock_gres_t that can be made available on
 *			a set of nodes
 * IN job_gres_list - list of job's GRES requirements (gres_job_state_t)
 * IN sock_gres_list - Per socket GRES availability on this node (sock_gres_t)
 */
extern void gres_sched_consec(list_t **consec_gres, list_t *job_gres_list,
			      list_t *sock_gres_list)
{
	foreach_gres_consec_args_t args = {
		.consec_gres = consec_gres,
		.sock_gres_list = sock_gres_list,
	};

	if (!job_gres_list)
		return;

	(void) list_for_each(job_gres_list, _foreach_gres_consec, &args);
}

static int _find_insufficient_gres(void *x, void *args)
{
	gres_state_t *gres_state_job = x;
	gres_job_state_t *gres_js = gres_state_job->gres_data;
	list_t *sock_gres_list = args;
	sock_gres_t *sock_data;

	if (!gres_js->gres_per_job) /* Don't care about totals */
		return 0;
	if (gres_js->total_gres >= gres_js->gres_per_job)
		return 0;
	sock_data = list_find_first(sock_gres_list, gres_find_sock_by_job_state,
				    gres_state_job);
	if (!sock_data) /* None of this GRES available */
		return -1;
	if ((gres_js->total_gres + sock_data->total_cnt) <
	    gres_js->gres_per_job)
		return -1;

	return 0;
}

/*
 * Determine if the additional sock_gres_list resources will result in
 * satisfying the job's gres_per_job constraints
 * IN job_gres_list - job's GRES requirements
 * IN sock_gres_list - available GRES in a set of nodes, data structure built
 *		       by gres_job_sched_consec()
 */
extern bool gres_sched_sufficient(list_t *job_gres_list, list_t *sock_gres_list)
{
	if (!job_gres_list)
		return true;
	if (!sock_gres_list)
		return false;

	return !list_find_first(job_gres_list, _find_insufficient_gres,
				sock_gres_list);
}
