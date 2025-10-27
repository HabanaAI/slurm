/*****************************************************************************\
 *  gres_select_util.c - filters used in the select plugin
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *  Derived in large part from code previously in interfaces/gres.h
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

#include "gres_select_util.h"

#include "src/common/xstring.h"

typedef struct {
	bool first_set;
	job_resources_t *job_res;
	bool rc;
} foreach_gres_job_mem_set_args_t;

typedef struct {
	int min_cpus;
	uint32_t node_count;
	uint32_t sockets_per_node;
	uint32_t task_count;
} foreach_gres_job_min_cpus_args_t;

typedef struct {
	int min_cpus;
	uint32_t sockets_per_node;
	uint32_t tasks_per_node;
} foreach_gres_job_min_cpu_node_args_t;

typedef struct {
	int min_tasks;
	uint32_t node_count;
	uint16_t ntasks_per_tres;
	uint32_t plugin_id;
	uint32_t sockets_per_node;
} foreach_gres_job_min_tasks_args_t;

typedef struct {
	uint64_t cpu_per_gpu;
	uint16_t *cpus_per_task;
	char **cpus_per_tres;
	uint64_t mem_per_gpu;
	char **mem_per_tres;
	uint32_t plugin_id;
} foreach_gres_job_set_defs_args_t;

static int _foreach_gres_job_set_defs(void *x, void *arg)
{
	gres_state_t *gres_state_job = x;
	foreach_gres_job_set_defs_args_t *args = arg;
	gres_job_state_t *gres_js;

	if (gres_state_job->plugin_id != args->plugin_id)
		return 0;
	gres_js = gres_state_job->gres_data;
	if (!gres_js)
		return 0;
	gres_js->def_cpus_per_gres = args->cpu_per_gpu;
	gres_js->def_mem_per_gres = args->mem_per_gpu;
	if (!gres_js->cpus_per_gres) {
		xfree(*(args->cpus_per_tres));
		if (args->cpu_per_gpu)
			xstrfmtcat(*(args->cpus_per_tres), "gpu:%"PRIu64,
				   args->cpu_per_gpu);
	}
	if (!gres_js->mem_per_gres) {
		xfree(*(args->mem_per_tres));
		if (args->mem_per_gpu)
			xstrfmtcat(*(args->mem_per_tres), "gpu:%"PRIu64,
				   args->mem_per_gpu);
	}
	if (args->cpu_per_gpu && gres_js->gres_per_task) {
		*(args->cpus_per_task) =
			MAX(*(args->cpus_per_task),
			    (gres_js->gres_per_task * args->cpu_per_gpu));
	}

	return 0;
}

/*
 * Set job default parameters in a given element of a list
 * IN job_gres_list - job's gres_list built by gres_job_state_validate()
 * IN gres_name - name of gres, apply defaults to all elements (e.g. updates to
 *		  gres_name="gpu" would apply to "gpu:tesla", "gpu:volta", etc.)
 * IN cpu_per_gpu - value to set as default
 * IN mem_per_gpu - value to set as default
 * OUT *cpus_per_tres - CpusPerTres string displayed by scontrol show job
 * OUT *mem_per_tres - MemPerTres string displayed by scontrol show job
 * IN/OUT *cpus_per_task - Increased if cpu_per_gpu * gres_per_task is more than
 *                         *cpus_per_task
 */
extern void gres_select_util_job_set_defs(list_t *job_gres_list,
					  char *gres_name,
					  uint64_t cpu_per_gpu,
					  uint64_t mem_per_gpu,
					  char **cpus_per_tres,
					  char **mem_per_tres,
					  uint16_t *cpus_per_task)
{
	foreach_gres_job_set_defs_args_t args = {
		.cpu_per_gpu = cpu_per_gpu,
		.cpus_per_task = cpus_per_task,
		.cpus_per_tres = cpus_per_tres,
		.mem_per_gpu = mem_per_gpu,
		.mem_per_tres = mem_per_tres,
	};

	/*
	 * Currently only GPU supported, check how cpus_per_tres/mem_per_tres
	 * is handled in _fill_job_desc_from_sbatch_opts and
	 * _job_desc_msg_create_from_opts.
	 */
	xassert(!xstrcmp(gres_name, "gpu"));

	if (!job_gres_list)
		return;

	args.plugin_id = gres_build_id(gres_name);
	(void) list_for_each(job_gres_list, _foreach_gres_job_set_defs, &args);
}

