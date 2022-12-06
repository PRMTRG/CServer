#!/bin/bash
set -eu

gcc -pthread -O2 -Wall -Wextra -Wno-unused-function -std=c99 -pedantic -o bench bench.c
