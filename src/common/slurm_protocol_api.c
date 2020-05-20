/*****************************************************************************\
 *  slurm_protocol_api.c - high-level slurm communication functions
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Copyright (C) 2010-2015 SchedMD LLC.
 *  Copyright (C) 2013      Intel, Inc.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Kevin Tew <tew1@llnl.gov>, et. al.
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

#include "config.h"

/* GLOBAL INCLUDES */

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* PROJECT INCLUDES */
#include "src/common/assoc_mgr.h"
#include "src/common/fd.h"
#include "src/common/forward.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/msg_aggr.h"
#include "src/common/pack.h"
#include "src/common/read_config.h"
#include "src/common/slurm_accounting_storage.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_protocol_interface.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_common.h"
#include "src/common/slurm_protocol_pack.h"
#include "src/common/slurm_route.h"
#include "src/common/strlcpy.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmdbd/read_config.h"

strong_alias(convert_num_unit2, slurm_convert_num_unit2);
strong_alias(convert_num_unit, slurm_convert_num_unit);
strong_alias(revert_num_unit, slurm_revert_num_unit);
strong_alias(get_convert_unit_val, slurm_get_convert_unit_val);
strong_alias(get_unit_type, slurm_get_unit_type);

/* EXTERNAL VARIABLES */

/* #DEFINES */
#define _log_hex(data, len)						\
	do {								\
		if (slurm_conf.debug_flags & DEBUG_FLAG_NET_RAW)	\
			_print_data(__func__, data, len);		\
	} while (0)


/* STATIC VARIABLES */
static int message_timeout = -1;

/* STATIC FUNCTIONS */
static char *_global_auth_key(void);
static void  _remap_slurmctld_errno(void);
static int   _unpack_msg_uid(Buf buffer, uint16_t protocol_version);
static bool  _is_port_ok(int, uint16_t, bool);
static void _print_data(const char *tag, const char *data, int len);

/* define slurmdbd_conf here so we can treat its existence as a flag */
slurmdbd_conf_t *slurmdbd_conf = NULL;

/**********************************************************************\
 * protocol configuration functions
\**********************************************************************/

/* Free memory space returned by _slurm_api_get_comm_config() */
static void _slurm_api_free_comm_config(slurm_protocol_config_t *proto_conf)
{
	if (proto_conf) {
		xfree(proto_conf->controller_addr);
		xfree(proto_conf);
	}
}

/*
 * Get communication data structure based upon configuration file
 * RET communication information structure, call _slurm_api_free_comm_config
 *	to release allocated memory
 */
static slurm_protocol_config_t *_slurm_api_get_comm_config(void)
{
	slurm_protocol_config_t *proto_conf = NULL;
	slurm_addr_t controller_addr;
	slurm_conf_t *conf;
	int i;

	conf = slurm_conf_lock();

	if (!conf->control_cnt ||
	    !conf->control_addr || !conf->control_addr[0]) {
		error("Unable to establish controller machine");
		goto cleanup;
	}
	if (conf->slurmctld_port == 0) {
		error("Unable to establish controller port");
		goto cleanup;
	}
	if (conf->control_cnt == 0) {
		error("No slurmctld servers configured");
		goto cleanup;
	}

	memset(&controller_addr, 0, sizeof(slurm_addr_t));
	slurm_set_addr(&controller_addr, conf->slurmctld_port,
		       conf->control_addr[0]);
	if (controller_addr.sin_port == 0) {
		error("Unable to establish control machine address");
		goto cleanup;
	}

	proto_conf = xmalloc(sizeof(slurm_protocol_config_t));
	proto_conf->controller_addr = xcalloc(conf->control_cnt,
					      sizeof(slurm_addr_t));
	proto_conf->control_cnt = conf->control_cnt;
	memcpy(&proto_conf->controller_addr[0], &controller_addr,
	       sizeof(slurm_addr_t));

	for (i = 1; i < proto_conf->control_cnt; i++) {
		if (conf->control_addr[i]) {
			slurm_set_addr(&proto_conf->controller_addr[i],
				       conf->slurmctld_port,
				       conf->control_addr[i]);
		}
	}

	if (conf->slurmctld_addr) {
		proto_conf->vip_addr_set = true;
		slurm_set_addr(&proto_conf->vip_addr, conf->slurmctld_port,
			       conf->slurmctld_addr);
	}

cleanup:
	slurm_conf_unlock();
	return proto_conf;
}

static int _get_tres_id(char *type, char *name)
{
	slurmdb_tres_rec_t tres_rec;
	memset(&tres_rec, 0, sizeof(slurmdb_tres_rec_t));
	tres_rec.type = type;
	tres_rec.name = name;

	return assoc_mgr_find_tres_pos(&tres_rec, false);
}

static int _tres_weight_item(double *weights, char *item_str)
{
	char *type = NULL, *value_str = NULL, *val_unit = NULL, *name = NULL;
	int tres_id;
	double weight_value = 0;

	if (!item_str) {
		error("TRES weight item is null");
		return SLURM_ERROR;
	}

	type = strtok_r(item_str, "=", &value_str);
	if (type == NULL) {
		error("\"%s\" is an invalid TRES weight entry", item_str);
		return SLURM_ERROR;
	}
	if (strchr(type, '/'))
		type = strtok_r(type, "/", &name);

	if (!value_str || !*value_str) {
		error("\"%s\" is an invalid TRES weight entry", item_str);
		return SLURM_ERROR;
	}

	if ((tres_id = _get_tres_id(type, name)) == -1) {
		error("TRES weight '%s%s%s' is not a configured TRES type.",
		      type, (name) ? ":" : "", (name) ? name : "");
		return SLURM_ERROR;
	}

	errno = 0;
	weight_value = strtod(value_str, &val_unit);
	if (errno) {
		error("Unable to convert %s value to double in %s",
		      __func__, value_str);
		return SLURM_ERROR;
	}

	if (val_unit && *val_unit) {
		int base_unit = slurmdb_get_tres_base_unit(type);
		int convert_val = get_convert_unit_val(base_unit, *val_unit);
		if (convert_val == SLURM_ERROR)
			return SLURM_ERROR;
		if (convert_val > 0) {
			weight_value /= convert_val;
		}
	}

	weights[tres_id] = weight_value;

	return SLURM_SUCCESS;
}

/* slurm_get_tres_weight_array
 * IN weights_str - string of tres and weights to be parsed.
 * IN tres_cnt - count of how many tres' are on the system (e.g.
 * 		slurmctld_tres_cnt).
 * IN fail - whether to fatal or not if there are parsing errors.
 * RET double* of tres weights.
 */
double *slurm_get_tres_weight_array(char *weights_str, int tres_cnt, bool fail)
{
	double *weights;
	char *tmp_str;
	char *token, *last = NULL;

	if (!weights_str || !*weights_str || !tres_cnt)
		return NULL;

	tmp_str = xstrdup(weights_str);
	weights = xcalloc(tres_cnt, sizeof(double));

	token = strtok_r(tmp_str, ",", &last);
	while (token) {
		if (_tres_weight_item(weights, token)) {
			xfree(weights);
			xfree(tmp_str);
			if (fail)
				fatal("failed to parse tres weights str '%s'",
				      weights_str);
			else
				error("failed to parse tres weights str '%s'",
				      weights_str);
			return NULL;
		}
		token = strtok_r(NULL, ",", &last);
	}
	xfree(tmp_str);
	return weights;
}

/* slurm_get_private_data
 * get private data from slurm_conf object
 */
uint16_t slurm_get_private_data(void)
{
	uint16_t private_data = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		private_data = slurmdbd_conf->private_data;
	} else {
		conf = slurm_conf_lock();
		private_data = conf->private_data;
		slurm_conf_unlock();
	}
	return private_data;
}

/* slurm_get_resume_fail_program
 * returns the ResumeFailProgram from slurm_conf object
 * RET char *    - ResumeFailProgram, MUST be xfreed by caller
 */
char *slurm_get_resume_fail_program(void)
{
	char *resume_fail_program = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		resume_fail_program = xstrdup(conf->resume_fail_program);
		slurm_conf_unlock();
	}
	return resume_fail_program;
}

/* slurm_get_resume_program
 * returns the ResumeProgram from slurm_conf object
 * RET char *    - ResumeProgram, MUST be xfreed by caller
 */
char *slurm_get_resume_program(void)
{
	char *resume_program = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		resume_program = xstrdup(conf->resume_program);
		slurm_conf_unlock();
	}
	return resume_program;
}

/* slurm_get_state_save_location
 * get state_save_location from slurm_conf object
 * RET char *   - state_save_location directory, MUST be xfreed by caller
 */
char *slurm_get_state_save_location(void)
{
	char *state_save_loc = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		state_save_loc = xstrdup(conf->state_save_location);
		slurm_conf_unlock();
	}
	return state_save_loc;
}

/* slurm_get_stepd_loc
 * get path to the slurmstepd
 *      1. configure --sbindir concatenated with slurmstepd.
 *	2. configure --prefix concatenated with /sbin/slurmstepd.
 * RET char * - absolute path to the slurmstepd, MUST be xfreed by caller
 */
extern char *slurm_get_stepd_loc(void)
{
#ifdef SBINDIR
	return xstrdup_printf("%s/slurmstepd", SBINDIR);
#elif defined SLURM_PREFIX
	return xstrdup_printf("%s/sbin/slurmstepd", SLURM_PREFIX);
#endif
}

/* slurm_get_tmp_fs
 * returns the TmpFS configuration parameter from slurm_conf object
 * RET char *    - tmp_fs, MUST be xfreed by caller
 */
extern char *slurm_get_tmp_fs(char *node_name)
{
	char *tmp_fs = NULL;
	slurm_conf_t *conf = NULL;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		if (!node_name)
			tmp_fs = xstrdup(conf->tmp_fs);
		else
			tmp_fs = slurm_conf_expand_slurmd_path(
				conf->tmp_fs, node_name);
		slurm_conf_unlock();
	}
	return tmp_fs;
}

/* slurm_get_bb_type
 * returns the BurstBufferType (bb_type) from slurm_conf object
 * RET char *    - BurstBufferType, MUST be xfreed by caller
 */
extern char *slurm_get_bb_type(void)
{
	char *bb_type = NULL;
	slurm_conf_t *conf = NULL;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		bb_type = xstrdup(conf->bb_type);
		slurm_conf_unlock();
	}
	return bb_type;
}

/* slurm_get_cluster_name
 * returns the cluster name from slurm_conf object
 * RET char *    - cluster name,  MUST be xfreed by caller
 */
char *slurm_get_cluster_name(void)
{
	char *name = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		name = xstrdup(conf->cluster_name);
		slurm_conf_unlock();
	}
	return name;
}

/* slurm_get_comm_parameters
 * returns the value of comm_param in slurm_conf object
 * RET char *    - comm parameters, MUST be xfreed by caller
 */
extern char *slurm_get_comm_parameters(void)
{
	char *comm_params = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		comm_params = xstrdup(conf->comm_params);
		slurm_conf_unlock();
	}
	return comm_params;
}


/* slurm_get_power_parameters
 * returns the PowerParameters from slurm_conf object
 * RET char *    - PowerParameters, MUST be xfreed by caller
 */
extern char *slurm_get_power_parameters(void)
{
	char *power_parameters = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		power_parameters = xstrdup(conf->power_parameters);
		slurm_conf_unlock();
	}
	return power_parameters;
}

/* slurm_set_power_parameters
 * reset the PowerParameters object
 */
extern void slurm_set_power_parameters(char *power_parameters)
{
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		xfree(conf->power_parameters);
		conf->power_parameters = xstrdup(power_parameters);
		slurm_conf_unlock();
	}
}

/* slurm_get_topology_param
 * returns the value of topology_param in slurm_conf object
 * RET char *    - topology parameters, MUST be xfreed by caller
 */
extern char * slurm_get_topology_param(void)
{
	char *topology_param = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		topology_param = xstrdup(conf->topology_param);
		slurm_conf_unlock();
	}
	return topology_param;
}

/* slurm_get_topology_plugin
 * returns the value of topology_plugin in slurm_conf object
 * RET char *    - topology type, MUST be xfreed by caller
 */
extern char * slurm_get_topology_plugin(void)
{
	char *topology_plugin = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		topology_plugin = xstrdup(conf->topology_plugin);
		slurm_conf_unlock();
	}
	return topology_plugin;
}

/* slurm_get_propagate_prio_process
 * return the PropagatePrioProcess flag from slurm_conf object
 */
extern uint16_t slurm_get_propagate_prio_process(void)
{
	uint16_t propagate_prio = 0;
	slurm_conf_t *conf;

 	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		propagate_prio = conf->propagate_prio_process;
		slurm_conf_unlock();
	}
	return propagate_prio;
}

/* slurm_get_track_wckey
 * returns the value of track_wckey in slurm_conf object
 */
extern uint16_t slurm_get_track_wckey(void)
{
	uint16_t track_wckey = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		track_wckey = slurmdbd_conf->track_wckey;
	} else {
		conf = slurm_conf_lock();
		track_wckey = conf->conf_flags & CTL_CONF_WCKEY ? 1 : 0;
		slurm_conf_unlock();
	}
	return track_wckey;
}

/* slurm_get_vsize_factor
 * returns the value of vsize_factor in slurm_conf object
 */
extern uint16_t slurm_get_vsize_factor(void)
{
	uint16_t vsize_factor = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		vsize_factor = conf->vsize_factor;
		slurm_conf_unlock();
	}
	return vsize_factor;
}

/* slurm_get_job_submit_plugins
 * get job_submit_plugins from slurm_conf object from
 * slurm_conf object
 * RET char *   - job_submit_plugins, MUST be xfreed by caller
 */
