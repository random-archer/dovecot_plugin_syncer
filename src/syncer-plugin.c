/*
 * dovecot plugin: syncer
 *
 * provide support for full multi-node multi-master mail store replication
 */

#include <config-syncer.h>

//#include <sys/wait.h>

#include "lib.h"
#include "hash.h"
#include "array.h"
#include "llist.h"
#include "str.h"
#include "str-sanitize.h"
#include "env-util.h"
#include "execv-const.h"
#include "imap-util.h"
#include "mkdir-parents.h"
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

// plugin configuration keys and default values

static const char *KEY_DIR = "syncer_dir";
static const char *VAL_DIR = "~/syncer";

static const char *KEY_USE_DIR = "syncer_use_dir";
static const char *VAL_USE_DIR = "false";

static const char *KEY_USE_CONTENT = "syncer_use_content";
static const char *VAL_USE_CONTENT = "false";

static const char *KEY_PIPE = "syncer_pipe";
static const char *VAL_PIPE = "/run/dovecot/syncer/pipe";

static const char *KEY_USE_PIPE = "syncer_use_pipe";
static const char *VAL_USE_PIPE = "false";

static const char *KEY_SCRIPT = "syncer_script";
static const char *VAL_SCRIPT = "/etc/dovecot/syncer-script.sh";

static const char *KEY_USE_LOG = "syncer_use_log";
static const char *VAL_USE_LOG = "false";

static const char *KEY_DIR_MODE = "syncer_dir_mode";
static const char *VAL_DIR_MODE = "0755";

static const char *KEY_FILE_MODE = "syncer_file_mode";
static const char *VAL_FILE_MODE = "0644";

// plugin identity name
static const char *syncer_package_name = SYNCER_NAME;

// plugin identity version
static const char *syncer_package_version = SYNCER_VERSION;

struct syncer_mbox_info {
    const char *user_name;
    const char *chng_type;
    const char *mbox_name;
    const char *mbox_guid;
};

// plugin context
struct syncer_mail_user {

    //
    union mail_user_module_context module_ctx;

    //
    pool_t pool;

    //
    const char *user_name;

    //
    const char *mail_home;

    // enable plugin message logging
    bool syncer_use_log;

    // name of report directory
    const char *syncer_dir;

    // enable dir/file based reporting
    bool syncer_use_dir;

    // provide content in report guid files
    bool syncer_use_content;

    // name of report fifo pipe
    const char *syncer_pipe;

    // enable fifo/pipe based reporting
    bool syncer_use_pipe;

    // TODO
    const char *syncer_script;

    // folder creation mode mask
    int syncer_dir_mode;

    // file creation mode mask
    int syncer_file_mode;

    // collected changes storage
    HASH_TABLE(const char *, struct syncer_mbox_info *)
    mbox_maps; // maps form: mbox-guid into: change-info

};

static void syncer_ensure_folder(const char *path, int mode) {
    struct stat data = { 0 };
    if (stat(path, &data) == -1) {
        if (mkdir_parents(path, mode) < 0 && errno != EEXIST) {
            i_fatal("mkdir(%s) failed: %m", path);
        }
    }
}

static void syncer_ensure_pipe(const char *path, int dir_mode, int file_mode) {
    struct stat data = { 0 };
    if (stat(path, &data) == -1) {
        const char *p = strrchr(path, '/');
        const char *root = t_strdup_until(path, p);
        syncer_ensure_folder(root, dir_mode);
        mkfifo(path, file_mode);
    }
}

static const char *syncer_record_sep = "\t";
static const char *syncer_record_eol = "\n";
static const char syncer_logger_format[] = "%s.v%s/%s";

static void syncer_log_err(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i_error(syncer_logger_format, syncer_package_name, syncer_package_version, t_strdup_vprintf(fmt, args));
    va_end(args);
}

static void syncer_log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i_info(syncer_logger_format, syncer_package_name, syncer_package_version, t_strdup_vprintf(fmt, args));
    va_end(args);
}

