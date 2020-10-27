
## README from the orginal bitbucket project, now owned by Vita Nuova

[  Inferno® is a distributed operating system, originally developed at Bell Labs, but now developed and maintained by Vita Nuova® as Free Software.  Applications written in Inferno's concurrent programming language, Limbo, are compiled to its portable virtual machine code (Dis), to run anywhere on a network in the portable environment that Inferno provides.  Unusually, that environment looks and acts like a complete operating system.

Inferno represents services and resources in a file-like name hierarchy.  Programs access them using only the file operations open, read/write, and close.  `Files' are not just stored data, but represent devices, network and protocol interfaces, dynamic data sources, and services.  The approach unifies and provides basic naming, structuring, and access control mechanisms for all system resources.  A single file-service protocol (the same as Plan 9's 9P) makes all those resources available for import or export throughout the network in a uniform way, independent of location. An application simply attaches the resources it needs to its own per-process name hierarchy ('name space').

Inferno can run 'native' on various ARM, PowerPC, SPARC and x86 platforms but also 'hosted', under an existing operating system (including AIX, FreeBSD, IRIX, Linux, MacOS X, Plan 9, and Solaris), again on various processor types.

This Bitbucket project includes source for the basic applications, Inferno itself (hosted and native), all supporting software, including the native compiler suite, essential executables and supporting files.  ]


## README for my modifications

This repo is a modified pre-built version of the bitbucket repo from: https://bitbucket.org/inferno-os/inferno-os/src/master/
The build configurations is listed in mkconfig. If building for another system different than the config given, please change the mkconfig file and "mk" again

**The modifications in this repo against the orgiginal:**
- Added welcoming line with time and user name at OS emulation start up (in emu/port/dis.c)
- Replace round-robin with multi-level queue (similar to that of Linux) process scheduling (in emu/port/dis.c). This will further ensure "fairness" of scheduling for processes. Well, still depending on your definition of fairness
- Added a memory slab for allocation of small memory block (of size < 4MB). Previously, the original has a tree data structure (with each node is a doubly-linked list) pointing to available memory blocks. However, accessing blocks this qay will take a (best) time complexity of O(lgn) with n being the number of nodes and more if each node only has 1 block in its list, or memory block of appropriate size is not in tree). With the new slab system, any blocks with size < 4MB (the size range most frequently allocated) will be put on an array of pointers to heads of singly-linked list, making the access time O(1) for cases that blocks of appropriate size are in the slab, and divert back to the tree method when no block of requested size is on slab. 
- Added a RAID 0 (striping) driver in emu/port/devraid0.c, which makes an abstraction of writing to 1 "file"/disk while actually writing even-numbered bytes on 1 "file"/disk and odd-numbered bytes on another file/disk. Refer to specific documentation on how to use this.

*More detailed documentation on each of these new features are included in the doc folder in pdf form: Multi-level_queue.pdf, Slab_MM.pdf and RAID0driver.pdf.*