char *slurm_get_job_submit_plugins(void)
{
	char *job_submit_plugins = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		job_submit_plugins = xstrdup(conf->job_submit_plugins);
		slurm_conf_unlock();
	}
	return job_submit_plugins;
}

/* slurm_get_node_features_plugins
 * get node_features_plugins from slurm_conf object
 * RET char *   - knl_plugins, MUST be xfreed by caller
 */
char *slurm_get_node_features_plugins(void)
{
	char *knl_plugins = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		knl_plugins = xstrdup(conf->node_features_plugins);
		slurm_conf_unlock();
	}
	return knl_plugins;
}

/* slurm_get_accounting_storage_tres
 * returns the accounting storage tres from slurm_conf object
 * RET char *    - accounting storage tres,  MUST be xfreed by caller
 */
char *slurm_get_accounting_storage_tres(void)
{
	char *accounting_tres;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		accounting_tres = NULL;
	} else {
		conf = slurm_conf_lock();
		accounting_tres = xstrdup(conf->accounting_storage_tres);
		slurm_conf_unlock();
	}
	return accounting_tres;

}

/* slurm_set_accounting_storage_tres
 * sets the value of accounting_storage_tres in slurm_conf object
 * RET 0 or error_code
 */
extern int slurm_set_accounting_storage_tres(char *tres)
{
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		xfree(conf->accounting_storage_tres);
		conf->accounting_storage_tres = xstrdup(tres);
		slurm_conf_unlock();
	}
	return 0;

}

/* slurm_get_accounting_storage_user
 * returns the storage user from slurm_conf object
 * RET char *    - storage user,  MUST be xfreed by caller
 */
char *slurm_get_accounting_storage_user(void)
{
	char *storage_user;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		storage_user = xstrdup(slurmdbd_conf->storage_user);
	} else {
		conf = slurm_conf_lock();
		storage_user = xstrdup(conf->accounting_storage_user);
		slurm_conf_unlock();
	}
	return storage_user;
}

/* slurm_get_accounting_storage_backup_host
 * returns the storage backup host from slurm_conf object
 * RET char *    - storage backup host,  MUST be xfreed by caller
 */
char *slurm_get_accounting_storage_backup_host(void)
{
	char *storage_host;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		storage_host = xstrdup(slurmdbd_conf->storage_backup_host);
	} else {
		conf = slurm_conf_lock();
		storage_host = xstrdup(conf->accounting_storage_backup_host);
		slurm_conf_unlock();
	}
	return storage_host;
}

char *slurm_get_accounting_storage_ext_host(void)
{
	char *ext_host = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		ext_host = xstrdup(conf->accounting_storage_ext_host);
		slurm_conf_unlock();
	}
	return ext_host;
}

/* slurm_get_accounting_storage_host
 * returns the storage host from slurm_conf object
 * RET char *    - storage host,  MUST be xfreed by caller
 */
char *slurm_get_accounting_storage_host(void)
{
	char *storage_host;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		storage_host = xstrdup(slurmdbd_conf->storage_host);
	} else {
		conf = slurm_conf_lock();
		storage_host = xstrdup(conf->accounting_storage_host);
		slurm_conf_unlock();
	}
	return storage_host;
}

/* slurm_get_accounting_storage_loc
 * returns the storage location from slurm_conf object
 * RET char *    - storage location,  MUST be xfreed by caller
 */
char *slurm_get_accounting_storage_loc(void)
{
	char *storage_loc;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		storage_loc = xstrdup(slurmdbd_conf->storage_loc);
	} else {
		conf = slurm_conf_lock();
		storage_loc = xstrdup(conf->accounting_storage_loc);
		slurm_conf_unlock();
	}
	return storage_loc;
}

/* slurm_set_accounting_storage_loc
 * IN: char *loc (name of file or database)
 * RET 0 or error code
 */
int slurm_set_accounting_storage_loc(char *loc)
{
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		xfree(slurmdbd_conf->storage_loc);
		slurmdbd_conf->storage_loc = xstrdup(loc);
	} else {
		conf = slurm_conf_lock();
		xfree(conf->accounting_storage_loc);
		conf->accounting_storage_loc = xstrdup(loc);
		slurm_conf_unlock();
	}
	return 0;
}

/* slurm_get_accounting_storage_enforce
 * returns what level to enforce associations at
 */
uint16_t slurm_get_accounting_storage_enforce(void)
{
	uint16_t enforce = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		enforce = conf->accounting_storage_enforce;
		slurm_conf_unlock();
	}
	return enforce;

}

/* slurm_with_slurmdbd
 * returns true if operating with slurmdbd
 */
bool slurm_with_slurmdbd(void)
{
	bool with_slurmdbd;
	slurm_conf_t *conf = slurm_conf_lock();
	with_slurmdbd = !xstrcasecmp(conf->accounting_storage_type,
	                             "accounting_storage/slurmdbd");
	slurm_conf_unlock();
	return with_slurmdbd;
}

/* slurm_get_accounting_storage_pass
 * returns the storage password from slurm_conf object
 * RET char *    - storage password,  MUST be xfreed by caller
 */
char *slurm_get_accounting_storage_pass(void)
{
	char *storage_pass;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
		storage_pass = xstrdup(slurmdbd_conf->storage_pass);
	} else {
		conf = slurm_conf_lock();
		storage_pass = xstrdup(conf->accounting_storage_pass);
		slurm_conf_unlock();
	}
	return storage_pass;
}

/*
 * Convert AuthInfo to a socket path. Accepts two input formats:
 * 1) <path>		(Old format)
 * 2) socket=<path>[,]	(New format)
 * NOTE: Caller must xfree return value
 */
extern char *slurm_auth_opts_to_socket(char *opts)
{
	char *socket = NULL, *sep, *tmp;

	if (!opts)
		return NULL;

	tmp = strstr(opts, "socket=");
	if (tmp) {
		/* New format */
		socket = xstrdup(tmp + 7);
		sep = strchr(socket, ',');
		if (sep)
			sep[0] = '\0';
	} else if (strchr(opts, '=')) {
		/* New format, but socket not specified */
		;
	} else {
		/* Old format */
		socket = xstrdup(opts);
	}

	return socket;
}

/* slurm_get_auth_ttl
 * returns the credential Time To Live option from the AuthInfo parameter
 * cache value in local buffer for best performance
 * RET int - Time To Live in seconds or 0 if not specified
 */
extern int slurm_get_auth_ttl(void)
{
	static int ttl = -1;
	char *tmp;

	if (ttl >= 0)
		return ttl;

	if (!slurm_conf.authinfo)
		return 0;

	tmp = strstr(slurm_conf.authinfo, "ttl=");
	if (tmp) {
		ttl = atoi(tmp + 4);
		if (ttl < 0)
			ttl = 0;
	} else {
		ttl = 0;
	}

	return ttl;
}

/* _global_auth_key
 * returns the storage password from slurm_conf or slurmdbd_conf object
 * cache value in local buffer for best performance
 * RET char *    - storage password
 */
static char *_global_auth_key(void)
{
	static bool loaded_storage_pass = false;
	static char storage_pass[512] = "\0";
	static char *storage_pass_ptr = NULL;

	if (loaded_storage_pass)
		return storage_pass_ptr;

	if (slurmdbd_conf) {
		if (slurm_conf.authinfo) {
			if (strlcpy(storage_pass, slurm_conf.authinfo,
				    sizeof(storage_pass))
			    >= sizeof(storage_pass))
				fatal("AuthInfo is too long");
			storage_pass_ptr = storage_pass;
		}
	} else {
		slurm_conf_t *conf = slurm_conf_lock();
		if (conf->accounting_storage_pass) {
			if (strlcpy(storage_pass, conf->accounting_storage_pass,
				    sizeof(storage_pass))
			    >= sizeof(storage_pass))
				fatal("AccountingStoragePass is too long");
			storage_pass_ptr = storage_pass;
		}
		slurm_conf_unlock();
	}

	loaded_storage_pass = true;
	return storage_pass_ptr;
}

/*
 * slurm_get_dependency_params
 * RET dependency_params must be xfreed by caller
 */
char *slurm_get_dependency_params(void)
{
	char *dependency_params = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		dependency_params = xstrdup(conf->dependency_params);
		slurm_conf_unlock();
	}
	return dependency_params;
}

/* slurm_get_preempt_mode
 * returns the PreemptMode value from slurm_conf object
 * RET uint16_t   - PreemptMode value (See PREEMPT_MODE_* in slurm.h)
 */
uint16_t slurm_get_preempt_mode(void)
{
	uint16_t preempt_mode = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		preempt_mode = conf->preempt_mode;
		slurm_conf_unlock();
	}
	return preempt_mode;
}

/* slurm_get_energy_accounting_type
 * get EnergyAccountingType from slurm_conf object
 * RET char *   - energy_accounting type, MUST be xfreed by caller
 */
char *slurm_get_acct_gather_energy_type(void)
{
	char *acct_gather_energy_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		acct_gather_energy_type =
			xstrdup(conf->acct_gather_energy_type);
		slurm_conf_unlock();
	}
	return acct_gather_energy_type;
}

/* slurm_get_profile_accounting_type
 * get ProfileAccountingType from slurm_conf object
 * RET char *   - profile_accounting type, MUST be xfreed by caller
 */
char *slurm_get_acct_gather_profile_type(void)
{
	char *acct_gather_profile_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		acct_gather_profile_type =
			xstrdup(conf->acct_gather_profile_type);
		slurm_conf_unlock();
	}
	return acct_gather_profile_type;
}

/* slurm_get_interconnect_accounting_type
 * get InterconnectAccountingType from slurm_conf object
 * RET char *   - interconnect_accounting type, MUST be xfreed by caller
 */
char *slurm_get_acct_gather_interconnect_type(void)
{
	char *acct_gather_interconnect_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		acct_gather_interconnect_type =
			xstrdup(conf->acct_gather_interconnect_type);
		slurm_conf_unlock();
	}
	return acct_gather_interconnect_type;
}

/* slurm_get_filesystem_accounting_type
 * get FilesystemAccountingType from slurm_conf object
 * RET char *   - filesystem_accounting type, MUST be xfreed by caller
 */
char *slurm_get_acct_gather_filesystem_type(void)
{
	char *acct_gather_filesystem_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		acct_gather_filesystem_type =
			xstrdup(conf->acct_gather_filesystem_type);
		slurm_conf_unlock();
	}
	return acct_gather_filesystem_type;
}


extern uint16_t slurm_get_acct_gather_node_freq(void)
{
	uint16_t freq = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		freq = conf->acct_gather_node_freq;
		slurm_conf_unlock();
	}
	return freq;
}

/* slurm_get_ext_sensors_type
 * get ExtSensorsType from slurm_conf object
 * RET char *   - ext_sensors type, MUST be xfreed by caller
 */
char *slurm_get_ext_sensors_type(void)
{
	char *ext_sensors_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		ext_sensors_type =
			xstrdup(conf->ext_sensors_type);
		slurm_conf_unlock();
	}
	return ext_sensors_type;
}

extern uint16_t slurm_get_ext_sensors_freq(void)
{
	uint16_t freq = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		freq = conf->ext_sensors_freq;
		slurm_conf_unlock();
	}
	return freq;
}

/*
 * returns the configured GpuFreqDef value
 * RET char *    - GpuFreqDef value,  MUST be xfreed by caller
 */
char *slurm_get_gpu_freq_def(void)
{
	char *gpu_freq_def = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		gpu_freq_def = xstrdup(conf->gpu_freq_def);
		slurm_conf_unlock();
	}
	return gpu_freq_def;
}

/*
 * slurm_get_jobcomp_type
 * returns the job completion logger type from slurm_conf object
 * RET char *    - job completion type,  MUST be xfreed by caller
 */
char *slurm_get_jobcomp_type(void)
{
	char *jobcomp_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		jobcomp_type = xstrdup(conf->job_comp_type);
		slurm_conf_unlock();
	}
	return jobcomp_type;
}

/* slurm_get_jobcomp_loc
 * returns the job completion loc from slurm_conf object
 * RET char *    - job completion location,  MUST be xfreed by caller
 */
char *slurm_get_jobcomp_loc(void)
{
	char *jobcomp_loc = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		jobcomp_loc = xstrdup(conf->job_comp_loc);
		slurm_conf_unlock();
	}
	return jobcomp_loc;
}

/* slurm_get_jobcomp_user
 * returns the storage user from slurm_conf object
 * RET char *    - storage user,  MUST be xfreed by caller
 */
char *slurm_get_jobcomp_user(void)
{
	char *storage_user = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		storage_user = xstrdup(conf->job_comp_user);
		slurm_conf_unlock();
	}
	return storage_user;
}

/* slurm_get_jobcomp_host
 * returns the storage host from slurm_conf object
 * RET char *    - storage host,  MUST be xfreed by caller
 */
char *slurm_get_jobcomp_host(void)
{
	char *storage_host = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		storage_host = xstrdup(conf->job_comp_host);
		slurm_conf_unlock();
	}
	return storage_host;
}

char *slurm_get_jobcomp_params(void)
{
	char *param = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		param = xstrdup(conf->job_comp_params);
		slurm_conf_unlock();
	}
	return param;
}

/* slurm_get_jobcomp_pass
 * returns the storage password from slurm_conf object
 * RET char *    - storage password,  MUST be xfreed by caller
 */
char *slurm_get_jobcomp_pass(void)
{
	char *storage_pass = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		storage_pass = xstrdup(conf->job_comp_pass);
		slurm_conf_unlock();
	}
	return storage_pass;
}

/* slurm_get_jobcomp_port
 * returns the storage port from slurm_conf object
 * RET uint32_t   - storage port
 */