static void syncer_log_debug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i_debug(syncer_logger_format, syncer_package_name, syncer_package_version, t_strdup_vprintf(fmt, args));
    va_end(args);
}

// report collected mailbox changes via:
// * log
// * fifo pipe
// * marker file
static void syncer_report_change(struct syncer_mail_user *plug_user) {

    if ( hash_table_count(plug_user->mbox_maps) == 0) {
        return; // nothing to report
    }

    // pipe reports
    const char *syncer_pipe = plug_user->syncer_pipe;
    syncer_ensure_pipe(syncer_pipe, plug_user->syncer_dir_mode, plug_user->syncer_file_mode);
    int fd_pipe = open(syncer_pipe, O_RDWR); // non-blocking fifo pipe
    if (fd_pipe < 0) {
        syncer_log_err("%s : %s %s", __func__, "failed to open: syncer_pipe", syncer_pipe);
    }

    // dir/file reports
    const char *syncer_dir = plug_user->syncer_dir;
    const char *syncer_dir_guid = i_strconcat(syncer_dir, "/guid", NULL);
    const char *syncer_dir_type = i_strconcat(syncer_dir, "/type", NULL);
    syncer_ensure_folder(syncer_dir, plug_user->syncer_dir_mode);
    syncer_ensure_folder(syncer_dir_guid, plug_user->syncer_dir_mode);
    syncer_ensure_folder(syncer_dir_type, plug_user->syncer_dir_mode);

    const char *mbox_guid;
    struct syncer_mbox_info *mbox_info;
    struct hash_iterate_context *iter = hash_table_iterate_init(plug_user->mbox_maps);
    while (hash_table_iterate(iter, plug_user->mbox_maps, &mbox_guid, &mbox_info)) {

        const char *record_entry = i_strconcat( //
                "chng_type=", mbox_info->chng_type, syncer_record_sep, //
                "user_name=", mbox_info->user_name, syncer_record_sep, //
                "mbox_name=", mbox_info->mbox_name, syncer_record_sep, //
                "mbox_guid=", mbox_info->mbox_guid, syncer_record_sep, //
                NULL);

        if (plug_user->syncer_use_log) {
            // optional plugin messages
            syncer_log_info("%s : %s", __func__, record_entry);
        }

        if (plug_user->syncer_use_pipe) {
            // report change to non-blocking fifo pipe
            int rv;
            rv = write(fd_pipe, record_entry, strlen(record_entry));
            if (rv < 0) {
                syncer_log_err("%s : %s rv=%d", __func__, "failed to pipe: record_entry", rv);
            }
            rv = write(fd_pipe, syncer_record_eol, strlen(syncer_record_eol));
            if (rv < 0) {
                syncer_log_err("%s : %s rv=%d", __func__, "failed to pipe: syncer_record_eol", rv);
            }
        }

        if (plug_user->syncer_use_dir) {
            // report change by mbox
            const char *record_guid_file = i_strconcat(syncer_dir_guid, "/", mbox_guid, NULL);
            // report via empty marker file with file name based on mbox guid
            int fd_mbox = open(record_guid_file, O_RDWR | O_CREAT | O_TRUNC, plug_user->syncer_file_mode);
            if (fd_mbox < 0) {
                syncer_log_err("%s : %s %s", __func__, "failed to open: record_guid_file", record_guid_file);
            }
            // optionally report changed fields via content inside the marker file
            if (plug_user->syncer_use_content) {
                struct ostream *output = o_stream_create_fd(fd_mbox, 0);
                o_stream_set_no_error_handling(output, TRUE);
                o_stream_nsend_str(output, record_entry);
                o_stream_flush(output);
                o_stream_destroy(&output);
            }
            i_close_fd(&fd_mbox);
            // report change by type
            const char *record_type_file = i_strconcat(syncer_dir_type, "/", mbox_info->chng_type, NULL);
            //
            int fd_type = open(record_type_file, O_RDWR | O_CREAT | O_TRUNC, plug_user->syncer_file_mode);
            if (fd_type < 0) {
                syncer_log_err("%s : %s %s", __func__, "failed to open: record_type_file", record_type_file);
            }
            i_close_fd(&fd_type);
        }

    }

    i_close_fd(&fd_pipe);

    hash_table_iterate_deinit(&iter);

}

