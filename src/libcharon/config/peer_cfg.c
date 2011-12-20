/*
 * Copyright (C) 2007-2008 Tobias Brunner
 * Copyright (C) 2005-2009 Martin Willi
 * Copyright (C) 2005 Jan Hutter
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <string.h>

#include "peer_cfg.h"

#include <daemon.h>

#include <threading/mutex.h>
#include <utils/linked_list.h>
#include <utils/identification.h>

ENUM(ike_version_names, IKE_ANY, IKEV2,
	"IKEv1/2",
	"IKEv1",
	"IKEv2",
);

ENUM(cert_policy_names, CERT_ALWAYS_SEND, CERT_NEVER_SEND,
	"CERT_ALWAYS_SEND",
	"CERT_SEND_IF_ASKED",
	"CERT_NEVER_SEND",
);

ENUM(unique_policy_names, UNIQUE_NO, UNIQUE_KEEP,
	"UNIQUE_NO",
	"UNIQUE_REPLACE",
	"UNIQUE_KEEP",
);

typedef struct private_peer_cfg_t private_peer_cfg_t;

/**
 * Private data of an peer_cfg_t object
 */
struct private_peer_cfg_t {

	/**
	 * Public part
	 */
	peer_cfg_t public;

	/**
	 * Number of references hold by others to this peer_cfg
	 */
	refcount_t refcount;

	/**
	 * Name of the peer_cfg, used to query it
	 */
	char *name;

	/**
	 * IKE version to use for initiation
	 */
	ike_version_t ike_version;

	/**
	 * IKE config associated to this peer config
	 */
	ike_cfg_t *ike_cfg;

	/**
	 * list of child configs associated to this peer config
	 */
	linked_list_t *child_cfgs;

	/**
	 * mutex to lock access to list of child_cfgs
	 */
	mutex_t *mutex;

	/**
	 * should we send a certificate
	 */
	cert_policy_t cert_policy;

	/**
	 * uniqueness of an IKE_SA
	 */
	unique_policy_t unique;

	/**
	 * number of tries after giving up if peer does not respond
	 */
	u_int32_t keyingtries;

	/**
	 * enable support for MOBIKE
	 */
	bool use_mobike;

	/**
	 * Time before starting rekeying
	 */
	u_int32_t rekey_time;

	/**
	 * Time before starting reauthentication
	 */
	u_int32_t reauth_time;

	/**
	 * Time, which specifies the range of a random value subtracted from above.
	 */
	u_int32_t jitter_time;

	/**
	 * Delay before deleting a rekeying/reauthenticating SA
	 */
	u_int32_t over_time;

	/**
	 * DPD check intervall
	 */
	u_int32_t dpd;

	/**
	 * virtual IP to use locally
	 */
	host_t *virtual_ip;

	/**
	 * pool to acquire configuration attributes from
	 */
	char *pool;

	/**
	 * local authentication configs (rulesets)
	 */
	linked_list_t *local_auth;

	/**
	 * remote authentication configs (constraints)
	 */
	linked_list_t *remote_auth;

#ifdef ME
	/**
	 * Is this a mediation connection?
	 */
	bool mediation;

	/**
	 * Name of the mediation connection to mediate through
	 */
	peer_cfg_t *mediated_by;

	/**
	 * ID of our peer at the mediation server (= leftid of the peer's conn with
	 * the mediation server)
	 */
	identification_t *peer_id;
#endif /* ME */
};

METHOD(peer_cfg_t, get_name, char*,
	private_peer_cfg_t *this)
{
	return this->name;
}

METHOD(peer_cfg_t, get_ike_version, ike_version_t,
	private_peer_cfg_t *this)
{
	return this->ike_version;
}

METHOD(peer_cfg_t, get_ike_cfg, ike_cfg_t*,
	private_peer_cfg_t *this)
{
	return this->ike_cfg;
}

METHOD(peer_cfg_t, add_child_cfg, void,
	private_peer_cfg_t *this, child_cfg_t *child_cfg)
{
	this->mutex->lock(this->mutex);
	this->child_cfgs->insert_last(this->child_cfgs, child_cfg);
	this->mutex->unlock(this->mutex);
}

/**
 * child_cfg enumerator
 */
typedef struct {
	enumerator_t public;
	enumerator_t *wrapped;
	mutex_t *mutex;
} child_cfg_enumerator_t;

METHOD(peer_cfg_t, remove_child_cfg, void,
	private_peer_cfg_t *this, child_cfg_enumerator_t *enumerator)
{
	this->child_cfgs->remove_at(this->child_cfgs, enumerator->wrapped);
}