uint32_t slurm_get_jobcomp_port(void)
{
	uint32_t storage_port = 0;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		storage_port = conf->job_comp_port;
		slurm_conf_unlock();
	}
	return storage_port;

}

/* slurm_set_jobcomp_port
 * sets the jobcomp port in slurm_conf object
 * RET 0 or error code
 */
int slurm_set_jobcomp_port(uint32_t port)
{
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		if (port == 0) {
			error("can't have jobcomp port of 0");
			return SLURM_ERROR;
		}

		conf->job_comp_port = port;
		slurm_conf_unlock();
	}
	return 0;
}

/* slurm_get_keep_alive_time
 * returns keep_alive_time slurm_conf object
 * RET uint16_t	- keep_alive_time
 */
uint16_t slurm_get_keep_alive_time(void)
{
	uint16_t keep_alive_time = NO_VAL16;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		keep_alive_time = conf->keep_alive_time;
		slurm_conf_unlock();
	}
	return keep_alive_time;
}


/* slurm_get_mcs_plugin
 * RET mcs_plugin name, must be xfreed by caller */
char *slurm_get_mcs_plugin(void)
{
	char *mcs_plugin = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		mcs_plugin = xstrdup(conf->mcs_plugin);
		slurm_conf_unlock();
	}
	return mcs_plugin;
}

/* slurm_get_mcs_plugin_params
 * RET mcs_plugin_params name, must be xfreed by caller */
char *slurm_get_mcs_plugin_params(void)
{
	char *mcs_plugin_params = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		mcs_plugin_params = xstrdup(conf->mcs_plugin_params);
		slurm_conf_unlock();
	}
	return mcs_plugin_params;
}

/* slurm_get_preempt_type
 * get PreemptType from slurm_conf object
 * RET char *   - preempt type, MUST be xfreed by caller
 */
char *slurm_get_preempt_type(void)
{
	char *preempt_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		preempt_type = xstrdup(conf->preempt_type);
		slurm_conf_unlock();
	}
	return preempt_type;
}

/* slurm_get_proctrack_type
 * get ProctrackType from slurm_conf object
 * RET char *   - proctrack type, MUST be xfreed by caller
 */
char *slurm_get_proctrack_type(void)
{
	char *proctrack_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		proctrack_type = xstrdup(conf->proctrack_type);
		slurm_conf_unlock();
	}
	return proctrack_type;
}

/* slurm_get_sched_params
 * RET char * - Value of SchedulerParameters, MUST be xfreed by caller */
extern char *slurm_get_sched_params(void)
{
	char *params = 0;
	slurm_conf_t *conf;

 	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		params = xstrdup(conf->sched_params);
		slurm_conf_unlock();
	}
	return params;
}

/* slurm_get_select_type
 * get select_type from slurm_conf object
 * RET char *   - select_type, MUST be xfreed by caller
 */
char *slurm_get_select_type(void)
{
	char *select_type = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		select_type = xstrdup(conf->select_type);
		slurm_conf_unlock();
	}
	return select_type;
}

/* slurm_set_select_type_param
 * set select_type_param for slurm_conf object
 * IN uint16_t   - select_type_param
 */
void slurm_set_select_type_param(uint16_t select_type_param)
{
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		conf->select_type_param = select_type_param;
		slurm_conf_unlock();
	}
}

/** Return true if (remote) system runs Cray Aries */
bool is_cray_select_type(void)
{
	bool result = false;

	if (slurmdbd_conf) {
	} else {
		slurm_conf_t *conf = slurm_conf_lock();
		result = !xstrcasecmp(conf->select_type, "select/cray_aries");
		slurm_conf_unlock();
	}
	return result;
}

/* slurm_get_srun_prolog
 * return the name of the srun prolog program
 * RET char *   - name of prolog program, must be xfreed by caller
 */
char *slurm_get_srun_prolog(void)
{
	char *prolog = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		prolog = xstrdup(conf->srun_prolog);
		slurm_conf_unlock();
	}
	return prolog;
}

/* slurm_get_srun_epilog
 * return the name of the srun epilog program
 * RET char *   - name of epilog program, must be xfreed by caller
 */
char *slurm_get_srun_epilog(void)
{
	char *epilog = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		epilog = xstrdup(conf->srun_epilog);
		slurm_conf_unlock();
	}
	return epilog;
}

/*  slurm_get_srun_port_range()
 */
uint16_t *
slurm_get_srun_port_range(void)
{
	uint16_t *ports = NULL;
	slurm_conf_t *conf;

	if (slurmdbd_conf) {
	} else {
		conf = slurm_conf_lock();
		ports = conf->srun_port_range;
		slurm_conf_unlock();
	}
	return ports;	/* CLANG false positive */
}

/* slurm_get_core_spec_plugin
 * RET core_spec plugin name, must be xfreed by caller */
char *slurm_get_core_spec_plugin(void)
{
	char *core_spec_plugin = NULL;
	slurm_conf_t *conf;

	conf = slurm_conf_lock();
	core_spec_plugin = xstrdup(conf->core_spec_plugin);
	slurm_conf_unlock();
	return core_spec_plugin;
}

/* slurm_get_job_container_plugin
 * RET job_container plugin name, must be xfreed by caller */
char *slurm_get_job_container_plugin(void)
{
	char *job_container_plugin = NULL;
	slurm_conf_t *conf;

	conf = slurm_conf_lock();
	job_container_plugin = xstrdup(conf->job_container_plugin);
	slurm_conf_unlock();
	return job_container_plugin;
}

/* Change general slurm communication errors to slurmctld specific errors */
static void _remap_slurmctld_errno(void)
{
	int err = slurm_get_errno();

	if (err == SLURM_COMMUNICATIONS_CONNECTION_ERROR)
		slurm_seterrno(SLURMCTLD_COMMUNICATIONS_CONNECTION_ERROR);
	else if (err ==  SLURM_COMMUNICATIONS_SEND_ERROR)
		slurm_seterrno(SLURMCTLD_COMMUNICATIONS_SEND_ERROR);
	else if (err == SLURM_COMMUNICATIONS_RECEIVE_ERROR)
		slurm_seterrno(SLURMCTLD_COMMUNICATIONS_RECEIVE_ERROR);
	else if (err == SLURM_COMMUNICATIONS_SHUTDOWN_ERROR)
		slurm_seterrno(SLURMCTLD_COMMUNICATIONS_SHUTDOWN_ERROR);
}

/**********************************************************************\
 * general message management functions used by slurmctld, slurmd
\**********************************************************************/

/* In the socket implementation it creates a socket, binds to it, and
 *	listens for connections. Retry if bind() or listen() fail
 *      even if asked for an ephemeral port.
 *
 * IN  port     - port to bind the msg server to
 * RET int      - file descriptor of the connection created
 */
int slurm_init_msg_engine_port(uint16_t port)
{
	int cc;
	slurm_addr_t addr;
	int i;

	slurm_setup_sockaddr(&addr, port);
	cc = slurm_init_msg_engine(&addr);
	if ((cc < 0) && (port == 0) && (errno == EADDRINUSE)) {
		/* All ephemeral ports are in use, test other ports */
		for (i = 10001; i < 65536; i++) {
			slurm_setup_sockaddr(&addr, i);
			cc = slurm_init_msg_engine(&addr);
			if (cc >= 0)
				break;
		}
	}
	return cc;
}

/* slurm_init_msg_engine_ports()
 */
int slurm_init_msg_engine_ports(uint16_t *ports)
{
	int cc;
	int val;
	int s;
	int port;

	s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0)
		return -1;

	val = 1;
	cc = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(int));
	if (cc < 0) {
		close(s);
		return -1;
	}

	port = sock_bind_range(s, ports, false);
	if (port < 0) {
		close(s);
		return -1;
	}

	cc = listen(s, SLURM_DEFAULT_LISTEN_BACKLOG);
	if (cc < 0) {
		close(s);
		return -1;
	}

	return s;
}

/**********************************************************************\
 * msg connection establishment functions used by msg clients
\**********************************************************************/

/* In the bsd socket implementation it creates a SOCK_STREAM socket
 *	and calls connect on it a SOCK_DGRAM socket called with connect
 *	is defined to only receive messages from the address/port pair
 *	argument of the connect call slurm_address - for now it is
 *	really just a sockaddr_in
 * IN slurm_address	- slurm_addr_t of the connection destination
 * RET slurm_fd		- file descriptor of the connection created
 */
int slurm_open_msg_conn(slurm_addr_t * slurm_address)
{
	int fd = slurm_open_stream(slurm_address, false);
	if (fd >= 0)
		fd_set_close_on_exec(fd);
	return fd;
}

/*
 * Calls connect to make a connection-less datagram connection 
 *	primary or secondary slurmctld message engine
 * IN/OUT addr       - address of controller contacted
 * IN/OUT use_backup - IN: whether to try the backup first or not
 *                     OUT: set to true if connection established with backup
 * IN comm_cluster_rec	- Communication record (host/port/version)/
 * RET slurm_fd	- file descriptor of the connection created
 */
extern int slurm_open_controller_conn(slurm_addr_t *addr, bool *use_backup,
				      slurmdb_cluster_rec_t *comm_cluster_rec)
{
	int fd = -1;
	slurm_protocol_config_t *proto_conf = NULL;
	int i, retry, max_retry_period;

	if (!comm_cluster_rec) {
		/* This means the addr wasn't set up already */
		if (!(proto_conf = _slurm_api_get_comm_config()))
			return SLURM_ERROR;

		for (i = 0; i < proto_conf->control_cnt; i++) {
			proto_conf->controller_addr[i].sin_port =
				htons(slurm_conf.slurmctld_port +
				(((time(NULL) + getpid()) %
				 slurm_conf.slurmctld_port_count)));
		}

		if (proto_conf->vip_addr_set) {
			proto_conf->vip_addr.sin_port =
				htons(slurm_conf.slurmctld_port +
				(((time(NULL) + getpid()) %
				 slurm_conf.slurmctld_port_count)));
		}
	}

#ifdef HAVE_NATIVE_CRAY
	max_retry_period = 180;
#else
	max_retry_period = slurm_conf.msg_timeout;
#endif
	for (retry = 0; retry < max_retry_period; retry++) {
		if (retry)
			sleep(1);
		if (comm_cluster_rec) {
			if (comm_cluster_rec->control_addr.sin_port == 0) {
				slurm_set_addr(
					&comm_cluster_rec->control_addr,
					comm_cluster_rec->control_port,
					comm_cluster_rec->control_host);
			}
			addr = &comm_cluster_rec->control_addr;

			fd = slurm_open_msg_conn(addr);
			if (fd >= 0)
				goto end_it;
			log_flag(NET, "%s: Failed to contact controller: %m",
				 __func__);
		} else if (proto_conf->vip_addr_set) {
			fd = slurm_open_msg_conn(&proto_conf->vip_addr);
			if (fd >= 0)
				goto end_it;
			log_flag(NET, "%s: Failed to contact controller: %m",
				 __func__);
		} else {
			if (!*use_backup) {
				fd = slurm_open_msg_conn(
						&proto_conf->controller_addr[0]);
				if (fd >= 0) {
					*use_backup = false;
					goto end_it;
				}
				log_flag(NET,"%s: Failed to contact primary controller: %m",
					 __func__);
			}
			if ((proto_conf->control_cnt > 1) || *use_backup) {
				for (i = 1; i < proto_conf->control_cnt; i++) {
					fd = slurm_open_msg_conn(
						&proto_conf->controller_addr[i]);
					if (fd >= 0) {
						log_flag(NET, "%s: Contacted backup controller attempt:%d",
							 __func__, (i - 1));
						*use_backup = true;
						goto end_it;
					}
				}
				*use_backup = false;
				log_flag(NET, "%s: Failed to contact backup controller: %m",
					 __func__);
			}
		}
	}
	addr = NULL;
	_slurm_api_free_comm_config(proto_conf);
	slurm_seterrno_ret(SLURMCTLD_COMMUNICATIONS_CONNECTION_ERROR);

end_it:
	_slurm_api_free_comm_config(proto_conf);
	return fd;
}

/*
 * Calls connect to make a connection-less datagram connection to a specific
 *	primary or backup slurmctld message engine
 * IN dest      - controller to contact (0=primary, 1=backup, 2=backup2, etc.)
 * IN comm_cluster_rec	- Communication record (host/port/version)/
 * RET int      - file descriptor of the connection created
 */
extern int slurm_open_controller_conn_spec(int dest,
				      slurmdb_cluster_rec_t *comm_cluster_rec)
{
	slurm_protocol_config_t *proto_conf = NULL;
	slurm_addr_t *addr;
	int rc;

	if (comm_cluster_rec) {
		if (comm_cluster_rec->control_addr.sin_port == 0) {
			slurm_set_addr(
				&comm_cluster_rec->control_addr,
				comm_cluster_rec->control_port,
				comm_cluster_rec->control_host);
		}
		addr = &comm_cluster_rec->control_addr;
	} else {	/* Some backup slurmctld */
		if (!(proto_conf = _slurm_api_get_comm_config())) {
			debug3("Error: Unable to set default config");
			return SLURM_ERROR;
		}
		addr = NULL;
		if ((dest >= 0) && (dest <= proto_conf->control_cnt))
			addr = &proto_conf->controller_addr[dest];
		if (!addr) {
			rc = SLURM_ERROR;
			goto fini;
		}
	}

	rc = slurm_open_msg_conn(addr);
	if (rc == -1)
		_remap_slurmctld_errno();
fini:	_slurm_api_free_comm_config(proto_conf);
	return rc;
}

