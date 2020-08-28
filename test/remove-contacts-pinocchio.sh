#!/bin/bash

ACCOUNT_NAME="dummy0"
MC_PRESENCE_AVAILABLE=2

# ensure the connection/connection manager have settled down (can be a problem
# if we run this immediately after other tests, as we do with 'make check')
sleep 3

# ensure we have a clean contact list
pinocchio-ctl reset

# XXX: for some reason, we need to wait for something to settle out here
sleep 3

# enable the account
mc-account enable $ACCOUNT_NAME

# set our presence to "available" (2). Block until it completes
dbus-send --session --dest=org.freedesktop.Telepathy.MissionControl \
    --print-reply \
    --type=method_call \
    /org/freedesktop/Telepathy/MissionControl \
    org.freedesktop.Telepathy.MissionControl.SetPresence \
    uint32:$MC_PRESENCE_AVAILABLE string:''

presence=""
tries_left=30

# stall until we're actually online; this can be more than a second for
# pinocchio, and much longer for real protocols
while [ "x$presence" != "x$MC_PRESENCE_AVAILABLE" ]; do
    presence=$(dbus-send --session \
        --dest=org.freedesktop.Telepathy.MissionControl \
        --print-reply \
        --type=method_call \
        /org/freedesktop/Telepathy/MissionControl \
        org.freedesktop.Telepathy.MissionControl.GetPresenceActual)
    
    presence=$(echo $presence | awk '{print $8}')

    # make sure we don't get in an infinite loop if something got horribly
    # broken
    tries_left=$(($tries_left - 1))
    if [ $tries_left -le 0 ]; then
        echo >&2 "ERROR: Mission Control never got online"
        exit 1
    fi

    sleep 1
done

# run the core test itself
./remove-contacts $ACCOUNT_NAME
TEST_RESULT=$?

# clean up after ourselves
pinocchio-ctl reset

exit $TEST_RESULT
