#!/usr/bin/env python
"""
Attempt to figure out what options need to be added to the Exim
Makefile to embed a Python interpreter for your particular platform.

Most of the info seems to be available in the Python distutils.sysconfig
module, cross your fingers.

  2002-10-19 Barry Pederson <bp@barryp.org>

"""
import distutils.sysconfig, os.path

def patch_makefile(old_makefile, new_makefile):
    makefile = open(old_makefile, 'r').readlines()

    cfg = distutils.sysconfig.get_config_var
    cflags = '-I%s %s' % (cfg('INCLUDEPY'), cfg('CFLAGSFORSHARED'))
    lib = os.path.join(cfg('LIBPL'), cfg('LIBRARY'))
    extralibs =  ' '.join([cfg('LIBM'), cfg('LIBS'), cfg('LDFLAGS'), lib, cfg('LINKFORSHARED')])
    source = 'Local/expy_local_scan.c'

    #
    # Look for existing CFLAGS and EXTRALIBS lines, and append info.  
    # Note if this has been done by setting cflags and extralibs to None
    #
    for i in range(len(makefile)):
        if makefile[i].startswith('CFLAGS=') and cflags:
            makefile[i] = makefile[i].rstrip() + ' ' + cflags + '\n'
            cflags = None
        if makefile[i].startswith('EXTRALIBS=') and extralibs:
            makefile[i] = makefile[i].rstrip() + ' ' + extralibs + '\n'
            extralibs = None
        if makefile[i].startswith('LOCAL_SCAN_SOURCE='):
            makefile[i] = 'LOCAL_SCAN_SOURCE=' + source + '\n'
            source = None

    #
    # Didn't update existing lines? append new ones
    #
    if cflags:
        makefile.append('CFLAGS=%s\n' % cflags)
    if extralibs:
        makefile.append('EXTRALIBS=%s\n' % extralibs)
    if source:
        makefile.append('LOCAL_SCAN_SOURCE=%s\n' % source)

    #
    # Write out updated makefile
    #
    makefile = ''.join(makefile)
    open(new_makefile, 'w').write(makefile)    


if __name__ == '__main__':
    import sys
    if len(sys.argv) < 3:
        print 'Attempt to patch Exim makefile to support Python local_scan'
        print '    Usage: %s <original-makefile> <new-makefile>' % sys.argv[0]
        print ''
        print 'Suggested path for your local_scan module:'
        print '    ', os.path.join(distutils.sysconfig.get_python_lib(), 'exim_local_scan.py')
        sys.exit(1)

    patch_makefile(sys.argv[1], sys.argv[2])
