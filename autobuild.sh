#!/bin/bash

#
# rebuild from scratch
# note: run inside build container
#

set -e -u

provision_packages() {
    echo "# $FUNCNAME"
    if pacman -Q dovecot ; then
        return 0
    fi
    sudo pacman -S \
        dovecot \
        pigeonhole \
        base-devel
}

generate_configure() {
    echo "# $FUNCNAME"
    autoreconf \
        --force \
        --install
}

generate_makefile() {
    echo "# $FUNCNAME"
    "${0%/*}"/configure \
        --prefix=/usr \
        --disable-static \
        --with-moduledir=/usr/lib/dovecot/modules
}

run_build_cycle() {
    echo "# $FUNCNAME"
    make
    sudo make install
    make check
}

package_binary() {
    echo "# $FUNCNAME"
    local package="${0%/*}/package"
    mkdir -p "$package"
    make DESTDIR="$package" install
    local binary="${0%/*}/binary.tar.gz"
    tar czvf "$binary" --exclude='*.la' -C "$package" "."
}


provision_packages
generate_configure
generate_makefile
run_build_cycle
package_binary
