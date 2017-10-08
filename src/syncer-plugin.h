#ifndef SYNCER_PLUGIN_H
#define SYNCER_PLUGIN_H

extern const char *syncer_plugin_dependencies[];

void syncer_plugin_init(struct module *module);
void syncer_plugin_deinit(void);

#endif