extern int slurm_unpack_received_msg(slurm_msg_t *msg, int fd, Buf buffer)
{
	header_t header;
	int rc;
	void *auth_cred = NULL;

	if (unpack_header(&header, buffer) == SLURM_ERROR) {
		rc = SLURM_COMMUNICATIONS_RECEIVE_ERROR;
		goto total_return;
	}

	if (check_header_version(&header) < 0) {
		slurm_addr_t resp_addr;
		char addr_str[32];
		int uid = _unpack_msg_uid(buffer, header.version);

		if (!slurm_get_peer_addr(fd, &resp_addr)) {
			slurm_print_slurm_addr(
				&resp_addr, addr_str, sizeof(addr_str));
			error("%s: Invalid Protocol Version %u from uid=%d at %s",
			      __func__, header.version, uid, addr_str);
		} else {
			error("%s: Invalid Protocol Version %u from uid=%d from "
			      "problem connection: %m", __func__,
			      header.version, uid);
		}

		rc = SLURM_PROTOCOL_VERSION_ERROR;
		goto total_return;
	}
	//info("ret_cnt = %d",header.ret_cnt);
	if (header.ret_cnt > 0) {
		error("%s: we received more than one message back use "
		      "slurm_receive_msgs instead", __func__);
		header.ret_cnt = 0;
		FREE_NULL_LIST(header.ret_list);
		header.ret_list = NULL;
	}

	/* Forward message to other nodes */
	if (header.forward.cnt > 0) {
		error("%s: We need to forward this to other nodes use "
		      "slurm_receive_msg_and_forward instead", __func__);
	}

	if ((auth_cred = g_slurm_auth_unpack(buffer, header.version)) == NULL) {
		error("%s: g_slurm_auth_unpack: %s has authentication error: %m",
		      __func__, rpc_num2string(header.msg_type));
		rc = ESLURM_PROTOCOL_INCOMPLETE_PACKET;
		goto total_return;
	}
	msg->auth_index = slurm_auth_index(auth_cred);
	if (header.flags & SLURM_GLOBAL_AUTH_KEY) {
		rc = g_slurm_auth_verify(auth_cred, _global_auth_key());
	} else {
		rc = g_slurm_auth_verify(auth_cred, slurm_conf.authinfo);
	}

	if (rc != SLURM_SUCCESS) {
		error("%s: g_slurm_auth_verify: %s has authentication error: %s",
		      __func__, rpc_num2string(header.msg_type),
		      slurm_strerror(rc));
		(void) g_slurm_auth_destroy(auth_cred);
		rc = SLURM_PROTOCOL_AUTHENTICATION_ERROR;
		goto total_return;
	}

	/*
	 * Unpack message body
	 */
	msg->protocol_version = header.version;
	msg->msg_type = header.msg_type;
	msg->flags = header.flags;

	msg->body_offset =  get_buf_offset(buffer);

	if ((header.body_length > remaining_buf(buffer)) ||
	    (unpack_msg(msg, buffer) != SLURM_SUCCESS)) {
		rc = ESLURM_PROTOCOL_INCOMPLETE_PACKET;
		(void) g_slurm_auth_destroy(auth_cred);
		goto total_return;
	}

	msg->auth_cred = (void *)auth_cred;

	rc = SLURM_SUCCESS;

total_return:
	destroy_forward(&header.forward);

	slurm_seterrno(rc);
	if (rc != SLURM_SUCCESS) {
		msg->auth_cred = (void *) NULL;
		error("%s: %s", __func__, slurm_strerror(rc));
		rc = -1;
		usleep(10000);	/* Discourage brute force attack */
	} else {
		rc = 0;
	}
	return rc;
}

/**********************************************************************\
 * receive message functions
\**********************************************************************/

/*
 * NOTE: memory is allocated for the returned msg must be freed at
 *       some point using the slurm_free_functions.
 * IN fd	- file descriptor to receive msg on
 * OUT msg	- a slurm_msg struct to be filled in by the function
 * IN timeout	- how long to wait in milliseconds
 * RET int	- returns 0 on success, -1 on failure and sets errno
 */
int slurm_receive_msg(int fd, slurm_msg_t *msg, int timeout)
{
	char *buf = NULL;
	size_t buflen = 0;
	int rc;
	Buf buffer;
	bool keep_buffer = false;

	if (msg->flags & SLURM_MSG_KEEP_BUFFER)
		keep_buffer = true;

	if (msg->conn) {
		persist_msg_t persist_msg;

		buffer = slurm_persist_recv_msg(msg->conn);
		if (!buffer) {
			error("%s: No response to persist_init", __func__);
			slurm_persist_conn_close(msg->conn);
			return SLURM_ERROR;
		}
		memset(&persist_msg, 0, sizeof(persist_msg_t));
		rc = slurm_persist_msg_unpack(msg->conn, &persist_msg, buffer);

		if (keep_buffer)
			msg->buffer = buffer;
		else
			free_buf(buffer);

		if (rc) {
			error("%s: Failed to unpack persist msg", __func__);
			slurm_persist_conn_close(msg->conn);
			return SLURM_ERROR;
		}

		msg->msg_type = persist_msg.msg_type;
		msg->data = persist_msg.data;

		return SLURM_SUCCESS;
	}

	xassert(fd >= 0);

	msg->conn_fd = fd;

	if (timeout <= 0)
		/* convert secs to msec */
		timeout = slurm_conf.msg_timeout * MSEC_IN_SEC;

	else if (timeout > (slurm_conf.msg_timeout * MSEC_IN_SEC * 10)) {
		/* consider 10x the timeout to be very long */
		log_flag(NET, "%s: You are receiving a message with very long timeout of %d seconds",
			 __func__, (timeout / MSEC_IN_SEC));
	} else if (timeout < MSEC_IN_SEC) {
		/* consider a less than 1 second to be very short */
		error("%s: You are receiving a message with a very short "
		      "timeout of %d msecs", __func__, timeout);
	}

	/*
	 * Receive a msg. slurm_msg_recvfrom() will read the message
	 *  length and allocate space on the heap for a buffer containing
	 *  the message.
	 */
	if (slurm_msg_recvfrom_timeout(fd, &buf, &buflen, 0, timeout) < 0) {
		rc = errno;
		goto endit;
	}

	_log_hex(buf, buflen);
	buffer = create_buf(buf, buflen);

	rc = slurm_unpack_received_msg(msg, fd, buffer);

	if (keep_buffer)
		msg->buffer = buffer;
	else
		free_buf(buffer);

endit:
	slurm_seterrno(rc);

	return rc;
}

/*
 * NOTE: memory is allocated for the returned list
 *       and must be freed at some point using the list_destroy function.
 * IN open_fd	- file descriptor to receive msg on
 * IN steps	- how many steps down the tree we have to wait for
 * IN timeout	- how long to wait in milliseconds
 * RET List	- List containing the responses of the children (if any) we
 *		  forwarded the message to. List containing type
 *		  (ret_data_info_t).
 */
List slurm_receive_msgs(int fd, int steps, int timeout)
{
	char *buf = NULL;
	size_t buflen = 0;
	header_t header;
	int rc;
	void *auth_cred = NULL;
	slurm_msg_t msg;
	Buf buffer;
	ret_data_info_t *ret_data_info = NULL;
	List ret_list = NULL;
	int orig_timeout = timeout;

	xassert(fd >= 0);

	slurm_msg_t_init(&msg);
	msg.conn_fd = fd;

	if (timeout <= 0) {
		/* convert secs to msec */
		timeout = slurm_conf.msg_timeout * 1000;
		orig_timeout = timeout;
	}
	if (steps) {
		if (message_timeout < 0)
			message_timeout = slurm_conf.msg_timeout * 1000;
		orig_timeout = (timeout -
				(message_timeout*(steps-1)))/steps;
		steps--;
	}

	log_flag(NET, "%s: orig_timeout was %d we have %d steps and a timeout of %d",
		 __func__, orig_timeout, steps, timeout);
	/* we compare to the orig_timeout here because that is really
	 *  what we are going to wait for each step
	 */
	if (orig_timeout >= (slurm_conf.msg_timeout * 10000)) {
		log_flag(NET, "%s: Sending a message with timeout's greater than %d seconds, requested timeout is %d seconds",
			 __func__, (slurm_conf.msg_timeout * 10),
			 (timeout/1000));
	} else if (orig_timeout < 1000) {
		log_flag(NET, "%s: Sending a message with a very short timeout of %d milliseconds each step in the tree has %d milliseconds",
			 __func__, timeout, orig_timeout);
	}


	/*
	 * Receive a msg. slurm_msg_recvfrom() will read the message
	 *  length and allocate space on the heap for a buffer containing
	 *  the message.
	 */
	if (slurm_msg_recvfrom_timeout(fd, &buf, &buflen, 0, timeout) < 0) {
		forward_init(&header.forward);
		rc = errno;
		goto total_return;
	}

	_log_hex(buf, buflen);
	buffer = create_buf(buf, buflen);

	if (unpack_header(&header, buffer) == SLURM_ERROR) {
		free_buf(buffer);
		rc = SLURM_COMMUNICATIONS_RECEIVE_ERROR;
		goto total_return;
	}

	if (check_header_version(&header) < 0) {
		slurm_addr_t resp_addr;
		char addr_str[32];
		int uid = _unpack_msg_uid(buffer, header.version);
		if (!slurm_get_peer_addr(fd, &resp_addr)) {
			slurm_print_slurm_addr(
				&resp_addr, addr_str, sizeof(addr_str));
			error("Invalid Protocol Version %u from uid=%d at %s",
			      header.version, uid, addr_str);
		} else {
			error("Invalid Protocol Version %u from uid=%d from "
			      "problem connection: %m",
			      header.version, uid);
		}

		free_buf(buffer);
		rc = SLURM_PROTOCOL_VERSION_ERROR;
		goto total_return;
	}
	//info("ret_cnt = %d",header.ret_cnt);
	if (header.ret_cnt > 0) {
		if (header.ret_list)
			ret_list = header.ret_list;
		else
			ret_list = list_create(destroy_data_info);
		header.ret_cnt = 0;
		header.ret_list = NULL;
	}

	/* Forward message to other nodes */
	if (header.forward.cnt > 0) {
		error("We need to forward this to other nodes use "
		      "slurm_receive_msg_and_forward instead");
	}

	if ((auth_cred = g_slurm_auth_unpack(buffer, header.version)) == NULL) {
		error("%s: g_slurm_auth_unpack: %m", __func__);
		free_buf(buffer);
		rc = ESLURM_PROTOCOL_INCOMPLETE_PACKET;
		goto total_return;
	}
	msg.auth_index = slurm_auth_index(auth_cred);
	if (header.flags & SLURM_GLOBAL_AUTH_KEY) {
		rc = g_slurm_auth_verify(auth_cred, _global_auth_key());
	} else {
		rc = g_slurm_auth_verify(auth_cred, slurm_conf.authinfo);
	}

	if (rc != SLURM_SUCCESS) {
		error("%s: g_slurm_auth_verify: %s has authentication error: %m",
		      __func__, rpc_num2string(header.msg_type));
		(void) g_slurm_auth_destroy(auth_cred);
		free_buf(buffer);
		rc = SLURM_PROTOCOL_AUTHENTICATION_ERROR;
		goto total_return;
	}

	/*
	 * Unpack message body
	 */
	msg.protocol_version = header.version;
	msg.msg_type = header.msg_type;
	msg.flags = header.flags;

	if ((header.body_length > remaining_buf(buffer)) ||
	    (unpack_msg(&msg, buffer) != SLURM_SUCCESS)) {
		(void) g_slurm_auth_destroy(auth_cred);
		free_buf(buffer);
		rc = ESLURM_PROTOCOL_INCOMPLETE_PACKET;
		goto total_return;
	}
	g_slurm_auth_destroy(auth_cred);

	free_buf(buffer);
	rc = SLURM_SUCCESS;

total_return:
	destroy_forward(&header.forward);

	if (rc != SLURM_SUCCESS) {
		if (ret_list) {
			ret_data_info = xmalloc(sizeof(ret_data_info_t));
			ret_data_info->err = rc;
			ret_data_info->type = RESPONSE_FORWARD_FAILED;
			ret_data_info->data = NULL;
			list_push(ret_list, ret_data_info);
		}
		error("slurm_receive_msgs: %s", slurm_strerror(rc));
		usleep(10000);	/* Discourage brute force attack */
	} else {
		if (!ret_list)
			ret_list = list_create(destroy_data_info);
		ret_data_info = xmalloc(sizeof(ret_data_info_t));
		ret_data_info->err = rc;
		ret_data_info->node_name = NULL;
		ret_data_info->type = msg.msg_type;
		ret_data_info->data = msg.data;
		list_push(ret_list, ret_data_info);
	}


	errno = rc;
	return ret_list;

}

/* try to determine the UID associated with a message with different
 * message header version, return -1 if we can't tell */
static int _unpack_msg_uid(Buf buffer, uint16_t protocol_version)
{
	int uid = -1;
	void *auth_cred = NULL;

	if ((auth_cred = g_slurm_auth_unpack(buffer, protocol_version)) == NULL)
		return uid;
	if (g_slurm_auth_verify(auth_cred, slurm_conf.authinfo))
		return uid;

	uid = (int) g_slurm_auth_get_uid(auth_cred);
	g_slurm_auth_destroy(auth_cred);

	return uid;
}

/*
 * NOTE: memory is allocated for the returned msg and the returned list
 *       both must be freed at some point using the slurm_free_functions
 *       and list_destroy function.
 * IN open_fd	- file descriptor to receive msg on
 * IN/OUT msg	- a slurm_msg struct to be filled in by the function
 *		  we use the orig_addr from this var for forwarding.
 * IN timeout	- how long to wait in milliseconds
 * RET int	- returns 0 on success, -1 on failure and sets errno
 */
