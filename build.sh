#!/bin/bash

g++ -g example.c -Inode_modules/native-video-deps/include -L/usr/lib/x86_64-linux-gnu/ -lm -lpthread -lavcodec -lavformat -lavutil -lswresample -lswscale
