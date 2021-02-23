#!/bin/sh

set -eu

TEST_DIR=$(dirname "$0")/..
# shellcheck source=/dev/null
. "$TEST_DIR/lib.sh"

assert_num_children() {
    assert "$1 services are running" "$(texec pgrep -P 1 "$2" | wc -l)" -eq "$1"
}

test_teardown() {
    say "Test done $(date)"
    say "Running test teardown..."

    # Teardown
    rm -f test_root/bin/service.sh
}

# Test
say "Test start $(date)"
# toggle_finit_debug
say "Add a dynamic service in $FINIT_CONF_DIR/service.conf"
cp "$TEST_DIR"/common/service.sh "$TEST_DIR"/test_root/test_assets/
texec sh -c "echo 'service [2345] kill:20 log /test_assets/service.sh pid:/run/service.pid' > $FINIT_CONF_DIR/service.conf"

say 'Reload Finit'
texec kill -SIGHUP 1

pause

retry 'assert_num_children 1 service.sh' 5

say 'Remove the dynamic service from /etc/finit.d/service.conf'
texec sh -c "echo > $FINIT_CONF_DIR/service.conf"

say 'Reload Finit'
texec kill -SIGHUP 1

retry 'assert_num_children 0 service.sh' 5