METHOD(enumerator_t, child_cfg_enumerator_destroy, void,
	child_cfg_enumerator_t *this)
{
	this->mutex->unlock(this->mutex);
	this->wrapped->destroy(this->wrapped);
	free(this);
}

METHOD(enumerator_t, child_cfg_enumerate, bool,
	child_cfg_enumerator_t *this, child_cfg_t **chd)
{
	return this->wrapped->enumerate(this->wrapped, chd);
}

METHOD(peer_cfg_t, create_child_cfg_enumerator, enumerator_t*,
	private_peer_cfg_t *this)
{
	child_cfg_enumerator_t *enumerator;

	INIT(enumerator,
		.public = {
			.enumerate = (void*)_child_cfg_enumerate,
			.destroy = (void*)_child_cfg_enumerator_destroy,
		},
		.mutex = this->mutex,
		.wrapped = this->child_cfgs->create_enumerator(this->child_cfgs),
	);

	this->mutex->lock(this->mutex);
	return &enumerator->public;
}

/**
 * Check how good a list of TS matches a given child config
 */
static int get_ts_match(child_cfg_t *cfg, bool local,
						linked_list_t *sup_list, host_t *host)
{
	linked_list_t *cfg_list;
	enumerator_t *sup_enum, *cfg_enum;
	traffic_selector_t *sup_ts, *cfg_ts;
	int match = 0, round;

	/* fetch configured TS list, narrowing dynamic TS */
	cfg_list = cfg->get_traffic_selectors(cfg, local, NULL, host);

	/* use a round counter to rate leading TS with higher priority */
	round = sup_list->get_count(sup_list);

	sup_enum = sup_list->create_enumerator(sup_list);
	while (sup_enum->enumerate(sup_enum, &sup_ts))
	{
		cfg_enum = cfg_list->create_enumerator(cfg_list);
		while (cfg_enum->enumerate(cfg_enum, &cfg_ts))
		{
			if (cfg_ts->equals(cfg_ts, sup_ts))
			{	/* equality is honored better than matches */
				match += round * 5;
			}
			else if (cfg_ts->is_contained_in(cfg_ts, sup_ts) ||
					 sup_ts->is_contained_in(sup_ts, cfg_ts))
			{
				match += round * 1;
			}
		}
		cfg_enum->destroy(cfg_enum);
		round--;
	}
	sup_enum->destroy(sup_enum);

	cfg_list->destroy_offset(cfg_list, offsetof(traffic_selector_t, destroy));

	return match;
}

METHOD(peer_cfg_t, select_child_cfg, child_cfg_t*,
	private_peer_cfg_t *this, linked_list_t *my_ts, linked_list_t *other_ts,
	host_t *my_host, host_t *other_host)
{
	child_cfg_t *current, *found = NULL;
	enumerator_t *enumerator;
	int best = 0;

	DBG2(DBG_CFG, "looking for a child config for %#R=== %#R", my_ts, other_ts);
	enumerator = create_child_cfg_enumerator(this);
	while (enumerator->enumerate(enumerator, &current))
	{
		int my_prio, other_prio;

		my_prio = get_ts_match(current, TRUE, my_ts, my_host);
		other_prio = get_ts_match(current, FALSE, other_ts, other_host);

		if (my_prio && other_prio)
		{
			DBG2(DBG_CFG, "  candidate \"%s\" with prio %d+%d",
				 current->get_name(current), my_prio, other_prio);
			if (my_prio + other_prio > best)
			{
				best = my_prio + other_prio;
				DESTROY_IF(found);
				found = current->get_ref(current);
			}
		}
	}
	enumerator->destroy(enumerator);
	if (found)
	{
		DBG2(DBG_CFG, "found matching child config \"%s\" with prio %d",
			 found->get_name(found), best);
	}
	return found;
}

METHOD(peer_cfg_t, get_cert_policy, cert_policy_t,
	private_peer_cfg_t *this)
{
	return this->cert_policy;
}

METHOD(peer_cfg_t, get_unique_policy, unique_policy_t,
	private_peer_cfg_t *this)
{
	return this->unique;
}

METHOD(peer_cfg_t, get_keyingtries, u_int32_t,
	private_peer_cfg_t *this)
{
	return this->keyingtries;
}

METHOD(peer_cfg_t, get_rekey_time, u_int32_t,
	private_peer_cfg_t *this, bool jitter)
{
	if (this->rekey_time == 0)
	{
		return 0;
	}
	if (this->jitter_time == 0 || !jitter)
	{
		return this->rekey_time;
	}
	return this->rekey_time - (random() % this->jitter_time);
}

