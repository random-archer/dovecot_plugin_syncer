
### dovecot syncer plugin

helps to create scripts for true multi-node multi-master replication 

plugin listens on all mailbox changes and reports them
by writing a marker file `<mail_home>/syncer/<mailbox_guid>` 

scripts then can watch for changes in the `syncer` folder
and invoke `doveadm sync -g <mailbox_guid>` to multiple replication members

download binary file for archlinux `binary.tar.gz`

or build from scratch with `autobuild.sh`
