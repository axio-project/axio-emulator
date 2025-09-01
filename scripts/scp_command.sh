#!/bin/bash

# remote server info
remote_host_name="Desktop_01"

if [ $1 == "--remote-to-local" ]; then
    scp Desktop_01:$2 $3 > /dev/null
    exit 0
elif [ $1 == "--local-to-remote" ]; then
    scp $2 Desktop_01:$3 > /dev/null
    exit 0
else
    echo "Invalid argument"
    exit 1
fi