METHOD(peer_cfg_t, get_reauth_time, u_int32_t,
	private_peer_cfg_t *this, bool jitter)
{
	if (this->reauth_time == 0)
	{
		return 0;
	}
	if (this->jitter_time == 0 || !jitter)
	{
		return this->reauth_time;
	}
	return this->reauth_time - (random() % this->jitter_time);
}

METHOD(peer_cfg_t, get_over_time, u_int32_t,
	private_peer_cfg_t *this)
{
	return this->over_time;
}

METHOD(peer_cfg_t, use_mobike, bool,
	private_peer_cfg_t *this)
{
	return this->use_mobike;
}

METHOD(peer_cfg_t, get_dpd, u_int32_t,
	private_peer_cfg_t *this)
{
	return this->dpd;
}

METHOD(peer_cfg_t, get_virtual_ip, host_t*,
	private_peer_cfg_t *this)
{
	return this->virtual_ip;
}

METHOD(peer_cfg_t, get_pool, char*,
	private_peer_cfg_t *this)
{
	return this->pool;
}

METHOD(peer_cfg_t, add_auth_cfg, void,
	private_peer_cfg_t *this, auth_cfg_t *cfg, bool local)
{
	if (local)
	{
		this->local_auth->insert_last(this->local_auth, cfg);
	}
	else
	{
		this->remote_auth->insert_last(this->remote_auth, cfg);
	}
}

METHOD(peer_cfg_t, create_auth_cfg_enumerator, enumerator_t*,
	private_peer_cfg_t *this, bool local)
{
	if (local)
	{
		return this->local_auth->create_enumerator(this->local_auth);
	}
	return this->remote_auth->create_enumerator(this->remote_auth);
}

#ifdef ME
METHOD(peer_cfg_t, is_mediation, bool,
	private_peer_cfg_t *this)
{
	return this->mediation;
}

METHOD(peer_cfg_t, get_mediated_by, peer_cfg_t*,
	private_peer_cfg_t *this)
{
	return this->mediated_by;
}

METHOD(peer_cfg_t, get_peer_id, identification_t*,
	private_peer_cfg_t *this)
{
	return this->peer_id;
}
#endif /* ME */

/**
 * check auth configs for equality
 */
static bool auth_cfg_equal(private_peer_cfg_t *this, private_peer_cfg_t *other)
{
	enumerator_t *e1, *e2;
	auth_cfg_t *cfg1, *cfg2;
	bool equal = TRUE;

	if (this->local_auth->get_count(this->local_auth) !=
		other->local_auth->get_count(other->local_auth))
	{
		return FALSE;
	}
	if (this->remote_auth->get_count(this->remote_auth) !=
		other->remote_auth->get_count(other->remote_auth))
	{
		return FALSE;
	}

	e1 = this->local_auth->create_enumerator(this->local_auth);
	e2 = other->local_auth->create_enumerator(other->local_auth);
	while (e1->enumerate(e1, &cfg1) && e2->enumerate(e2, &cfg2))
	{
		if (!cfg1->equals(cfg1, cfg2))
		{
			equal = FALSE;
			break;
		}
	}
	e1->destroy(e1);
	e2->destroy(e2);

	if (!equal)
	{
		return FALSE;
	}

	e1 = this->remote_auth->create_enumerator(this->remote_auth);
	e2 = other->remote_auth->create_enumerator(other->remote_auth);
	while (e1->enumerate(e1, &cfg1) && e2->enumerate(e2, &cfg2))
	{
		if (!cfg1->equals(cfg1, cfg2))
		{
			equal = FALSE;
			break;
		}
	}
	e1->destroy(e1);
	e2->destroy(e2);

	return equal;
}

METHOD(peer_cfg_t, equals, bool,
	private_peer_cfg_t *this, private_peer_cfg_t *other)
{
	if (this == other)
	{
		return TRUE;
	}
	if (this->public.equals != other->public.equals)
	{
		return FALSE;
	}

	return (
		this->ike_version == other->ike_version &&
		this->cert_policy == other->cert_policy &&
		this->unique == other->unique &&
		this->keyingtries == other->keyingtries &&
		this->use_mobike == other->use_mobike &&
		this->rekey_time == other->rekey_time &&
		this->reauth_time == other->reauth_time &&
		this->jitter_time == other->jitter_time &&
		this->over_time == other->over_time &&
		this->dpd == other->dpd &&
		(this->virtual_ip == other->virtual_ip ||
		 (this->virtual_ip && other->virtual_ip &&
		  this->virtual_ip->equals(this->virtual_ip, other->virtual_ip))) &&
		(this->pool == other->pool ||
		 (this->pool && other->pool && streq(this->pool, other->pool))) &&
		auth_cfg_equal(this, other)
#ifdef ME
		&& this->mediation == other->mediation &&
		this->mediated_by == other->mediated_by &&
		(this->peer_id == other->peer_id ||
		 (this->peer_id && other->peer_id &&
		  this->peer_id->equals(this->peer_id, other->peer_id)))
#endif /* ME */
		);
}