int slurm_receive_msg_and_forward(int fd, slurm_addr_t *orig_addr,
				  slurm_msg_t *msg, int timeout)
{
	char *buf = NULL;
	size_t buflen = 0;
	header_t header;
	int rc;
	void *auth_cred = NULL;
	Buf buffer;

	xassert(fd >= 0);

	if (msg->forward.init != FORWARD_INIT)
		slurm_msg_t_init(msg);
	/* set msg connection fd to accepted fd. This allows
	 *  possibility for slurmd_req () to close accepted connection
	 */
	msg->conn_fd = fd;
	/* this always is the connection */
	memcpy(&msg->address, orig_addr, sizeof(slurm_addr_t));

	/* where the connection originated from, this
	 * might change based on the header we receive */
	memcpy(&msg->orig_addr, orig_addr, sizeof(slurm_addr_t));

	msg->ret_list = list_create(destroy_data_info);

	if (timeout <= 0) {
		log_flag(NET, "%s: Overriding timeout of %d milliseconds to %d seconds",
			 __func__, timeout, slurm_conf.msg_timeout);
		/* convert secs to msec */
		timeout = slurm_conf.msg_timeout * 1000;
	} else if (timeout < 1000) {
		log_flag(NET, "%s: Sending a message with a very short timeout of %d milliseconds",
			 __func__, timeout);
	} else if (timeout >= (slurm_conf.msg_timeout * 10000)) {
		log_flag(NET, "%s: Sending a message with timeout's greater than %d seconds, requested timeout is %d seconds",
			 __func__, (slurm_conf.msg_timeout * 10),
			 (timeout/1000));
	}

	/*
	 * Receive a msg. slurm_msg_recvfrom() will read the message
	 *  length and allocate space on the heap for a buffer containing
	 *  the message.
	 */
	if (slurm_msg_recvfrom_timeout(fd, &buf, &buflen, 0, timeout) < 0) {
		forward_init(&header.forward);
		rc = errno;
		goto total_return;
	}

	_log_hex(buf, buflen);
	buffer = create_buf(buf, buflen);

	if (unpack_header(&header, buffer) == SLURM_ERROR) {
		free_buf(buffer);
		rc = SLURM_COMMUNICATIONS_RECEIVE_ERROR;
		goto total_return;
	}

	if (check_header_version(&header) < 0) {
		slurm_addr_t resp_addr;
		char addr_str[32];
		int uid = _unpack_msg_uid(buffer, header.version);

		if (!slurm_get_peer_addr(fd, &resp_addr)) {
			slurm_print_slurm_addr(
				&resp_addr, addr_str, sizeof(addr_str));
			error("Invalid Protocol Version %u from uid=%d at %s",
			      header.version, uid, addr_str);
		} else {
			error("Invalid Protocol Version %u from uid=%d from "
			      "problem connection: %m",
			      header.version, uid);
		}

		free_buf(buffer);
		rc = SLURM_PROTOCOL_VERSION_ERROR;
		goto total_return;
	}
	if (header.ret_cnt > 0) {
		error("we received more than one message back use "
		      "slurm_receive_msgs instead");
		header.ret_cnt = 0;
		FREE_NULL_LIST(header.ret_list);
		header.ret_list = NULL;
	}

	/*
	 * header.orig_addr will be set to where the first message
	 * came from if this is a forward else we set the
	 * header.orig_addr to our addr just in case we need to send it off.
	 */
	if (header.orig_addr.sin_addr.s_addr != 0) {
		memcpy(&msg->orig_addr, &header.orig_addr, sizeof(slurm_addr_t));
	} else {
		memcpy(&header.orig_addr, orig_addr, sizeof(slurm_addr_t));
	}

	/* Forward message to other nodes */
	if (header.forward.cnt > 0) {
		log_flag(NET, "%s: forwarding to %u nodes",
			 __func__, header.forward.cnt);
		msg->forward_struct = xmalloc(sizeof(forward_struct_t));
		slurm_mutex_init(&msg->forward_struct->forward_mutex);
		slurm_cond_init(&msg->forward_struct->notify, NULL);

		msg->forward_struct->buf_len = remaining_buf(buffer);
		msg->forward_struct->buf =
			xmalloc(msg->forward_struct->buf_len);
		memcpy(msg->forward_struct->buf,
		       &buffer->head[buffer->processed],
		       msg->forward_struct->buf_len);

		msg->forward_struct->ret_list = msg->ret_list;
		/* take out the amount of timeout from this hop */
		msg->forward_struct->timeout = header.forward.timeout;
		if (!msg->forward_struct->timeout)
			msg->forward_struct->timeout = message_timeout;
		msg->forward_struct->fwd_cnt = header.forward.cnt;

		log_flag(NET, "%s: forwarding messages to %u nodes with timeout of %d",
			 __func__, msg->forward_struct->fwd_cnt,
			 msg->forward_struct->timeout);

		if (forward_msg(msg->forward_struct, &header) == SLURM_ERROR) {
			error("%s: problem with forward msg", __func__);
		}
	}

	if ((auth_cred = g_slurm_auth_unpack(buffer, header.version)) == NULL) {
		error("%s: g_slurm_auth_unpack: %s has authentication error: %m",
		      __func__, rpc_num2string(header.msg_type));
		free_buf(buffer);
		rc = ESLURM_PROTOCOL_INCOMPLETE_PACKET;
		goto total_return;
	}
	msg->auth_index = slurm_auth_index(auth_cred);
	if (header.flags & SLURM_GLOBAL_AUTH_KEY) {
		rc = g_slurm_auth_verify(auth_cred, _global_auth_key());
	} else {
		rc = g_slurm_auth_verify(auth_cred, slurm_conf.authinfo);
	}

	if (rc != SLURM_SUCCESS) {
		error("%s: g_slurm_auth_verify: %s has authentication error: %m",
		      __func__, rpc_num2string(header.msg_type));
		(void) g_slurm_auth_destroy(auth_cred);
		free_buf(buffer);
		rc = SLURM_PROTOCOL_AUTHENTICATION_ERROR;
		goto total_return;
	}

	/*
	 * Unpack message body
	 */
	msg->protocol_version = header.version;
	msg->msg_type = header.msg_type;
	msg->flags = header.flags;

	if (header.msg_type == MESSAGE_COMPOSITE) {
		slurm_send_rc_msg(msg, SLURM_SUCCESS);
		msg_aggr_add_comp(buffer, auth_cred, &header);
		goto total_return;
	}

	if ( (header.body_length > remaining_buf(buffer)) ||
	     (unpack_msg(msg, buffer) != SLURM_SUCCESS) ) {
		(void) g_slurm_auth_destroy(auth_cred);
		free_buf(buffer);
		rc = ESLURM_PROTOCOL_INCOMPLETE_PACKET;
		goto total_return;
	}
	msg->auth_cred = (void *) auth_cred;

	free_buf(buffer);
	rc = SLURM_SUCCESS;

total_return:
	destroy_forward(&header.forward);

	slurm_seterrno(rc);
	if (rc != SLURM_SUCCESS) {
		msg->msg_type = RESPONSE_FORWARD_FAILED;
		msg->auth_cred = (void *) NULL;
		msg->data = NULL;
		error("slurm_receive_msg_and_forward: %s",
		      slurm_strerror(rc));
		usleep(10000);	/* Discourage brute force attack */
	} else {
		rc = 0;
	}
	return rc;

}

/**********************************************************************\
 * send message functions
\**********************************************************************/

/*
 *  Do the wonderful stuff that needs be done to pack msg
 *  and hdr into buffer
 */
static void
_pack_msg(slurm_msg_t *msg, header_t *hdr, Buf buffer)
{
	unsigned int tmplen, msglen;

	tmplen = get_buf_offset(buffer);
	pack_msg(msg, buffer);
	msglen = get_buf_offset(buffer) - tmplen;

	/* update header with correct cred and msg lengths */
	update_header(hdr, msglen);

	/* repack updated header */
	tmplen = get_buf_offset(buffer);
	set_buf_offset(buffer, 0);
	pack_header(hdr, buffer);
	set_buf_offset(buffer, tmplen);
}

/*
 *  Send a slurm message over an open file descriptor `fd'
 *    Returns the size of the message sent in bytes, or -1 on failure.
 */
int slurm_send_node_msg(int fd, slurm_msg_t * msg)
{
	header_t header;
	Buf      buffer;
	int      rc;
	void *   auth_cred;
	time_t   start_time = time(NULL);

	if (msg->conn) {
		persist_msg_t persist_msg;

		memset(&persist_msg, 0, sizeof(persist_msg_t));
		persist_msg.msg_type  = msg->msg_type;
		persist_msg.data      = msg->data;
		persist_msg.data_size = msg->data_size;

		buffer = slurm_persist_msg_pack(msg->conn, &persist_msg);
		if (!buffer)    /* pack error */
			return SLURM_ERROR;

		rc = slurm_persist_send_msg(msg->conn, buffer);
		free_buf(buffer);

		if ((rc < 0) && (errno == ENOTCONN)) {
			log_flag(NET, "%s: persistent connection has disappeared for msg_type=%u",
				 __func__, msg->msg_type);
		} else if (rc < 0) {
			slurm_addr_t peer_addr;
			char addr_str[32];
			if (!slurm_get_peer_addr(msg->conn->fd, &peer_addr)) {
				slurm_print_slurm_addr(
					&peer_addr, addr_str, sizeof(addr_str));
				error("slurm_persist_send_msg: address:port=%s msg_type=%u: %m",
				      addr_str, msg->msg_type);
			} else
				error("slurm_persist_send_msg: msg_type=%u: %m",
				      msg->msg_type);
		}

		return rc;
	}

	/*
	 * Initialize header with Auth credential and message type.
	 * We get the credential now rather than later so the work can
	 * can be done in parallel with waiting for message to forward,
	 * but we may need to generate the credential again later if we
	 * wait too long for the incoming message.
	 */
	if (msg->flags & SLURM_GLOBAL_AUTH_KEY) {
		auth_cred = g_slurm_auth_create(msg->auth_index,
						_global_auth_key());
	} else {
		auth_cred = g_slurm_auth_create(msg->auth_index,
						slurm_conf.authinfo);
	}

	if (msg->forward.init != FORWARD_INIT) {
		forward_init(&msg->forward);
		msg->ret_list = NULL;
	}

	if (!msg->forward.tree_width)
		msg->forward.tree_width = slurm_conf.tree_width;

	forward_wait(msg);

	if (difftime(time(NULL), start_time) >= 60) {
		(void) g_slurm_auth_destroy(auth_cred);
		if (msg->flags & SLURM_GLOBAL_AUTH_KEY) {
			auth_cred = g_slurm_auth_create(msg->auth_index,
							_global_auth_key());
		} else {
			auth_cred = g_slurm_auth_create(msg->auth_index,
							slurm_conf.authinfo);
		}
	}
	if (auth_cred == NULL) {
		error("%s: g_slurm_auth_create: %s has authentication error: %m",
		      __func__, rpc_num2string(msg->msg_type));
		slurm_seterrno_ret(SLURM_PROTOCOL_AUTHENTICATION_ERROR);
	}

	init_header(&header, msg, msg->flags);

	/*
	 * Pack header into buffer for transmission
	 */
	buffer = init_buf(BUF_SIZE);
	pack_header(&header, buffer);

	/*
	 * Pack auth credential
	 */
	rc = g_slurm_auth_pack(auth_cred, buffer, header.version);
	(void) g_slurm_auth_destroy(auth_cred);
	if (rc) {
		error("%s: g_slurm_auth_pack: %s has  authentication error: %m",
		      __func__, rpc_num2string(header.msg_type));
		free_buf(buffer);
		slurm_seterrno_ret(SLURM_PROTOCOL_AUTHENTICATION_ERROR);
	}

	/*
	 * Pack message into buffer
	 */
	_pack_msg(msg, &header, buffer);
	_log_hex(get_buf_data(buffer), get_buf_offset(buffer));

	/*
	 * Send message
	 */
	rc = slurm_msg_sendto(fd, get_buf_data(buffer),
			      get_buf_offset(buffer));

	if ((rc < 0) && (errno == ENOTCONN)) {
		log_flag(NET, "%s: peer has disappeared for msg_type=%u",
			 __func__, msg->msg_type);
	} else if (rc < 0) {
		slurm_addr_t peer_addr;
		char addr_str[32];
		if (!slurm_get_peer_addr(fd, &peer_addr)) {
			slurm_print_slurm_addr(
				&peer_addr, addr_str, sizeof(addr_str));
			error("slurm_msg_sendto: address:port=%s "
			      "msg_type=%u: %m",
			      addr_str, msg->msg_type);
		} else if (errno == ENOTCONN) {
			log_flag(NET, "%s: peer has disappeared for msg_type=%u",
				 __func__, msg->msg_type);
		} else
			error("slurm_msg_sendto: msg_type=%u: %m",
			      msg->msg_type);
	}

	free_buf(buffer);
	return rc;
}

/**********************************************************************\
 * stream functions
\**********************************************************************/

/* slurm_write_stream
 * writes a buffer out a stream file descriptor
 * IN open_fd		- file descriptor to write on
 * IN buffer		- buffer to send
 * IN size		- size of buffer send
 * IN timeout		- how long to wait in milliseconds
 * RET size_t		- bytes sent , or -1 on errror
 */
