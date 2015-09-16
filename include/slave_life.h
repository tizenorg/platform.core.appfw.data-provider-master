/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*!
 * Managing the reference counter of a slave
 */

struct slave_node;

enum slave_event {
	SLAVE_EVENT_ACTIVATE,
	SLAVE_EVENT_DEACTIVATE, /* deactivate callback, can return REACTIVATE, DEFAULT */
	SLAVE_EVENT_DELETE, /* Callbacks for this event type, must has not to do something with slave object.
			       use this only for just notice the state of slave */
	SLAVE_EVENT_FAULT, /* Critical fault */

	SLAVE_EVENT_PAUSE,
	SLAVE_EVENT_RESUME,

	SLAVE_NEED_TO_REACTIVATE
};

enum slave_state {
	/**
	 * @note
	 * Launch the slave but not yet receives "hello" packet
	 */
	SLAVE_REQUEST_TO_LAUNCH,

	/**
	 * @note
	 * Terminate the slave but not yet receives dead signal
	 */
	SLAVE_REQUEST_TO_TERMINATE,

	/**
	 * @note
	 * No slave process exists, just slave object created
	 */
	SLAVE_TERMINATED,

	/**
	 * @note
	 * State change request is sent,
	 */
	SLAVE_REQUEST_TO_PAUSE,
	SLAVE_REQUEST_TO_RESUME,

	/**
	 * @note
	 * Request an action for disconnecting to master from the provider side.
	 * This flag should be treated as an activated state.
	 */
	SLAVE_REQUEST_TO_DISCONNECT,

	/**
	 * @note
	 * SLAVE_ACTIVATED = { SLAVE_PAUSED, SLAVE_RESUMED }
	 */
	SLAVE_PAUSED,
	SLAVE_RESUMED,

	SLAVE_ERROR = 0xFF /* Explicitly define the size of this enum type */
};

enum PROVIDER_CTRL {
	PROVIDER_CTRL_DEFAULT = 0x00,			/*!< Set default control operation */
	PROVIDER_CTRL_MANUAL_TERMINATION = 0x01,	/*!< Terminate process manually */
	PROVIDER_CTRL_MANUAL_REACTIVATION = 0x02,	/*!< Reactivate process manually */
};

extern struct slave_node *slave_ref(struct slave_node *slave);
extern struct slave_node *slave_unref(struct slave_node *slave);
extern const int const slave_refcnt(struct slave_node *slave);

/*!
 * \brief
 * Create a new slave object or destroy it
 *
 * \param[in] name
 * \param[in] is_secured
 * \param[in] abi
 * \param[in] pkgname
 * \param[in] period
 * \return slave_node
 */
extern struct slave_node *slave_create(const char *name, int is_secured, const char *abi, const char *pkgname, int network, const char *hw_acceleration);

/*!
 * \brief
 * \param[in] slave
 * \return void
 */
extern void slave_destroy(struct slave_node *slave);

/*!
 * \brief
 * Launch or terminate a slave
 * \param[in] slave
 * \return int
 */
extern int slave_activate(struct slave_node *slave);

/*!
 * To check the slave's activation state
 */
extern const int const slave_is_activated(struct slave_node *slave);
extern int slave_activated(struct slave_node *slave);

extern int slave_give_more_ttl(struct slave_node *slave);
extern int slave_freeze_ttl(struct slave_node *slave);
extern int slave_thaw_ttl(struct slave_node *slave);
extern int slave_expired_ttl(struct slave_node *slave);

/*!
 * \NOTE
 * To mangage the unexpected termination of a slave
 * After this function call, the slave object can be deleted
 */
extern struct slave_node *slave_deactivated_by_fault(struct slave_node *slave) __attribute__((warn_unused_result));

/*!
 * \NOTE
 * After this function, the slave object can be deleted
 */
extern struct slave_node *slave_deactivated(struct slave_node *slave) __attribute__((warn_unused_result));

