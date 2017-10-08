//#include <define.h>

#include <sys/wait.h>

#include "lib.h"
#include "hash.h"
#include "array.h"
#include "llist.h"
#include "str.h"
#include "str-sanitize.h"
#include "env-util.h"
#include "execv-const.h"
#include "imap-util.h"
#include "mail-user.h"
#include "mail-storage-private.h"
#include "mail-namespace.h"
#include "istream.h"
#include "ostream.h"
#include "notify-plugin.h"

#include "syncer-plugin.h"

#define UNUSED(x) (void)(x)

#define SYNCER_USER_CONTEXT(obj) MODULE_CONTEXT(obj, syncer_mail_user_module)

static struct notify_context *syncer_context;

static MODULE_CONTEXT_DEFINE_INIT(syncer_mail_user_module, &mail_user_module_register)
;

struct syncer_mbox_info {

};

struct syncer_mail_user {
	union mail_user_module_context module_ctx;
	const char *mail_home;
	const char *user_name;
	const char *user_home;
	const char *syncer_path;
	const char *syncer_script;
	HASH_TABLE(const char *, void *)
	guids; // mail box
};

static void syncer_ensure_folder(const char * path) {
	struct stat st = { 0 };
	if (stat(path, &st) == -1) {
		mkdir(path, 0700);
	}
}