METHOD(peer_cfg_t, get_ref, peer_cfg_t*,
	private_peer_cfg_t *this)
{
	ref_get(&this->refcount);
	return &this->public;
}

METHOD(peer_cfg_t, destroy, void,
	private_peer_cfg_t *this)
{
	if (ref_put(&this->refcount))
	{
		this->ike_cfg->destroy(this->ike_cfg);
		this->child_cfgs->destroy_offset(this->child_cfgs,
										offsetof(child_cfg_t, destroy));
		DESTROY_IF(this->virtual_ip);
		this->local_auth->destroy_offset(this->local_auth,
										offsetof(auth_cfg_t, destroy));
		this->remote_auth->destroy_offset(this->remote_auth,
										offsetof(auth_cfg_t, destroy));
#ifdef ME
		DESTROY_IF(this->mediated_by);
		DESTROY_IF(this->peer_id);
#endif /* ME */
		this->mutex->destroy(this->mutex);
		free(this->name);
		free(this->pool);
		free(this);
	}
}

/*
 * Described in header-file
 */
peer_cfg_t *peer_cfg_create(char *name, ike_version_t ike_version,
							ike_cfg_t *ike_cfg, cert_policy_t cert_policy,
							unique_policy_t unique, u_int32_t keyingtries,
							u_int32_t rekey_time, u_int32_t reauth_time,
							u_int32_t jitter_time, u_int32_t over_time,
							bool mobike, u_int32_t dpd, host_t *virtual_ip,
							char *pool, bool mediation, peer_cfg_t *mediated_by,
							identification_t *peer_id)
{
	private_peer_cfg_t *this;

	if (rekey_time && jitter_time > rekey_time)
	{
		jitter_time = rekey_time;
	}
	if (reauth_time && jitter_time > reauth_time)
	{
		jitter_time = reauth_time;
	}

	INIT(this,
		.public = {
			.get_name = _get_name,
			.get_ike_version = _get_ike_version,
			.get_ike_cfg = _get_ike_cfg,
			.add_child_cfg = _add_child_cfg,
			.remove_child_cfg = (void*)_remove_child_cfg,
			.create_child_cfg_enumerator = _create_child_cfg_enumerator,
			.select_child_cfg = _select_child_cfg,
			.get_cert_policy = _get_cert_policy,
			.get_unique_policy = _get_unique_policy,
			.get_keyingtries = _get_keyingtries,
			.get_rekey_time = _get_rekey_time,
			.get_reauth_time = _get_reauth_time,
			.get_over_time = _get_over_time,
			.use_mobike = _use_mobike,
			.get_dpd = _get_dpd,
			.get_virtual_ip = _get_virtual_ip,
			.get_pool = _get_pool,
			.add_auth_cfg = _add_auth_cfg,
			.create_auth_cfg_enumerator = _create_auth_cfg_enumerator,
			.equals = (void*)_equals,
			.get_ref = _get_ref,
			.destroy = _destroy,
#ifdef ME
			.is_mediation = _is_mediation,
			.get_mediated_by = _get_mediated_by,
			.get_peer_id = _get_peer_id,
#endif /* ME */
		},
		.name = strdup(name),
		.ike_version = ike_version,
		.ike_cfg = ike_cfg,
		.child_cfgs = linked_list_create(),
		.mutex = mutex_create(MUTEX_TYPE_DEFAULT),
		.cert_policy = cert_policy,
		.unique = unique,
		.keyingtries = keyingtries,
		.rekey_time = rekey_time,
		.reauth_time = reauth_time,
		.jitter_time = jitter_time,
		.over_time = over_time,
		.use_mobike = mobike,
		.dpd = dpd,
		.virtual_ip = virtual_ip,
		.pool = strdupnull(pool),
		.local_auth = linked_list_create(),
		.remote_auth = linked_list_create(),
		.refcount = 1,
	);

#ifdef ME
	this->mediation = mediation;
	this->mediated_by = mediated_by;
	this->peer_id = peer_id;
#else /* ME */
	DESTROY_IF(mediated_by);
	DESTROY_IF(peer_id);
#endif /* ME */

	return &this->public;
}
