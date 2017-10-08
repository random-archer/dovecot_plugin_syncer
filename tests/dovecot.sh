#!/bin/bash

#
# invoke basic tests with dovecot server
#

set -e -u

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
    sudo rm -r -f /etc/dovecot/*
    
    echo "# provision"
    sudo cp -v -a -r ${0%/*}/etc /
        
    readonly user="arkon@private.dom"
    readonly wait="0.1"
    
    dovecot_start
}

terminate() {
    dovecot_stop
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

echo "# mail search"
sudo doveadm search -u "$user" Subject 2 OR Subject 3  
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