extern int slave_event_callback_add(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data);
extern int slave_event_callback_del(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data);

extern int slave_set_data(struct slave_node *slave, const char *tag, void *data);
extern void *slave_del_data(struct slave_node *slave, const char *tag);
extern void *slave_data(struct slave_node *slave, const char *tag);

extern struct slave_node *slave_find_by_pid(pid_t pid);
extern struct slave_node *slave_find_by_name(const char *name);
extern struct slave_node *slave_find_by_pkgname(const char *pkgname);
extern struct slave_node *slave_find_by_rpc_handle(int handle);

extern void slave_dead_handler(struct slave_node *slave);
extern const int const slave_is_secured(const struct slave_node *slave);
extern const int const slave_is_app(const struct slave_node *slave);
extern const char * const slave_name(const struct slave_node *slave);
extern const pid_t const slave_pid(const struct slave_node *slave);
extern const char * const slave_abi(const struct slave_node *slave);
extern int slave_set_pid(struct slave_node *slave, pid_t pid);

/*!
 * \note
 * Used for making decision of destroying a slave or not
 * Used for balancing load of the slave.
 */
extern void slave_load_package(struct slave_node *slave);
extern void slave_unload_package(struct slave_node *slave);
extern int const slave_loaded_package(struct slave_node *slave);
extern struct slave_node *slave_find_available(const char *slave_pkgname, const char *abi, int secured, int network, const char *hw_acceleration, int auto_align);

extern double const slave_ttl(const struct slave_node *slave);

/*!
 * \note
 * Used for making decision of activating a slave or not
 */
extern void slave_load_instance(struct slave_node *slave);

/*!
 * \NOTE
 * After this function call, the slave object can be deleted.
 */
extern struct slave_node *slave_unload_instance(struct slave_node *slave) __attribute__((warn_unused_result));

extern int const slave_loaded_instance(struct slave_node *slave);

extern int slave_resume(struct slave_node *slave);
extern int slave_pause(struct slave_node *slave);

extern const char *slave_pkgname(const struct slave_node *slave);
extern const char *slave_state_string(const struct slave_node *slave);

extern enum slave_state slave_state(const struct slave_node *slave);
extern void slave_set_state(struct slave_node *slave, enum slave_state state);

extern const void *slave_list(void);
extern int const slave_fault_count(const struct slave_node *slave);

extern int slave_need_to_reactivate_instances(struct slave_node *slave);
extern void slave_set_reactivate_instances(struct slave_node *slave, int reactivate);

extern void slave_set_reactivation(struct slave_node *slave, int flag);
extern int slave_need_to_reactivate(struct slave_node *slave);

extern int slave_network(const struct slave_node *slave);
extern void slave_set_network(struct slave_node *slave, int network);

extern int slave_deactivate_all(int reactivate, int reactivate_instances, int no_timer);
extern int slave_activate_all(void);

extern void slave_set_control_option(struct slave_node *slave, int ctrl);
extern int slave_control_option(struct slave_node *slave);

extern char *slave_package_name(const char *abi, const char *lbid);

extern int slave_priority(struct slave_node *slave);
extern int slave_set_priority(struct slave_node *slave, int priority);

extern int slave_valid(const struct slave_node *slave);
extern void slave_set_valid(struct slave_node *slave);

extern void slave_set_extra_bundle_data(struct slave_node *slave, const char *extra_bundle_data);
extern const char *slave_extra_bundle_data(struct slave_node *slave);

extern int slave_is_watch(struct slave_node *slave);
extern void slave_set_is_watch(struct slave_node *slave, int flag);

extern int slave_set_resource_limit(struct slave_node *slave, unsigned int soft, unsigned int hard);
extern int slave_get_resource_limit(struct slave_node *slave, unsigned int *soft, unsigned int *hard);

extern void slave_set_wait_deactivation(struct slave_node *slave, int wait);
extern int slave_wait_deactivation(struct slave_node *slave);

/* End of a file */