size_t slurm_write_stream(int open_fd, char *buffer, size_t size)
{
	return slurm_send_timeout(open_fd, buffer, size,
	                          SLURM_PROTOCOL_NO_SEND_RECV_FLAGS,
	                          (slurm_conf.msg_timeout * 1000));
}
size_t slurm_write_stream_timeout(int open_fd, char *buffer,
				  size_t size, int timeout)
{
	return slurm_send_timeout(open_fd, buffer, size,
	                          SLURM_PROTOCOL_NO_SEND_RECV_FLAGS,
	                          timeout);
}

/* slurm_read_stream
 * read into buffer grom a stream file descriptor
 * IN open_fd	- file descriptor to read from
 * OUT buffer   - buffer to receive into
 * IN size	- size of buffer
 * IN timeout	- how long to wait in milliseconds
 * RET size_t	- bytes read , or -1 on errror
 */
size_t slurm_read_stream(int open_fd, char *buffer, size_t size)
{
	return slurm_recv_timeout(open_fd, buffer, size,
	                          SLURM_PROTOCOL_NO_SEND_RECV_FLAGS,
	                          (slurm_conf.msg_timeout * 1000));
}
size_t slurm_read_stream_timeout(int open_fd, char *buffer,
				 size_t size, int timeout)
{
	return slurm_recv_timeout(open_fd, buffer, size,
	                          SLURM_PROTOCOL_NO_SEND_RECV_FLAGS,
	                          timeout);
}

/**********************************************************************\
 * address conversion and management functions
\**********************************************************************/

/* slurm_set_addr
 * initializes the slurm_address with the supplied port and host name
 * OUT slurm_address	- slurm_addr_t to be filled in
 * IN port		- port in host order
 * IN host		- hostname or dns name
 */
void slurm_set_addr(slurm_addr_t * slurm_address, uint16_t port, char *host)
{
	slurm_set_addr_char(slurm_address, port, host);
}

/* slurm_get_ip_str
 * given a slurm_address it returns its port and ip address string
 * IN slurm_address	- slurm_addr_t to be queried
 * OUT port		- port number
 * OUT ip		- ip address in dotted-quad string form
 * IN buf_len		- length of ip buffer
 */
void slurm_get_ip_str(slurm_addr_t * slurm_address, uint16_t * port,
		      char *ip, unsigned int buf_len)
{
	unsigned char *uc = (unsigned char *)&slurm_address->sin_addr.s_addr;
	*port = slurm_address->sin_port;
	snprintf(ip, buf_len, "%u.%u.%u.%u", uc[0], uc[1], uc[2], uc[3]);
}

/* slurm_get_peer_addr
 * get the slurm address of the peer connection, similar to getpeeraddr
 * IN fd		- an open connection
 * OUT slurm_address	- place to park the peer's slurm_addr
 */
int slurm_get_peer_addr(int fd, slurm_addr_t * slurm_address)
{
	struct sockaddr name;
	socklen_t namelen = (socklen_t) sizeof(struct sockaddr);
	int rc;

	if ((rc = getpeername((int) fd, &name, &namelen)))
		return rc;
	memcpy(slurm_address, &name, sizeof(slurm_addr_t));
	return 0;
}

/**********************************************************************\
 * slurm_addr_t pack routines
\**********************************************************************/

/* slurm_pack_slurm_addr_array
 * packs an array of slurm_addrs into a buffer
 * OUT slurm_address	- slurm_addr_t to pack
 * IN size_val  	- how many to pack
 * IN/OUT buffer	- buffer to pack the slurm_addr_t from
 * returns		- Slurm error code
 */
void slurm_pack_slurm_addr_array(slurm_addr_t * slurm_address,
				 uint32_t size_val, Buf buffer)
{
	int i = 0;
	uint32_t nl = htonl(size_val);
	pack32(nl, buffer);

	for (i = 0; i < size_val; i++) {
		slurm_pack_slurm_addr(slurm_address + i, buffer);
	}

}

/* slurm_unpack_slurm_addr_array
 * unpacks an array of slurm_addrs from a buffer
 * OUT slurm_address	- slurm_addr_t to unpack to
 * IN size_val  	- how many to unpack
 * IN/OUT buffer	- buffer to upack the slurm_addr_t from
 * returns		- Slurm error code
 */
int slurm_unpack_slurm_addr_array(slurm_addr_t ** slurm_address,
				  uint32_t * size_val, Buf buffer)
{
	int i = 0;
	uint32_t nl;

	*slurm_address = NULL;
	safe_unpack32(&nl, buffer);
	if (nl > NO_VAL)
		goto unpack_error;
	*size_val = ntohl(nl);
	*slurm_address = xcalloc(*size_val, sizeof(slurm_addr_t));

	for (i = 0; i < *size_val; i++) {
		if (slurm_unpack_slurm_addr_no_alloc((*slurm_address) + i,
						     buffer))
			goto unpack_error;

	}
	return SLURM_SUCCESS;

unpack_error:
	xfree(*slurm_address);
	*slurm_address = NULL;
	return SLURM_ERROR;
}

static void _resp_msg_setup(slurm_msg_t *msg, slurm_msg_t *resp_msg,
			    uint16_t msg_type, void *data)
{
	slurm_msg_t_init(resp_msg);
	resp_msg->address = msg->address;
	resp_msg->auth_index = msg->auth_index;
	resp_msg->conn = msg->conn;
	resp_msg->data = data;
	resp_msg->flags = msg->flags;
	resp_msg->forward = msg->forward;
	resp_msg->forward_struct = msg->forward_struct;
	resp_msg->msg_type = msg_type;
	resp_msg->protocol_version = msg->protocol_version;
	resp_msg->ret_list = msg->ret_list;
	resp_msg->orig_addr = msg->orig_addr;
}

static void _rc_msg_setup(slurm_msg_t *msg, slurm_msg_t *resp_msg,
			  return_code_msg_t *rc_msg, int rc)
{
	memset(rc_msg, 0, sizeof(return_code_msg_t));
	rc_msg->return_code = rc;

	_resp_msg_setup(msg, resp_msg, RESPONSE_SLURM_RC, rc_msg);
}


/**********************************************************************\
 * simplified communication routines
 * They open a connection do work then close the connection all within
 * the function
\**********************************************************************/

/* slurm_send_msg
 * given the original request message this function sends a
 *	arbitrary message back to the client that made the request
 * IN request_msg	- slurm_msg the request msg
 * IN msg_type          - message type being returned
 * IN resp_msg		- the message being returned to the client
 */
int slurm_send_msg(slurm_msg_t *msg, uint16_t msg_type, void *resp)
{
	if (msg->msg_index && msg->ret_list) {
		slurm_msg_t *resp_msg = xmalloc_nz(sizeof(slurm_msg_t));

		_resp_msg_setup(msg, resp_msg, msg_type, resp);

		resp_msg->msg_index = msg->msg_index;
		resp_msg->ret_list = NULL;
		/*
		 * The return list here is the list we are sending to
		 * the node, so after we attach this message to it set
		 * it to NULL to remove it.
		 */
		list_append(msg->ret_list, resp_msg);
		return SLURM_SUCCESS;
	} else {
		slurm_msg_t resp_msg;

		if (msg->conn_fd < 0) {
			slurm_seterrno(ENOTCONN);
			return SLURM_ERROR;
		}
		_resp_msg_setup(msg, &resp_msg, msg_type, resp);

		/* send message */
		return slurm_send_node_msg(msg->conn_fd, &resp_msg);
	}
}

/* slurm_send_rc_msg
 * given the original request message this function sends a
 *	slurm_return_code message back to the client that made the request
 * IN request_msg	- slurm_msg the request msg
 * IN rc		- the return_code to send back to the client
 */
int slurm_send_rc_msg(slurm_msg_t *msg, int rc)
{
	if (msg->msg_index && msg->ret_list) {
		slurm_msg_t *resp_msg = xmalloc_nz(sizeof(slurm_msg_t));
		return_code_msg_t *rc_msg =
			xmalloc_nz(sizeof(return_code_msg_t));

		_rc_msg_setup(msg, resp_msg, rc_msg, rc);

		resp_msg->msg_index = msg->msg_index;
		resp_msg->ret_list = NULL;
		/* The return list here is the list we are sending to
		   the node, so after we attach this message to it set
		   it to NULL to remove it.
		*/
		list_append(msg->ret_list, resp_msg);
		return SLURM_SUCCESS;
	} else {
		slurm_msg_t resp_msg;
		return_code_msg_t rc_msg;

		if (msg->conn_fd < 0) {
			slurm_seterrno(ENOTCONN);
			return SLURM_ERROR;
		}
		_rc_msg_setup(msg, &resp_msg, &rc_msg, rc);

		/* send message */
		return slurm_send_node_msg(msg->conn_fd, &resp_msg);
	}
}

/* slurm_send_rc_err_msg
 * given the original request message this function sends a
 *	slurm_return_code message back to the client that made the request
 * IN request_msg	- slurm_msg the request msg
 * IN rc		- the return_code to send back to the client
 * IN err_msg   	- message for user
 */
int slurm_send_rc_err_msg(slurm_msg_t *msg, int rc, char *err_msg)
{
	slurm_msg_t resp_msg;
	return_code2_msg_t rc_msg;

	if (msg->conn_fd < 0) {
		slurm_seterrno(ENOTCONN);
		return SLURM_ERROR;
	}
	rc_msg.return_code = rc;
	rc_msg.err_msg     = err_msg;

	_resp_msg_setup(msg, &resp_msg, RESPONSE_SLURM_RC_MSG, &rc_msg);

	/* send message */
	return slurm_send_node_msg(msg->conn_fd, &resp_msg);
}

/*
 * Sends back reroute_msg_t which directs the client to make the request to
 * another cluster.
 *
 * IN msg	  - msg to respond to.
 * IN cluster_rec - cluster to direct msg to.
 */
int slurm_send_reroute_msg(slurm_msg_t *msg, slurmdb_cluster_rec_t *cluster_rec)
{
	slurm_msg_t resp_msg;
	reroute_msg_t reroute_msg = {0};

	if (msg->conn_fd < 0) {
		slurm_seterrno(ENOTCONN);
		return SLURM_ERROR;
	}

	/* Don't free the cluster_rec, it's pointing to the actual object. */
	reroute_msg.working_cluster_rec = cluster_rec;

	_resp_msg_setup(msg, &resp_msg, RESPONSE_SLURM_REROUTE_MSG,
			&reroute_msg);

	/* send message */
	return slurm_send_node_msg(msg->conn_fd, &resp_msg);
}

/*
 * Send and recv a slurm request and response on the open slurm descriptor
 * Doesn't close the connection.
 * IN fd	- file descriptor to receive msg on
 * IN req	- a slurm_msg struct to be sent by the function
 * OUT resp	- a slurm_msg struct to be filled in by the function
 * IN timeout	- how long to wait in milliseconds
 * RET int	- returns 0 on success, -1 on failure and sets errno
 */
extern int slurm_send_recv_msg(int fd, slurm_msg_t *req,
			       slurm_msg_t *resp, int timeout)
{
	int rc = -1;
	slurm_msg_t_init(resp);

	/* If we are using a persistent connection make sure it is the one we
	 * actually want.  This should be the correct one already, but just make
	 * sure.
	 */
	if (req->conn) {
		fd = req->conn->fd;
		resp->conn = req->conn;
	}

	if (slurm_send_node_msg(fd, req) >= 0) {
		/* no need to adjust and timeouts here since we are not
		   forwarding or expecting anything other than 1 message
		   and the regular timeout will be altered in
		   slurm_receive_msg if it is 0 */
		rc = slurm_receive_msg(fd, resp, timeout);
	}

	return rc;
}

/*
 * Send and recv a slurm request and response on the open slurm descriptor
 * Closes the connection.
 * IN fd	- file descriptor to receive msg on
 * IN req	- a slurm_msg struct to be sent by the function
 * OUT resp	- a slurm_msg struct to be filled in by the function
 * IN timeout	- how long to wait in milliseconds
 * RET int	- returns 0 on success, -1 on failure and sets errno
 */
static int
_send_and_recv_msg(int fd, slurm_msg_t *req,
		   slurm_msg_t *resp, int timeout)
{
	int rc = slurm_send_recv_msg(fd, req, resp, timeout);
	(void) close(fd);
	return rc;
}

/*
 * Send and recv a slurm request and response on the open slurm descriptor
 * with a list containing the responses of the children (if any) we
 * forwarded the message to. List containing type (ret_data_info_t).
 * IN fd	- file descriptor to receive msg on
 * IN req	- a slurm_msg struct to be sent by the function
 * IN timeout	- how long to wait in milliseconds
 * RET List	- List containing the responses of the children (if any) we
 *		  forwarded the message to. List containing type
 *		  (ret_data_info_t).
 */
static List
_send_and_recv_msgs(int fd, slurm_msg_t *req, int timeout)
{
	List ret_list = NULL;
	int steps = 0;

	if (!req->forward.timeout) {
		if (!timeout)
			timeout = slurm_conf.msg_timeout * 1000;
		req->forward.timeout = timeout;
	}
	if (slurm_send_node_msg(fd, req) >= 0) {
		if (req->forward.cnt > 0) {
			/* figure out where we are in the tree and set
			 * the timeout for to wait for our children
			 * correctly
			 * (timeout+message_timeout sec per step)
			 * to let the child timeout */
			if (message_timeout < 0)
				message_timeout =
					slurm_conf.msg_timeout * 1000;
			steps = req->forward.cnt + 1;
			if (!req->forward.tree_width)
				req->forward.tree_width =
					slurm_conf.tree_width;
			if (req->forward.tree_width)
				steps /= req->forward.tree_width;
			timeout = (message_timeout * steps);
			steps++;

			timeout += (req->forward.timeout*steps);
		}
		ret_list = slurm_receive_msgs(fd, steps, timeout);
	}

	(void) close(fd);

	return ret_list;
}

