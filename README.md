
## vrunas
---------------

* [Overview](#overview)
* [System Requirments](#systemrequirments)
* [Compilation](#compilation)
* [Contact](#contact)
* [License](#license)

## Overview
**vrunas** is a simple setuid()/setgid() wrapper running a process under specific user/group identity.
Additionaly, 
- it can print the id of a given user/group: 'uidgid=$(./vrunas -U root -G wheel)'
- it can print timings of the run process: 'vrunas -t sleep 2'
- it can redirect stderr/stdout to stdout/stderr/anyFile: 'vrunas -1 -o log ls / /notfound'

## System requirements
- A somewhat capable compiler (gcc/clang), make (GNU,BSD), sh (sh/bash/ksh)
  and coreutils (awk,grep,sed,date,touch,head,printf,which,find,test,...)

This is not an exhaustive list but the list of systems on which it has been built:
- Linux: slitaz 4 2.6.37, ubuntu 12.04 3.11.0, debian9.
- OSX 10.11.6
- OpenBSD 5.5
- FreeBSD 11.1

## Compilation
Make sure you clone the repository with '--recursive' option.  
    $ git clone --recursive https://github.com/vsallaberry/vrunas

Just type:  
    $ make # (or 'make -j3' for SMP)

If the Makefile cannot be parsed by 'make', try:  
    $ ./make-fallback

Most of utilities used in Makefile are defined in variables and can be changed
with something like 'make SED=gsed TAR=gnutar' (or ./make-fallback SED=...)

To See how make understood the Makefile, you can type:  
    $ make info # ( or ./make-fallback info)

When making without version.h created (not the case for this repo), some old
bsd make can stop. Just type again '$ make' and it will be fine.

## Contact
[vsallaberry@gmail.com]  
<https://github.com/vsallaberry/vrunas>

## License
GPLv3 or later. See LICENSE file.

CopyRight: Copyright (C) 2018-2019 Vincent Sallaberry

