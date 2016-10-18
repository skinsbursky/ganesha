# nfs-ganesha

NFS-Ganesha is an NFSv3,v4,v4.1 fileserver that runs in user mode on most UNIX/Linux systems.

## Documentation:

User and developer information can be found [here](https://github.com/nfs-ganesha/nfs-ganesha/wiki/Docs).

## Build instructions

### Getting the source

The --recursive option tells git to clone all the submodules too. 

```sh
 $ git clone --recursive ssh://git@git.acronis.com:7989/strg/pstorage-nfs.git
```

You must initialize the submodule after clone if you did not use the --recursive option. You must also do it after pulling a new update or checking out a new branch. Go to the root of your repository and enter:
```sh
 $ git submodule update --init
```

Checkout to acronis developers branch:
```sh
 $ git checkout acronis-dev
```

### Dependencies

nfs-ganesha has a few dependencies. On a red-hat based linux distribution, this should be enough for most of it:

```sh
 # Common things
 $ yum install gcc git cmake autoconf libtool bison flex
 # More nfs-ganesha specific
 $ yum install libgssglue-devel openssl-devel nfs-utils-lib-devel doxygen
```

### Compiling NFS-GANESHA with cmake

With cmake things are pretty straightforward:

```sh
 $ mkdir build
 $ cs build
 $ cmake ../src/ -DUSE_GUI_ADMIN_TOOLS=OFF -DUSE_FSAL_PROXY=OFF -DUSE_FSAL_VFS=OFF -DUSE_FSAL_PANFS=OFF -DUSE_FSAL_GPFS=OFF -DUSE_FSAL_GLUSTER=OFF -DUSE_FSAL_CEPH=OFF -DUSE_FSAL_LUSTRE=OFF -DUSE_FSAL_XFS=OFF -USE_FSAL_XFS=OFF -DUSE_FSAL_ZFS=off -DUSE_DBUS=ON -DSTRICT_PACKAGE=ON -DALLOCATOR=libc
 $ make && make rpms && make install
```