// load text config entry
static const char* syncer_plugin_getenv_text(struct mail_user *user, const char *key, const char *value) {
    const char *text = mail_user_plugin_getenv(user, key);
    text = text == NULL ? value : text;
    return text;
}

// load boolean config entry
static bool syncer_plugin_getenv_bool(struct mail_user *user, const char *key, const char *value) {
    const char *text = syncer_plugin_getenv_text(user, key, value);
    switch (text[0]) {
    case '1':
    case 'Y':
    case 'y':
    case 'T':
    case 't':
        return TRUE;
    default:
        return FALSE;
    }
}

// load octal number config entry
static int syncer_plugin_getenv_octal(struct mail_user *user, const char *key, const char *value) {
    const char *text = mail_user_plugin_getenv(user, key);
    text = text == NULL ? value : text;
    return (int) strtol(text, NULL, 8);
}

// plugin session termination
static void syncer_mail_user_deinit(struct mail_user *user) {
    struct syncer_mail_user *plug_user = SYNCER_USER_CONTEXT(user);
    syncer_report_change(plug_user);
    hash_table_destroy(&plug_user->mbox_maps);
    plug_user->module_ctx.super.deinit(user);
}

// plugin session initialization
static void syncer_mail_user_init(struct mail_user *user) {

    struct syncer_mail_user *plug_user;
    struct mail_user_vfuncs *vfuncs = user->vlast;

    // setup plugin context
    plug_user = p_new(user->pool, struct syncer_mail_user, 1);
    MODULE_CONTEXT_SET(user, syncer_mail_user_module, plug_user);

    // wire plugin callback
    plug_user->module_ctx.super = *vfuncs;
    user->vlast = &plug_user->module_ctx.super;
    vfuncs->deinit = syncer_mail_user_deinit;

    // load user parameters
    plug_user->mail_home = user->set->mail_home;
    plug_user->user_name = user->username;
    plug_user->pool = user->pool;

    // load plugin settings
    plug_user->syncer_dir = syncer_plugin_getenv_text(user, KEY_DIR, VAL_DIR);
    plug_user->syncer_use_dir = syncer_plugin_getenv_bool(user, KEY_USE_DIR, VAL_USE_DIR);
    plug_user->syncer_use_content = syncer_plugin_getenv_bool(user, KEY_USE_CONTENT, VAL_USE_CONTENT);
    plug_user->syncer_pipe = syncer_plugin_getenv_text(user, KEY_PIPE, VAL_PIPE);
    plug_user->syncer_use_pipe = syncer_plugin_getenv_bool(user, KEY_USE_PIPE, VAL_USE_PIPE);
    plug_user->syncer_script = syncer_plugin_getenv_text(user, KEY_SCRIPT, VAL_SCRIPT);
    plug_user->syncer_use_log = syncer_plugin_getenv_bool(user, KEY_USE_LOG, VAL_USE_LOG);
    plug_user->syncer_dir_mode = syncer_plugin_getenv_octal(user, KEY_DIR_MODE, VAL_DIR_MODE);
    plug_user->syncer_file_mode = syncer_plugin_getenv_octal(user, KEY_FILE_MODE, VAL_FILE_MODE);

    // resolve to absolute path
    plug_user->syncer_dir = p_strdup(plug_user->pool, mail_user_home_expand(user, plug_user->syncer_dir));

    // collected change store
    hash_table_create(&plug_user->mbox_maps, plug_user->pool, 0, str_hash, strcmp);

}

// TODO
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

