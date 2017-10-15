#!/bin/bash

#
# invoke basic tests with dovecot server
#

set -u -e -E

piper_service() {
    echo "# piper start"
    [[ -p $pipe ]] || {
        sudo mkdir -p ${pipe%/*}
        sudo mkfifo $pipe
        sudo chmod 0644 $pipe # non-blocking
    }
    #
    local line= ; 
    while true ; do # re-connect pipe w/o loss
        exec 3<$pipe
        exec 4<&-
        while read -r event; do # read report record
            local $event # inject params
            echo "piper: '$chng_type' '$user_name' '$mbox_name' '$mbox_guid'"
        done <&3
        exec 4<&3
        exec 3<&-
    done
    #
    echo "# piper finish"
}

dovecot_start() {
    sudo systemctl start dovecot
    while &> /dev/null ! systemctl is-active dovecot ; do
        sleep 1
    done
    echo "# dovecot: start"
}

dovecot_stop() {
    sudo systemctl stop dovecot
    while &> /dev/null systemctl is-active dovecot ; do
        sleep 1
    done
    echo "# dovecot: stop"
}

initialize() {
    echo "# cleanup"
    sudo rm -r -f /etc/dovecot*
    sudo rm -r -f /tmp/dovecot*
    sudo mkdir -p /tmp/dovecot
        
    echo "# provision"
    sudo cp -v -a -r ${BASH_SOURCE%/*}/etc /
        
    readonly user="arkon@private.dom"
    readonly home="/etc/dovecot/data/private.dom/arkon"
    mkdir -p $home
    readonly pipe="/tmp/dovecot/syncer-pipe/pipe"
    readonly sieve_script="$home/sieve/active.sieve"
    readonly sieve_filter=(
        'require "fileinto" ;'
        'if header :contains "subject" "2"'
        '{ fileinto "tester" ; }'
    )
    readonly wait="0.1"
    
    (piper_service) &
    
    dovecot_start
}

terminate() {
    #
    dovecot_stop
    #
    local list=$(jobs -p)
    if [[ $list ]] ; then kill $list; fi # stop subs
}

trap terminate EXIT

initialize

echo "# mailbox create"
sudo doveadm mailbox create -u "$user" 'INBOX'
sleep "$wait"

echo "# mailbox create"
sudo doveadm mailbox create -u "$user" 'tester'
sleep "$wait"

echo "# mail save"
echo -e "Subject: 1 \n save 1" | sudo doveadm save -u "$user" -m 'INBOX'
echo -e "Subject: 2 \n save 2" | sudo doveadm save -u "$user" -m 'INBOX'
echo -e "Subject: 3 \n save 3" | sudo doveadm save -u "$user" -m 'INBOX'
echo -e "Subject: 4 \n save 4" | sudo doveadm save -u "$user" -m 'INBOX'
sleep "$wait"

echo "# sieve filter"
echo "${sieve_filter[@]}" | sudo doveadm sieve put -u "$user" 'filter'
sudo doveadm sieve activate -u "$user" 'filter'
sudo sieve-filter -e -W -u "$user" "$sieve_script" 'INBOX'

echo "# mail search"
sudo doveadm search -u "$user" Subject 2
sleep "$wait"

echo "# mail copy"
sudo doveadm copy -u "$user" 'tester' mailbox 'INBOX' all
sleep "$wait"

echo "# mail copy"
sudo doveadm copy -u "$user" 'tester' mailbox 'INBOX' all
sleep "$wait"

echo "# mail expunge"
sudo doveadm expunge -u "$user" mailbox 'INBOX' all
sleep "$wait"

echo "# mailbox rename"
sudo doveadm mailbox rename -u "$user" 'tester' 'tester-rename'
sleep "$wait"
                              
echo "# mailbox rename"
sudo doveadm mailbox rename -u "$user" 'tester-rename' 'tester'
sleep "$wait"

echo "# mail search"
sudo doveadm search -u "$user" mailbox 'tester' all  
sleep "$wait"

echo "# mailbox delete"
sudo doveadm mailbox delete -u "$user" 'tester' 
sleep "$wait"

echo "# report change"
sudo ls -Rlas $home/syncer*
sudo ls -Rlas /tmp/dovecot/syncer*
    