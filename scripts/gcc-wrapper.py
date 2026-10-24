#! /usr/bin/env python
# -*- coding: utf-8 -*-

from __future__ import print_function
import errno
import os
import sys
import subprocess

def run_gcc():
    args = sys.argv[1:]
    try:
        proc = subprocess.Popen(args)
        return proc.wait()
    except OSError as e:
        if e.errno == errno.ENOENT:
            sys.stderr.write(args[0] + ': ' + e.strerror + '\n')
            sys.stderr.write('Is your PATH set correctly?\n')
        else:
            sys.stderr.write(' '.join(args) + ' ' + str(e) + '\n')
        return e.errno

if __name__ == '__main__':
    status = run_gcc()
    sys.exit(status)