/* sets args->min_cpus with min required cpus needed to satisfy gres request */
static int _foreach_gres_job_min_cpu_node(void *x, void *arg)
{
	gres_state_t *gres_state_job = x;
	foreach_gres_job_min_cpu_node_args_t *args = arg;
	gres_job_state_t *gres_js = gres_state_job->gres_data;
	uint16_t cpus_per_gres;
	uint64_t total_gres = 0;
	int tmp;

	if (gres_js->cpus_per_gres)
		cpus_per_gres = gres_js->cpus_per_gres;
	else
		cpus_per_gres = gres_js->def_cpus_per_gres;
	if (cpus_per_gres == 0)
		return 0;

	if (gres_js->gres_per_node) {
		total_gres = gres_js->gres_per_node;
	} else if (gres_js->gres_per_socket) {
		total_gres = gres_js->gres_per_socket * args->sockets_per_node;
	} else if (gres_js->gres_per_task) {
		total_gres = gres_js->gres_per_task * args->tasks_per_node;
	} else
		total_gres = 1;
	tmp = cpus_per_gres * total_gres;
	args->min_cpus = MAX(args->min_cpus, tmp);

	return 0;
}

/*
 * Determine the minimum number of CPUs required to satisfy the job's GRES
 *	request on one node
 * sockets_per_node IN - count of sockets per node in job allocation
 * tasks_per_node IN - count of tasks per node in job allocation
 * job_gres_list IN - job GRES specification
 * RET count of required CPUs for the job
 */
extern int gres_select_util_job_min_cpu_node(uint32_t sockets_per_node,
					     uint32_t tasks_per_node,
					     list_t *job_gres_list)
{
	foreach_gres_job_min_cpu_node_args_t args = {
		.sockets_per_node = sockets_per_node,
		.tasks_per_node = tasks_per_node,
	};

	if (!job_gres_list || (list_count(job_gres_list) == 0))
		return 0;

	(void) list_for_each(job_gres_list, _foreach_gres_job_min_cpu_node,
			     &args);
	return args.min_cpus;
}

static int _foreach_gres_job_min_tasks(void *x, void *arg)
{
	gres_state_t *gres_state_job = x;
	foreach_gres_job_min_tasks_args_t *args = arg;
	gres_job_state_t *gres_js;
	uint64_t total_gres = 0;
	int tmp;

	/* Filter on GRES name, if specified */
	if (args->plugin_id && (args->plugin_id != gres_state_job->plugin_id))
		return 0;

	gres_js = gres_state_job->gres_data;

	if (gres_js->gres_per_job) {
		total_gres = gres_js->gres_per_job;
	} else if (gres_js->gres_per_node) {
		total_gres = gres_js->gres_per_node * args->node_count;
	} else if (gres_js->gres_per_socket) {
		total_gres = gres_js->gres_per_socket * args->node_count *
			args->sockets_per_node;
	} else if (gres_js->gres_per_task) {
		error("%s: gres_per_task and ntasks_per_tres conflict",
			__func__);
	} else
		return 0;

	tmp = args->ntasks_per_tres * total_gres;
	args->min_tasks = MAX(args->min_tasks, tmp);

	return 0;
}

/*
 * Determine the minimum number of tasks required to satisfy the job's GRES
 *	request (based upon total GRES times ntasks_per_tres value). If
 *	ntasks_per_tres is not specified, returns 0.
 * node_count IN - count of nodes in job allocation
 * sockets_per_node IN - count of sockets per node in job allocation
 * ntasks_per_tres IN - # of tasks per GPU
 * gres_name IN - (optional) Filter GRES by name. If NULL, check all GRES
 * job_gres_list IN - job GRES specification
 * RET count of required tasks for the job
 */
extern int gres_select_util_job_min_tasks(uint32_t node_count,
					  uint32_t sockets_per_node,
					  uint16_t ntasks_per_tres,
					  char *gres_name,
					  list_t *job_gres_list)
{
	foreach_gres_job_min_tasks_args_t args = {
		.node_count = node_count,
		.ntasks_per_tres = ntasks_per_tres,
		.sockets_per_node = sockets_per_node,
	};

	if (!ntasks_per_tres || (ntasks_per_tres == NO_VAL16))
		return 0;

	if (!job_gres_list || (list_count(job_gres_list) == 0))
		return 0;

	if (gres_name && (gres_name[0] != '\0'))
		args.plugin_id = gres_build_id(gres_name);

	(void) list_for_each(job_gres_list, _foreach_gres_job_min_tasks, &args);

	return args.min_tasks;
}

