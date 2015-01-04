This program was written after experimenting with LXC.

LXC is a great tool but it also means your filesystem is filling with strangely numbered ids of files which will only work for a specific user using his alternative namespace. So, I sought a way to address this issue and thought it was also a great chance to brush up on the C language, and improve my knowledge of maintaining file systems.

At first I just thought of shifting the user and group id's, but then I realized that extended posix ACL's are also something to be reckoned with. They also store numeric ids albeit inside extended attributes.

Some usage info then:

```
./fs-offset -b base [-n] [-f] -- treeroot(single absolute path of dir)
-b : new base uid for tree based at basepath
-n : only do preliminary tests and print statistics (dryrun)
-f : force, ignore safety questions
-p : posix, alter posix acl qualifier ids
treeroot : the root of the tree to rebase.

CAUTION: The filesystem under the treeroot must not be accessed when running this program.
```

To compile you need to install the headers for libacl on your system and architecture, and link to libacl.

Special thanks go to:
- The people that support GNU for their great toolchain.
- The crews of `You Complete Me` and `vim`.
- The people participating in the `clang` set of tools.
- All the contributors to `stackoverflow` and its sister sites
- The great Michael Kerrisk for the terrific man-pages support.
- The contributors to LXC, namespaces and cgroups.
