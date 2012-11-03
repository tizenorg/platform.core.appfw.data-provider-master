enum pkgmgr_event_type {
	PKGMGR_EVENT_DOWNLOAD,
	PKGMGR_EVENT_INSTALL,
	PKGMGR_EVENT_UPDATE,
	PKGMGR_EVENT_UNINSTALL,
	PKGMGR_EVENT_RECOVER,
};

enum pkgmgr_status {
	PKGMGR_STATUS_START,
	PKGMGR_STATUS_PROCESSING,
	PKGMGR_STATUS_COMMAND,
	PKGMGR_STATUS_END,
	PKGMGR_STATUS_ERROR,
};

extern int pkgmgr_init(void);
extern int pkgmgr_fini(void);

extern int pkgmgr_add_event_callback(enum pkgmgr_event_type type, int (*cb)(const char *pkgname, enum pkgmgr_status status, double value, void *data), void *data);

extern void *pkgmgr_del_event_callback(enum pkgmgr_event_type type, int (*cb)(const char *pkgname, enum pkgmgr_status status, double value, void *data), void *data);

/* End of a file */
