#!/bin/bash

if [ "$1" == "--debug" ]; then
    cc='gcc -g -Wall -Wextra -std=c99 -pedantic'
else
    cc='gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -std=c99 -pedantic'
fi

$cc -o cserver main.c utils.c request.c response.c routing.c templating.c forum.c resource_cache.c