/*
 * slurm_send_recv_controller_msg
 * opens a connection to the controller, sends the controller a message,
 * listens for the response, then closes the connection
 * IN request_msg	- slurm_msg request
 * OUT response_msg	- slurm_msg response
 * IN comm_cluster_rec	- Communication record (host/port/version)/
 * RET int 		- returns 0 on success, -1 on failure and sets errno
 */
extern int slurm_send_recv_controller_msg(slurm_msg_t * request_msg,
				slurm_msg_t * response_msg,
				slurmdb_cluster_rec_t *comm_cluster_rec)
{
	int fd = -1;
	int rc = 0;
	time_t start_time = time(NULL);
	int retry = 1;
	slurm_conf_t *conf;
	bool have_backup;
	uint16_t slurmctld_timeout;
	slurm_addr_t ctrl_addr;
	static bool use_backup = false;
	slurmdb_cluster_rec_t *save_comm_cluster_rec = comm_cluster_rec;

	/*
	 * Just in case the caller didn't initialize his slurm_msg_t, and
	 * since we KNOW that we are only sending to one node (the controller),
	 * we initialize some forwarding variables to disable forwarding.
	 */
	forward_init(&request_msg->forward);
	request_msg->ret_list = NULL;
	request_msg->forward_struct = NULL;

tryagain:
	retry = 1;
	if (comm_cluster_rec)
		request_msg->flags |= SLURM_GLOBAL_AUTH_KEY;

	if ((fd = slurm_open_controller_conn(&ctrl_addr, &use_backup,
					     comm_cluster_rec)) < 0) {
		rc = -1;
		goto cleanup;
	}

	conf = slurm_conf_lock();
	have_backup = conf->control_cnt > 1;
	slurmctld_timeout = conf->slurmctld_timeout;
	slurm_conf_unlock();

	while (retry) {
		/*
		 * If the backup controller is in the process of assuming
		 * control, we sleep and retry later
		 */
		retry = 0;
		rc = _send_and_recv_msg(fd, request_msg, response_msg, 0);
		if (response_msg->auth_cred)
			g_slurm_auth_destroy(response_msg->auth_cred);
		else
			rc = -1;

		if ((rc == 0) && (!comm_cluster_rec)
		    && (response_msg->msg_type == RESPONSE_SLURM_RC)
		    && ((((return_code_msg_t *)response_msg->data)->return_code)
			== ESLURM_IN_STANDBY_MODE)
		    && (have_backup)
		    && (difftime(time(NULL), start_time)
			< (slurmctld_timeout + (slurmctld_timeout / 2)))) {
			log_flag(NET, "%s: Primary not responding, backup not in control. Sleeping and retry.",
				 __func__);
			slurm_free_return_code_msg(response_msg->data);
			sleep(slurmctld_timeout / 2);
			use_backup = false;
			if ((fd = slurm_open_controller_conn(&ctrl_addr,
							     &use_backup,
							     comm_cluster_rec))
			    < 0) {
				rc = -1;
			} else {
				retry = 1;
			}
		}

		if (rc == -1)
			break;
	}

	if (!rc && (response_msg->msg_type == RESPONSE_SLURM_REROUTE_MSG)) {
		reroute_msg_t *rr_msg = (reroute_msg_t *)response_msg->data;

		/*
		 * Don't expect mutliple hops but in the case it does
		 * happen, free the previous rr cluster_rec.
		 */
		if (comm_cluster_rec &&
		    (comm_cluster_rec != save_comm_cluster_rec))
			slurmdb_destroy_cluster_rec(comm_cluster_rec);

		comm_cluster_rec = rr_msg->working_cluster_rec;
		slurmdb_setup_cluster_rec(comm_cluster_rec);
		rr_msg->working_cluster_rec = NULL;
		goto tryagain;
	}

	if (comm_cluster_rec != save_comm_cluster_rec)
		slurmdb_destroy_cluster_rec(comm_cluster_rec);

cleanup:
	if (rc != 0)
 		_remap_slurmctld_errno();

	return rc;
}

/* slurm_send_recv_node_msg
 * opens a connection to node, sends the node a message, listens
 * for the response, then closes the connection
 * IN request_msg	- slurm_msg request
 * OUT response_msg	- slurm_msg response
 * IN timeout		- how long to wait in milliseconds
 * RET int		- returns 0 on success, -1 on failure and sets errno
 */
int slurm_send_recv_node_msg(slurm_msg_t *req, slurm_msg_t *resp, int timeout)
{
	int fd = -1;

	resp->auth_cred = NULL;
	if ((fd = slurm_open_msg_conn(&req->address)) < 0)
		return -1;

	return _send_and_recv_msg(fd, req, resp, timeout);

}

/* slurm_send_only_controller_msg
 * opens a connection to the controller, sends the controller a
 * message then, closes the connection
 * IN request_msg	- slurm_msg request
 * IN comm_cluster_rec	- Communication record (host/port/version)
 * RET int		- return code
 * NOTE: NOT INTENDED TO BE CROSS-CLUSTER
 */
extern int slurm_send_only_controller_msg(slurm_msg_t *req,
				slurmdb_cluster_rec_t *comm_cluster_rec)
{
	int      rc = SLURM_SUCCESS;
	int fd = -1;
	slurm_addr_t ctrl_addr;
	bool     use_backup = false;

	/*
	 *  Open connection to Slurm controller:
	 */
	if ((fd = slurm_open_controller_conn(&ctrl_addr, &use_backup,
					     comm_cluster_rec)) < 0) {
		rc = SLURM_ERROR;
		goto cleanup;
	}

	if ((rc = slurm_send_node_msg(fd, req)) < 0) {
		rc = SLURM_ERROR;
	} else {
		log_flag(NET, "%s: sent %d", __func__, rc);
		rc = SLURM_SUCCESS;
	}

	(void) close(fd);

cleanup:
	if (rc != SLURM_SUCCESS)
		_remap_slurmctld_errno();
	return rc;
}

/*
 *  Open a connection to the "address" specified in the slurm msg `req'
 *   Then, immediately close the connection w/out waiting for a reply.
 *
 *   Returns SLURM_SUCCESS on success SLURM_ERROR (< 0) for failure.
 *
 * DO NOT USE THIS IN NEW CODE
 * Use slurm_send_recv_rc_msg_only_one() or something similar instead.
 *
 * By not waiting for a response message, the message to be transmitted
 * may never be received by the remote end. The remote TCP stack may
 * acknowledge the data while the application itself has not had a chance
 * to receive it. The only way to tell that the application has processed
 * a given packet is for it to send back a message across the socket itself.
 *
 * The receive side looks like: poll() && read(), close(). If the poll() times
 * out, the kernel may still ACK the data while the application has jumped to
 * closing the connection. The send side cannot then distinguish between the
 * close happening as a result of the timeout vs. as a normal message shutdown.
 *
 * This is only one example of the many races inherent in this approach.
 *
 * See "UNIX Network Programming" Volume 1 (Third Edition) Section 7.5 on
 * SO_LINGER for a description of the subtle hazards inherent in abusing
 * TCP as a unidirectional pipe.
 */
int slurm_send_only_node_msg(slurm_msg_t *req)
{
	int rc = SLURM_SUCCESS;
	int fd = -1;
	struct pollfd pfd;
	int value = -1;
	int pollrc;

	if ((fd = slurm_open_msg_conn(&req->address)) < 0) {
		return SLURM_ERROR;
	}

	if ((rc = slurm_send_node_msg(fd, req)) < 0) {
		rc = SLURM_ERROR;
	} else {
		log_flag(NET, "%s: sent %d", __func__, rc);
		rc = SLURM_SUCCESS;
	}

	/*
	 * Make sure message was received by remote, and that there isn't
	 * and outstanding write() or that the connection has been reset.
	 *
	 * The shutdown() call intentionally falls through to the next block,
	 * the poll() should hit POLLERR which gives the TICOUTQ count as an
	 * additional diagnostic element.
	 *
	 * The steps below may result in a false-positive on occassion, in
	 * which case the code path above may opt to retransmit an already
	 * received message. If this is a concern, you should not be using
	 * this function.
	 */
	if (shutdown(fd, SHUT_WR))
		log_flag(NET, "%s: shutdown call failed: %m", __func__);

again:
	pfd.fd = fd;
	pfd.events = POLLIN;
	pollrc = poll(&pfd, 1, (slurm_conf.msg_timeout * 1000));
	if (pollrc == -1) {
		if (errno == EINTR)
			goto again;
		log_flag(NET, "%s: poll error: %m", __func__);
		(void) close(fd);
		return SLURM_ERROR;
	}

	if (pollrc == 0) {
		if (ioctl(fd, TIOCOUTQ, &value))
			log_flag(NET, "%s: TIOCOUTQ ioctl failed",
				 __func__);
		log_flag(NET, "%s: poll timed out with %d outstanding: %m",
			 __func__, value);
		(void) close(fd);
		return SLURM_ERROR;
	}

	if (pfd.revents & POLLERR) {
		int value = -1;

		if (ioctl(fd, TIOCOUTQ, &value))
			log_flag(NET, "%s: TIOCOUTQ ioctl failed",
				 __func__);
		fd_get_socket_error(fd, &errno);
		log_flag(NET, "%s: poll error with %d outstanding: %m",
			 __func__, value);

		(void) close(fd);
		return SLURM_ERROR;
	}

	(void) close(fd);

	return rc;
}

/*
 * Open a connection to the "address" specified in the slurm msg `req'
 * Then, immediately close the connection w/out waiting for a reply.
 * Ignore any errors. This should only be used when you do not care if
 * the message is ever received.
 */
void slurm_send_msg_maybe(slurm_msg_t *req)
{
	int fd = -1;

	if ((fd = slurm_open_msg_conn(&req->address)) < 0) {
		return;
	}

	(void) slurm_send_node_msg(fd, req);

	(void) close(fd);
}

/*
 *  Send a message to the nodelist specificed using fanout
 *    Then return List containing type (ret_data_info_t).
 * IN nodelist	  - list of nodes to send to.
 * IN msg	  - a slurm_msg struct to be sent by the function
 * IN timeout	  - how long to wait in milliseconds
 * RET List	  - List containing the responses of the children
 *		    (if any) we forwarded the message to. List
 *		    containing type (ret_data_info_t).
 */
List slurm_send_recv_msgs(const char *nodelist, slurm_msg_t *msg, int timeout)
{
	List ret_list = NULL;
	hostlist_t hl = NULL;

	if (!nodelist || !strlen(nodelist)) {
		error("slurm_send_recv_msgs: no nodelist given");
		return NULL;
	}

	hl = hostlist_create(nodelist);
	if (!hl) {
		error("slurm_send_recv_msgs: problem creating hostlist");
		return NULL;
	}

	ret_list = start_msg_tree(hl, msg, timeout);
	hostlist_destroy(hl);

	return ret_list;
}

/*
 *  Send a message to msg->address
 *    Then return List containing type (ret_data_info_t).
 * IN msg	  - a slurm_msg struct to be sent by the function
 * IN timeout	  - how long to wait in milliseconds
 * RET List	  - List containing the responses of the children
 *		    (if any) we forwarded the message to. List
 *		    containing type (ret_types_t).
 */
List slurm_send_addr_recv_msgs(slurm_msg_t *msg, char *name, int timeout)
{
	static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;
	static uint16_t conn_timeout = NO_VAL16;
	List ret_list = NULL;
	int fd = -1;
	ret_data_info_t *ret_data_info = NULL;
	ListIterator itr;
	int i;

	slurm_mutex_lock(&conn_lock);
	if (conn_timeout == NO_VAL16)
		conn_timeout = MIN(slurm_conf.msg_timeout, 10);
	slurm_mutex_unlock(&conn_lock);

	/* This connect retry logic permits Slurm hierarchical communications
	 * to better survive slurmd restarts */
	for (i = 0; i <= conn_timeout; i++) {
		if (i)
			sleep(1);
		fd = slurm_open_msg_conn(&msg->address);
		if ((fd >= 0) || (errno != ECONNREFUSED))
			break;
		if (i == 0)
			log_flag(NET, "%s: connect refused, retrying",
				 __func__);
	}
	if (fd < 0) {
		mark_as_failed_forward(&ret_list, name,
				       SLURM_COMMUNICATIONS_CONNECTION_ERROR);
		errno = SLURM_COMMUNICATIONS_CONNECTION_ERROR;
		return ret_list;
	}

	msg->ret_list = NULL;
	msg->forward_struct = NULL;
	if (!(ret_list = _send_and_recv_msgs(fd, msg, timeout))) {
		mark_as_failed_forward(&ret_list, name, errno);
		errno = SLURM_COMMUNICATIONS_CONNECTION_ERROR;
		return ret_list;
	} else {
		itr = list_iterator_create(ret_list);
		while ((ret_data_info = list_next(itr)))
			if (!ret_data_info->node_name) {
				ret_data_info->node_name = xstrdup(name);
			}
		list_iterator_destroy(itr);
	}
	return ret_list;
}

/*
 *  Open a connection to the "address" specified in the slurm msg "req".
 *    Then read back an "rc" message returning the "return_code" specified
 *    in the response in the "rc" parameter.
 * IN req	- a slurm_msg struct to be sent by the function
 * OUT rc	- return code from the sent message
 * IN timeout	- how long to wait in milliseconds
 * RET int either 0 for success or -1 for failure.
 */
