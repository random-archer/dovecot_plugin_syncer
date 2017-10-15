
### dovecot syncer plugin

helps to create scripts for true multi-node multi-master replication 

plugin listens on mailbox changes and reports them by:
* writing a marker file `<mail_home>/syncer/guid/<mailbox_guid>`
* sending a change event to the pipe `/run/dovecot/syncer/pipe` 

replication scripts then can watch for changes:
* new files in the `syncer/{guid,type}` folder
* new events in the `syncer/pipe` fifo
  
and invoke `doveadm sync -g <mailbox_guid>` to multiple replication nodes

### replication design

use `tinc` symmetric mail network mesh

have `tinc-up / tinc-down` script dynamically register mail replication nodes

have your replication scripts to use `tinc` dynamic nodes as replication targets  

### plugin installation

download archlinux `binary.tar.gz`

or build from scratch with `autobuild.sh`