static int _foreach_gres_job_mem_set(void *x, void *arg)
{
	gres_state_t *gres_state_job = x;
	foreach_gres_job_mem_set_args_t *args = arg;
	gres_job_state_t *gres_js = gres_state_job->gres_data;
	uint64_t gres_cnt, mem_size, mem_per_gres;
	int node_off;
	node_record_t *node_ptr;

	if (gres_js->mem_per_gres)
		mem_per_gres = gres_js->mem_per_gres;
	else
		mem_per_gres = gres_js->def_mem_per_gres;
	/*
	 * The logic below is correct because the only mem_per_gres
	 * is --mem-per-gpu adding another option will require change
	 * to take MAX of mem_per_gres for all types.
	 * Similar logic is in _step_alloc() (which is called by
	 * gres_stepmgr_step_alloc()), which would also need to be changed
	 * if another mem_per_gres option was added.
	 */
	if (!mem_per_gres || !gres_js->gres_cnt_node_select)
		return 0;
	args->rc = true;
	node_off = -1;
	for (int i = 0;
	     (node_ptr = next_node_bitmap(args->job_res->node_bitmap, &i));
	     i++) {
		node_off++;
		if (args->job_res->whole_node & WHOLE_NODE_REQUIRED) {
			gres_state_t *gres_state_node;
			gres_node_state_t *gres_ns;

			gres_state_node =
				list_find_first(node_ptr->gres_list,
						gres_find_id,
						&gres_state_job->plugin_id);
			if (!gres_state_node)
				return 0;
			gres_ns = gres_state_node->gres_data;
			gres_cnt = gres_ns->gres_cnt_avail;
		} else
			gres_cnt = gres_js->gres_cnt_node_select[i];
		mem_size = mem_per_gres * gres_cnt;
		if (args->first_set)
			args->job_res->memory_allocated[node_off] = mem_size;
		else
			args->job_res->memory_allocated[node_off] += mem_size;
	}

	args->first_set = false;
	return 0;
}

/*
 * Set per-node memory limits based upon GRES assignments
 * RET TRUE if mem-per-tres specification used to set memory limits
 */
extern bool gres_select_util_job_mem_set(list_t *job_gres_list,
					 job_resources_t *job_res)
{
	foreach_gres_job_mem_set_args_t args = {
		.first_set = true,
		.job_res = job_res,
	};

	if (!job_gres_list)
		return false;

	if (!bit_set_count(job_res->node_bitmap))
		return false;

	(void) list_for_each(job_gres_list, _foreach_gres_job_mem_set, &args);

	return args.rc;
}

static int _foreach_gres_job_min_cpus(void *x, void *arg)
{
	gres_state_t *gres_state_job = x;
	foreach_gres_job_min_cpus_args_t *args = arg;
	gres_job_state_t *gres_js = gres_state_job->gres_data;
	uint16_t cpus_per_gres;
	uint64_t total_gres = 0;
	int tmp;

	if (gres_js->cpus_per_gres)
		cpus_per_gres = gres_js->cpus_per_gres;
	else
		cpus_per_gres = gres_js->def_cpus_per_gres;
	if (cpus_per_gres == 0)
		return 0;
	if (gres_js->gres_per_job) {
		total_gres = gres_js->gres_per_job;
	} else if (gres_js->gres_per_node) {
		total_gres = gres_js->gres_per_node * args->node_count;
	} else if (gres_js->gres_per_socket) {
		total_gres = gres_js->gres_per_socket * args->node_count *
			args->sockets_per_node;
	} else if (gres_js->gres_per_task) {
		total_gres = gres_js->gres_per_task * args->task_count;
	} else
		return 0;
	tmp = cpus_per_gres * total_gres;
	args->min_cpus = MAX(args->min_cpus, tmp);

	return 0;
}

/*
 * Determine the minimum number of CPUs required to satisfy the job's GRES
 *	request (based upon total GRES times cpus_per_gres value)
 * node_count IN - count of nodes in job allocation
 * sockets_per_node IN - count of sockets per node in job allocation
 * task_count IN - count of tasks in job allocation
 * job_gres_list IN - job GRES specification
 * RET count of required CPUs for the job
 */
extern int gres_select_util_job_min_cpus(uint32_t node_count,
					 uint32_t sockets_per_node,
					 uint32_t task_count,
					 list_t *job_gres_list)
{
	foreach_gres_job_min_cpus_args_t args = {
		.node_count = node_count,
		.sockets_per_node = sockets_per_node,
		.task_count = task_count,
	};

	if (!job_gres_list || (list_count(job_gres_list) == 0))
		return 0;

	(void) list_for_each(job_gres_list, _foreach_gres_job_min_cpus, &args);

	return args.min_cpus;
}