static void syncer_report_change(struct syncer_mail_user *plug_user) {

	int fd;
	struct ostream *output;
	const char *syncer_path;
	const char *record_path;

	const char *key;
	void * value;
	struct hash_iterate_context *iter;
	iter = hash_table_iterate_init(plug_user->guids);

	syncer_path = i_strconcat(plug_user->mail_home, "/", plug_user->syncer_path,
	NULL);
	syncer_ensure_folder(syncer_path);

	while (hash_table_iterate(iter, plug_user->guids, &key, &value)) {

		i_info("%s : guid=%s", __func__, key);

		record_path = i_strconcat(syncer_path, "/", key, NULL);

		fd = open(record_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
		output = o_stream_create_fd(fd, 0, FALSE);
		o_stream_destroy(&output);
		i_close_fd(&fd);
	}

	hash_table_iterate_deinit(&iter);

}

static void syncer_mail_user_deinit(struct mail_user *user) {

	struct syncer_mail_user *plug_user = SYNCER_USER_CONTEXT(user);

	syncer_report_change(plug_user);

	hash_table_destroy(&plug_user->guids);

	plug_user->module_ctx.super.deinit(user);

}

static void syncer_mail_user_init(struct mail_user *user) {

	struct mail_user_vfuncs *v = user->vlast;

	struct syncer_mail_user *plug_user;
	const char *mail_home;
	const char *user_name;
	const char *user_home;
	const char *syncer_path;
	const char *syncer_script;

	plug_user = p_new(user->pool, struct syncer_mail_user, 1);
	MODULE_CONTEXT_SET(user, syncer_mail_user_module, plug_user);

	plug_user->module_ctx.super = *v;
	user->vlast = &plug_user->module_ctx.super;
	v->deinit = syncer_mail_user_deinit;

	mail_home = user->set->mail_home;
	user_name = user->username;
	user_home = mail_user_home_expand(user, "");
	syncer_path = mail_user_plugin_getenv(user, "syncer_path");
	syncer_script = mail_user_plugin_getenv(user, "syncer_script");

	if (!syncer_path) {
		syncer_path = "syncer";
	}

	plug_user->mail_home = mail_home;
	plug_user->user_name = user_name;
	plug_user->user_home = user_home;
	plug_user->syncer_path = syncer_path;
	plug_user->syncer_script = syncer_script;

	hash_table_create(&plug_user->guids, user->pool, 0, str_hash, strcmp);

}

//static void syncer_invoke_script() {
//
//if (plug_user->syncer_script) {
//	i_info("%s : username=%s : script_path=%s : mailbox=%s : change=%s",
//			__func__, plug_user->user_name, plug_user->syncer_script,
//			mbox->name, mode);
//} else {
//	i_warning("%s : username=%s script_path is missing", __func__,
//			plug_user->user_name);
//	return;
//}
//
//	const char **exec_args;
//	exec_args = i_new(const char *, 2);
//	exec_args[0] = plug_user->script_path;
//	exec_args[1] = NULL;
//
//	// allocate
//	env_put(t_strconcat("SYNCER_MODE_TYPE=", mode, NULL));
//	env_put(t_strconcat("SYNCER_MBOX_NAME=", mbox->name, NULL));
//	env_put(t_strconcat("SYNCER_MBOX_GUID=", guid_text, NULL));
//	env_put(t_strconcat("SYNCER_USER_NAME=", plug_user->user_name, NULL));
//	env_put(t_strconcat("SYNCER_MAIL_HOME=", plug_user->mail_home, NULL));
//
//	pid_t pid = fork();
//	switch (pid) {
//	case -1:
//		i_fatal("%s : process fork failure", __func__);
//		break;
//	case 0:
//		i_debug("%s : username=%s : child script continue", __func__,
//				plug_user->user_name);
//		execvp_const(exec_args[0], exec_args);
//		i_fatal("%s : username=%s : child should never return", __func__,
//				plug_user->user_name);
//		break;
//	default:
//		i_debug("%s : username=%s : parent continue; child pid=%d", __func__,
//				plug_user->user_name, pid);
//		int status;
//		waitpid(-1, &status, WNOHANG); // clean zombie children
//		break;
//	}
//
//	// deallocate
//	env_remove("SYNCER_MODE_TYPE");
//	env_remove("SYNCER_MBOX_NAME");
//	env_remove("SYNCER_MBOX_GUID");
//	env_remove("SYNCER_USER_NAME");
//	env_remove("SYNCER_MAIL_HOME");
//
//}

static void syncer_remember_change(struct mailbox *mbox, const char * mode) {

	UNUSED(mode);

	struct syncer_mail_user *plug_user = SYNCER_USER_CONTEXT(
			mbox->storage->user);

	struct mailbox_metadata mbox_meta;
	mailbox_get_metadata(mbox, MAILBOX_METADATA_GUID, &mbox_meta);

	const char *guid_text;
	guid_text = i_strdup(guid_128_to_string(mbox_meta.guid));

	hash_table_insert(plug_user->guids, guid_text, NULL);

}

static void syncer_mailbox_create(struct mailbox *mbox) {
	syncer_remember_change(mbox, "mailbox_create");
}

static void * syncer_mailbox_delete(struct mailbox *mbox) {
	syncer_remember_change(mbox, "mailbox_delete");
	return NULL;
}

static void syncer_mailbox_update(struct mailbox *mbox) {
	syncer_remember_change(mbox, "mailbox_update");
}

static void syncer_mailbox_rename(struct mailbox *src, struct mailbox *dest) {
	UNUSED(src);
	syncer_remember_change(dest, "mailbox_rename");
}

static void syncer_mail_save(void *txn, struct mail *mail) {
	UNUSED(txn);
	syncer_remember_change(mail->box, "mail_save");
}

static void syncer_mail_copy(void *txn, struct mail *src, struct mail *dst) {
	UNUSED(txn);
	UNUSED(src);
	syncer_remember_change(dst->box, "mail_copy");
}

static void syncer_mail_expunge(void *txn, struct mail *mail) {
	UNUSED(txn);
	syncer_remember_change(mail->box, "mail_expunge");
}

static void syncer_mail_update_flags(void *txn, struct mail *mail,
		enum mail_flags old_flags) {
	UNUSED(txn);
	UNUSED(old_flags);
	syncer_remember_change(mail->box, "mail_update_flags");
}

static void syncer_mail_update_keywords(void *txn, struct mail *mail,
		const char * const *old_keywords) {
	UNUSED(txn);
	UNUSED(old_keywords);
	syncer_remember_change(mail->box, "mail_update_keywords");
}

static const struct notify_vfuncs syncer_vfuncs = {
//
		.mail_save = syncer_mail_save, //
		.mail_copy = syncer_mail_copy, //
		.mail_expunge = syncer_mail_expunge, //
		.mail_update_flags = syncer_mail_update_flags, //
		.mail_update_keywords = syncer_mail_update_keywords, //
//
		.mailbox_create = syncer_mailbox_create, //
		.mailbox_delete_begin = syncer_mailbox_delete, //
		.mailbox_rename = syncer_mailbox_rename, //
		.mailbox_update = syncer_mailbox_update, //
		//
		};

static struct mail_storage_hooks syncer_mail_storage_hooks = {
//
		.mail_user_created = syncer_mail_user_init, //
//
		};

const char *syncer_plugin_dependencies[] = { "notify", NULL };

void syncer_plugin_init(struct module *module) {
	syncer_context = notify_register(&syncer_vfuncs);
	mail_storage_hooks_add(module, &syncer_mail_storage_hooks);
}

void syncer_plugin_deinit(void) {
	mail_storage_hooks_remove(&syncer_mail_storage_hooks);
	notify_unregister(syncer_context);
}