int slurm_send_recv_rc_msg_only_one(slurm_msg_t *req, int *rc, int timeout)
{
	int fd = -1;
	int ret_c = 0;
	slurm_msg_t resp;

	slurm_msg_t_init(&resp);

	/* Just in case the caller didn't initialize his slurm_msg_t, and
	 * since we KNOW that we are only sending to one node,
	 * we initialize some forwarding variables to disable forwarding.
	 */
	forward_init(&req->forward);
	req->ret_list = NULL;
	req->forward_struct = NULL;

	if ((fd = slurm_open_msg_conn(&req->address)) < 0)
		return -1;
	if (!_send_and_recv_msg(fd, req, &resp, timeout)) {
		if (resp.auth_cred)
			g_slurm_auth_destroy(resp.auth_cred);
		*rc = slurm_get_return_code(resp.msg_type, resp.data);
		slurm_free_msg_data(resp.msg_type, resp.data);
		ret_c = 0;
	} else
		ret_c = -1;
	return ret_c;
}

/*
 * Send message to controller and get return code.
 * Make use of slurm_send_recv_controller_msg(), which handles
 * support for backup controller and retry during transistion.
 * IN req - request to send
 * OUT rc - return code
 * IN comm_cluster_rec	- Communication record (host/port/version)/
 * RET - 0 on success, -1 on failure
 */
extern int slurm_send_recv_controller_rc_msg(slurm_msg_t *req, int *rc,
					slurmdb_cluster_rec_t *comm_cluster_rec)
{
	int ret_c;
	slurm_msg_t resp;

	if (!slurm_send_recv_controller_msg(req, &resp, comm_cluster_rec)) {
		*rc = slurm_get_return_code(resp.msg_type, resp.data);
		slurm_free_msg_data(resp.msg_type, resp.data);
		ret_c = 0;
	} else {
		ret_c = -1;
	}

	return ret_c;
}

/* this is used to set how many nodes are going to be on each branch
 * of the tree.
 * IN total       - total number of nodes to send to
 * IN tree_width  - how wide the tree should be on each hop
 * RET int *	  - int array tree_width in length each space
 *		    containing the number of nodes to send to each hop
 *		    on the span.
 */
extern int *set_span(int total,  uint16_t tree_width)
{
	int *span = NULL;
	int left = total;
	int i = 0;

	if (tree_width == 0)
		tree_width = slurm_conf.tree_width;

	span = xcalloc(tree_width, sizeof(int));
	//info("span count = %d", tree_width);
	if (total <= tree_width) {
		return span;
	}

	while (left > 0) {
		for (i = 0; i < tree_width; i++) {
			if ((tree_width-i) >= left) {
				if (span[i] == 0) {
					left = 0;
					break;
				} else {
					span[i] += left;
					left = 0;
					break;
				}
			} else if (left <= tree_width) {
				if (span[i] == 0)
					left--;

				span[i] += left;
				left = 0;
				break;
			}

			if (span[i] == 0)
				left--;

			span[i] += tree_width;
			left -= tree_width;
		}
	}

	return span;
}

/*
 * Free a slurm message's memebers but not the message itself
 */
extern void slurm_free_msg_members(slurm_msg_t *msg)
{
	if (msg) {
		if (msg->auth_cred)
			(void) g_slurm_auth_destroy(msg->auth_cred);
		free_buf(msg->buffer);
		slurm_free_msg_data(msg->msg_type, msg->data);
		FREE_NULL_LIST(msg->ret_list);
	}
}

/*
 * Free a slurm message
 */
extern void slurm_free_msg(slurm_msg_t *msg)
{
	if (msg) {
		slurm_free_msg_members(msg);
		xfree(msg);
	}
}

extern char *nodelist_nth_host(const char *nodelist, int inx)
{
	hostlist_t hl = hostlist_create(nodelist);
	char *name = hostlist_nth(hl, inx);
	hostlist_destroy(hl);
	return name;
}

extern int nodelist_find(const char *nodelist, const char *name)
{
	hostlist_t hl = hostlist_create(nodelist);
	int id = hostlist_find(hl, name);
	hostlist_destroy(hl);
	return id;
}

/*
 * Convert number from one unit to another.
 * By default, Will convert num to largest divisible unit.
 * Appends unit type suffix -- if applicable.
 *
 * IN num: number to convert.
 * OUT buf: buffer to copy converted number into.
 * IN buf_size: size of buffer.
 * IN orig_type: The original type of num.
 * IN spec_type: Type to convert num to. If specified, num will be converted up
 * or down to this unit type.
 * IN divisor: size of type
 * IN flags: flags to control whether to convert exactly or not at all.
 */
extern void convert_num_unit2(double num, char *buf, int buf_size,
			      int orig_type, int spec_type, int divisor,
			      uint32_t flags)
{
	char *unit = "\0KMGTP?";
	uint64_t i;

	if ((int64_t)num == 0) {
		snprintf(buf, buf_size, "0");
		return;
	}

	if (spec_type != NO_VAL) {
		/* spec_type overrides all flags */
		if (spec_type < orig_type) {
			while (spec_type < orig_type) {
				num *= divisor;
				orig_type--;
			}
		} else if (spec_type > orig_type) {
			while (spec_type > orig_type) {
				num /= divisor;
				orig_type++;
			}
		}
	} else if (flags & CONVERT_NUM_UNIT_RAW) {
		orig_type = UNIT_NONE;
	} else if (flags & CONVERT_NUM_UNIT_NO) {
		/* no op */
	} else if (flags & CONVERT_NUM_UNIT_EXACT) {
		/* convert until we would loose precision */
		/* half values  (e.g., 2.5G) are still considered precise */

		while (num >= divisor
		       && ((uint64_t)num % (divisor / 2) == 0)) {
			num /= divisor;
			orig_type++;
		}
	} else {
		/* aggressively convert values */
		while (num >= divisor) {
			num /= divisor;
			orig_type++;
		}
	}

	if (orig_type < UNIT_NONE || orig_type > UNIT_PETA)
		orig_type = UNIT_UNKNOWN;
	i = (uint64_t)num;
	/* Here we are checking to see if these numbers are the same,
	 * meaning the float has not floating point.  If we do have
	 * floating point print as a float.
	*/
	if ((double)i == num)
		snprintf(buf, buf_size, "%"PRIu64"%c", i, unit[orig_type]);
	else
		snprintf(buf, buf_size, "%.2f%c", num, unit[orig_type]);
}

extern void convert_num_unit(double num, char *buf, int buf_size,
			     int orig_type, int spec_type, uint32_t flags)
{
	convert_num_unit2(num, buf, buf_size, orig_type, spec_type, 1024,
			  flags);
}

extern int revert_num_unit(const char *buf)
{
	char *unit = "\0KMGTP\0";
	int i = 1, j = 0, number = 0;

	if (!buf)
		return -1;
	j = strlen(buf) - 1;
	while (unit[i]) {
		if (toupper((int)buf[j]) == unit[i])
			break;
		i++;
	}

	number = atoi(buf);
	if (unit[i])
		number *= (i*1024);

	return number;
}

extern int get_convert_unit_val(int base_unit, char convert_to)
{
	int conv_unit = 0, conv_value = 0;

	if ((conv_unit = get_unit_type(convert_to)) == SLURM_ERROR)
		return SLURM_ERROR;

	while (base_unit++ < conv_unit) {
		if (!conv_value)
			conv_value = 1024;
		else
			conv_value *= 1024;
	}

	return conv_value;
}

extern int get_unit_type(char unit)
{
	char *units = "\0KMGTP";
	char *tmp_char = NULL;

	if (unit == '\0') {
		error("Invalid unit type '%c'. Possible options are '%s'",
		      unit, units + 1);
		return SLURM_ERROR;
	}

	tmp_char = strchr(units + 1, toupper(unit));
	if (!tmp_char) {
		error("Invalid unit type '%c'. Possible options are '%s'",
		      unit, units + 1);
		return SLURM_ERROR;
	}
	return tmp_char - units;
}

static void _print_data(const char *tag, const char *data, int len)
{
	char *hex = NULL;
	char *str = NULL;
	int start = 0;

	if (len <= 0)
		return;

	/* print up to len or 16 lines worth */
	for (int i = 0; (i < len) && (i < (16 * 16)); i++) {
		if (i && !(i % 16)) {
			log_flag(NET_RAW, "%s: [%04u/%04u] 0x%s \"%s\"",
				 tag, start, len, hex, str);
			xfree(hex);
			xfree(str);
			start = i;
		}
		/* convert each char into equiv hex */
		xstrfmtcat(hex, "%02x ", (data[i] & 0xff));
		/* create safe string to print in quotes */
		if (isalnum(data[i]) || ispunct(data[i]) || (data[i] == ' '))
			xstrfmtcat(str, "%c", data[i]);
		else
			xstrfmtcat(str, "%c", '.');
	}

	log_flag(NET_RAW, "%s: [%04u/%04u] 0x%s \"%s\"",
		 tag, start, len, hex, str);
	xfree(hex);
	xfree(str);
}

/*
 * slurm_forward_data - forward arbitrary data to unix domain sockets on nodes
 * IN/OUT nodelist: Nodes to forward data to (if failure this list is changed to
 *                  reflect the failed nodes).
 * IN address: address of unix domain socket
 * IN len: length of data
 * IN data: real data
 * RET: error code
 */
extern int slurm_forward_data(
	char **nodelist, char *address, uint32_t len, const char *data)
{
	List ret_list = NULL;
	int temp_rc = 0, rc = 0;
	ret_data_info_t *ret_data_info = NULL;
	slurm_msg_t msg;
	forward_data_msg_t req;
	hostlist_t hl = NULL;
	bool redo_nodelist = false;
	slurm_msg_t_init(&msg);

	log_flag(NET, "%s: nodelist=%s, address=%s, len=%u",
		 __func__, *nodelist, address, len);
	req.address = address;
	req.len = len;
	req.data = (char *)data;

	msg.msg_type = REQUEST_FORWARD_DATA;
	msg.data = &req;

	if ((ret_list = slurm_send_recv_msgs(*nodelist, &msg, 0))) {
		if (list_count(ret_list) > 1)
			redo_nodelist = true;

		while ((ret_data_info = list_pop(ret_list))) {
			temp_rc = slurm_get_return_code(ret_data_info->type,
							ret_data_info->data);
			if (temp_rc != SLURM_SUCCESS) {
				rc = temp_rc;
				if (redo_nodelist) {
					if (!hl)
						hl = hostlist_create(
							ret_data_info->
							node_name);
					else
						hostlist_push_host(
							hl, ret_data_info->
							node_name);
				}
			}
			destroy_data_info(ret_data_info);
		}
	} else {
		error("slurm_forward_data: no list was returned");
		rc = SLURM_ERROR;
	}

	if (hl) {
		xfree(*nodelist);
		hostlist_sort(hl);
		*nodelist = hostlist_ranged_string_xmalloc(hl);
		hostlist_destroy(hl);
	}

	FREE_NULL_LIST(ret_list);

	return rc;
}

extern void slurm_setup_sockaddr(struct sockaddr_in *sin, uint16_t port)
{
	static uint32_t s_addr = NO_VAL;

	memset(sin, 0, sizeof(struct sockaddr_in));
	sin->sin_family = AF_INET;
	sin->sin_port = htons(port);

	if (s_addr == NO_VAL) {
		/* On systems with multiple interfaces we might not
		 * want to get just any address.  This is the case on
		 * a Cray system with RSIP.
		 */
		char *comm_params = slurm_get_comm_parameters();
		char *var;

		if (running_in_slurmctld())
			var = "NoCtldInAddrAny";
		else
			var = "NoInAddrAny";

		if (xstrcasestr(comm_params, var)) {
			char host[MAXHOSTNAMELEN];

			if (!gethostname(host, MAXHOSTNAMELEN)) {
				slurm_set_addr_char(sin, port, host);
				s_addr = sin->sin_addr.s_addr;
			} else
				fatal("slurm_setup_sockaddr: "
				      "Can't get hostname or addr: %m");
		} else
			s_addr = htonl(INADDR_ANY);

		xfree(comm_params);
	}

	sin->sin_addr.s_addr = s_addr;
}

/*
 * Check if we can bind() the socket s to port port.
 *
 * IN: s - socket
 * IN: port - port number to attempt to bind
 * IN: local - only bind to localhost if true
 * OUT: true/false if port was bound successfully
 */
int sock_bind_range(int s, uint16_t *range, bool local)
{
	uint32_t count;
	uint32_t min;
	uint32_t max;
	uint32_t port;
	uint32_t num;

	min = range[0];
	max = range[1];

	srand(getpid());
	num = max - min + 1;
	port = min + (random() % num);
	count = num;

	do {
		if (_is_port_ok(s, port, local))
			return port;

		if (port == max)
			port = min;
		else
			++port;
		--count;
	} while (count > 0);

	error("%s: all ports in range (%u, %u) exhausted, cannot establish listening port",
	      __func__, min, max);

	return -1;
}

/*
 * Check if we can bind() the socket s to port port.
 *
 * IN: s - socket
 * IN: port - port number to attempt to bind
 * IN: local - only bind to localhost if true
 * OUT: true/false if port was bound successfully
 */
static bool _is_port_ok(int s, uint16_t port, bool local)
{
	struct sockaddr_in sin;

	slurm_setup_sockaddr(&sin, port);

	if (local)
		sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		log_flag(NET, "%s: bind() failed on port:%d fd:%d: %m",
			 __func__, port, s);
		return false;
	}

	return true;
}

extern int slurm_hex_to_char(int v)
{
	if (v >= 0 && v < 10)
		return '0' + v;
	else if (v >= 10 && v < 16)
		return ('a' - 10) + v;
	else
		return -1;
}

extern int slurm_char_to_hex(int c)
{
	int cl;

	cl = tolower(c);
	if (c >= '0' && c <= '9')
		return c - '0';
	else if (cl >= 'a' && cl <= 'f')
		return cl + (10 - 'a');
	else
		return -1;
}