static int _foreach_gres_job_mem_max(void *x, void *arg)
{
	gres_state_t *gres_state_job = x;
	uint64_t *mem_max = arg;
	gres_job_state_t *gres_js = gres_state_job->gres_data;
	uint64_t mem_per_gres;

	if (gres_js->mem_per_gres)
		mem_per_gres = gres_js->mem_per_gres;
	else
		mem_per_gres = gres_js->def_mem_per_gres;
	*mem_max = MAX(*mem_max, mem_per_gres);

	return 0;
}

/*
 * Determine if the job GRES specification includes a mem-per-tres specification
 * RET largest mem-per-tres specification found
 */
extern uint64_t gres_select_util_job_mem_max(list_t *job_gres_list)
{
	uint64_t mem_max = 0;

	if (!job_gres_list)
		return 0;

	(void) list_for_each(job_gres_list, _foreach_gres_job_mem_max,
			     &mem_max);

	return mem_max;
}

/* note: key is not used */
static int _is_gres_per_task_set(void *x, void *key)
{
	gres_state_t *gres_state_job = x;
	gres_job_state_t *gres_js = gres_state_job->gres_data;

	if (gres_js->gres_per_task)
		return -1; /* true */

	return 0;
}

/*
 * Determine if job GRES specification includes a tres-per-task specification
 * RET TRUE if any GRES requested by the job include a tres-per-task option
 */
extern bool gres_select_util_job_tres_per_task(list_t *job_gres_list)
{
	if (!job_gres_list)
		return false;

	return list_find_first(job_gres_list, _is_gres_per_task_set, NULL);
}

static int _foreach_gres_get_task_limit(void *x, void *arg)
{
	sock_gres_t *sock_gres = x;
	uint32_t *max_tasks = arg;
	gres_job_state_t *gres_js;
	uint64_t task_limit;

	xassert(sock_gres->gres_state_job);

	gres_js = sock_gres->gres_state_job->gres_data;
	if (gres_js->gres_per_task == 0)
		return 0;
	task_limit = sock_gres->total_cnt / gres_js->gres_per_task;
	*max_tasks = MIN(*max_tasks, task_limit);

	return 0;
}

/*
 * Return the maximum number of tasks that can be started on a node with
 * sock_gres_list (per-socket GRES details for some node)
 */
extern uint32_t gres_select_util_get_task_limit(list_t *sock_gres_list)
{
	uint32_t max_tasks = NO_VAL;

	(void) list_for_each(sock_gres_list, _foreach_gres_get_task_limit,
			     &max_tasks);

	return max_tasks;
}

static int _accumulate_gres_device_req(void *x, void *arg)
{
	gres_state_t *gres_state_job = x, *new_gres_state_job;
	list_t *new_gres_list = arg;

	if ((new_gres_state_job = list_find_first(
		     new_gres_list,
		     gres_find_id,
		     &gres_state_job->plugin_id))) {
		gres_job_state_t *accum_gres_js =
			new_gres_state_job->gres_data;
		gres_job_state_t *gres_js = gres_state_job->gres_data;

		/*
		 * Add up gres counts but cpus_per_gres and mem_per_gres should
		 * be same.
		 */
		accum_gres_js->gres_per_job += gres_js->gres_per_job;
		accum_gres_js->gres_per_node += gres_js->gres_per_node;
		accum_gres_js->gres_per_socket += gres_js->gres_per_socket;
		accum_gres_js->gres_per_task += gres_js->gres_per_task;
		accum_gres_js->total_gres += gres_js->total_gres;
	} else {
		gres_job_state_t *gres_js = gres_job_state_dup(
			gres_state_job->gres_data);
		/*
		 * The type id or name should never be set here as we should
		 * only have counters here for the gres_per_* counters based on
		 * cpus/mem per_gres.
		 */
		xfree(gres_js->type_name);
		gres_js->type_id = 0;

		new_gres_state_job = gres_create_state(
			gres_state_job, GRES_STATE_SRC_STATE_PTR,
			GRES_STATE_TYPE_JOB, gres_js);
		list_append(new_gres_list, new_gres_state_job);
	}

	return 0;
}


/*
 * Create a (partial) copy of a job's gres state accumulating the gres_per_*
 * requirements to accurately calculate cpus_per_gres
 * IN gres_list - list of Gres records
 * RET The copy of list or NULL on failure
 */
extern list_t *gres_select_util_create_list_req_accum(list_t *gres_list)
{
	list_t *new_gres_list;

	if (!gres_list)
		return NULL;

	new_gres_list = list_create(gres_job_list_delete);

	(void) list_for_each(gres_list, _accumulate_gres_device_req,
			     new_gres_list);

	return new_gres_list;
}