// collect mailbox changes during plugin session
static void syncer_collect_change(struct mailbox *mbox, const char *chng_type) {

    struct syncer_mail_user *plug_user = SYNCER_USER_CONTEXT(mbox->storage->user);

    struct mailbox_metadata mbox_meta;
    mailbox_get_metadata(mbox, MAILBOX_METADATA_GUID, &mbox_meta);

    const char *guid_text = guid_128_to_string(mbox_meta.guid);

    if (hash_table_lookup(plug_user->mbox_maps, guid_text)) {
        // already reported
        if (plug_user->syncer_use_log) {
            syncer_log_debug("%s : exists '%s' '%s' '%s' '%s'", //
                    __func__, chng_type, plug_user->user_name, mbox->name, guid_text);
        }
    } else {
        // create new record
        if (plug_user->syncer_use_log) {
            syncer_log_debug("%s : insert '%s' '%s' '%s' '%s'", //
                    __func__, chng_type, plug_user->user_name, mbox->name, guid_text);
        }
        const char *mbox_guid = p_strdup(plug_user->pool, guid_text);
        struct syncer_mbox_info *mbox_info = p_new(plug_user->pool, struct syncer_mbox_info, 1);
        mbox_info->chng_type = p_strdup(plug_user->pool, chng_type);
        mbox_info->user_name = p_strdup(plug_user->pool, plug_user->user_name);
        mbox_info->mbox_name = p_strdup(plug_user->pool, mbox->name);
        mbox_info->mbox_guid = mbox_guid;
        hash_table_insert(plug_user->mbox_maps, mbox_guid, mbox_info);
    }

}

static void syncer_mailbox_create(struct mailbox *mbox) {
    syncer_collect_change(mbox, "mailbox_create");
}

static void* syncer_mailbox_delete(struct mailbox *mbox) {
    syncer_collect_change(mbox, "mailbox_delete");
    return NULL; // FIXME
}

static void syncer_mailbox_update(struct mailbox *mbox) {
    syncer_collect_change(mbox, "mailbox_update");
}

static void syncer_mailbox_rename(struct mailbox *src, struct mailbox *dest) {
    UNUSED(src);
    syncer_collect_change(dest, "mailbox_rename");
}

static void syncer_mail_save(void *txn, struct mail *mail) {
    UNUSED(txn);
    syncer_collect_change(mail->box, "mail_save");
}

static void syncer_mail_copy(void *txn, struct mail *src, struct mail *dst) {
    UNUSED(txn);
    UNUSED(src);
    syncer_collect_change(dst->box, "mail_copy");
}

static void syncer_mail_expunge(void *txn, struct mail *mail) {
    UNUSED(txn);
    syncer_collect_change(mail->box, "mail_expunge");
}

static void syncer_mail_update_flags(void *txn, struct mail *mail, enum mail_flags old_flags) {
    UNUSED(txn);
    UNUSED(old_flags);
    syncer_collect_change(mail->box, "mail_update_flags");
}

static void syncer_mail_update_keywords(void *txn, struct mail *mail, const char *const*old_keywords) {
    UNUSED(txn);
    UNUSED(old_keywords);
    syncer_collect_change(mail->box, "mail_update_keywords");
}

// event handler: mailbox change
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

// event handler: user create
static struct mail_storage_hooks syncer_mail_storage_hooks = {
//
        .mail_user_created = syncer_mail_user_init,	//
//
        };

//
// plugin registration interface
//

const char *syncer_plugin_version = DOVECOT_ABI_VERSION;
const char *syncer_plugin_dependencies[] = { "notify", NULL };

void syncer_plugin_init(struct module *module) {
    syncer_context = notify_register(&syncer_vfuncs);
    mail_storage_hooks_add(module, &syncer_mail_storage_hooks);
}

void syncer_plugin_deinit(void) {
    mail_storage_hooks_remove(&syncer_mail_storage_hooks);
    notify_unregister(syncer_context);
}
