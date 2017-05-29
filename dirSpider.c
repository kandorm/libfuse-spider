/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresponding user-space
 * libc functions. Its performance is terrible.
 *
 * Compile with
 *
 *     gcc -Wall dirSpider.c `pkg-config fuse3 --cflags --libs` -lcspider -I /usr/include/libxml2 -o dirSpider
 *
 * ## Source code ##
 * \include passthrough.c
 */


#define FUSE_USE_VERSION 30

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "c_list.h"
#include <stdlib.h>

#define MAX_NAMELEN 255

struct d_inode {
	char name[MAX_NAMELEN + 1];
	mode_t mode;
	unsigned int count;
	struct list_node file_entries;
	struct list_node dir_entries;
	struct list_node node;
};

struct f_inode {
	char name[MAX_NAMELEN + 1];
	void* contents;
	size_t size;
	unsigned int ref;
    mode_t mode;
    struct list_node node;
};

static struct d_inode *rootDir;

static void *xmp_init(struct fuse_conn_info *conn,
		      struct fuse_config *cfg)
{
	(void) conn;
	cfg->use_ino = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;

	return NULL;
}

static int get_f_inode(const char *path, struct f_inode *out) {
	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[3] = "/ ";
	char *p, *next;
	p = strtok(buf, delim);

	struct d_inode *cur_node = rootDir;
	struct list_node* n;
	while((next = strtok(NULL, delim))) {
		int exist = 0;
		list_for_each (n, &cur_node->dir_entries) {
			struct d_inode* o = list_entry(n, struct d_inode, node);
			if(strcmp(o->name, p) == 0) {
				cur_node = o;
				exist = 1;
				break;
			}
		}
		if(exist == 0)
			return -ENOENT;
		p = next;
	}
	list_for_each (n, &cur_node->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if(strcmp(o->name, p) == 0) {
			*out = *o;
			return 0;
		}
	}
	return -ENOENT;
}

static int get_d_inode(const char *path, struct d_inode *out) {
	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[3] = "/ ";
	char *p, *next;
	p = strtok(buf, delim);

	struct d_inode *cur_node = rootDir;
	struct list_node* n;
	while((next = strtok(NULL, delim))) {
		int exist = 0;
		list_for_each (n, &cur_node->dir_entries) {
			struct d_inode* o = list_entry(n, struct d_inode, node);
			if(strcmp(o->name, p) == 0) {
				cur_node = o;
				exist = 1;
				break;
			}
		}
		if(exist == 0)
			return -ENOENT;
		p = next;
	}
	list_for_each (n, &cur_node->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if(strcmp(o->name, p) == 0) {
			*out = *o;
			return 0;
		}
	}
	return -ENOENT;
}

static int xmp_getattr(const char *path, struct stat *st,
		       struct fuse_file_info *fi)
{
	/*
	(void) fi;
	int res;

	res = lstat(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
	*/
	struct d_inode *d_o = (struct d_inode *)malloc(sizeof(struct d_inode));
	memset(st, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		st->st_mode = 0755 | S_IFDIR;
		st->st_nlink = 2;
		st->st_size = 0;
		struct list_node* n;
		list_for_each (n, &rootDir->file_entries) {
			struct f_inode* o = list_entry(n, struct f_inode, node);
			++st->st_nlink;
			st->st_size += strlen(o->name);
		}
		list_for_each (n, &rootDir->dir_entries) {
			struct d_inode* o = list_entry(n, struct d_inode, node);
			++st->st_nlink;
			st->st_size += strlen(o->name);
		}
		return 0;

	} else if(get_d_inode(path, d_o) == 0) {

		st->st_mode = 0755 | S_IFDIR;
		st->st_nlink = 2;
		st->st_size = 0;

		list_init(&d_o->file_entries);
		list_init(&d_o->dir_entries);
		struct list_node* n;
		list_for_each (n, &d_o->file_entries) {
			struct f_inode* o = list_entry(n, struct f_inode, node);
			++st->st_nlink;
			st->st_size += strlen(o->name);
		}
		list_for_each (n, &d_o->dir_entries) {
			struct d_inode* o = list_entry(n, struct d_inode, node);
			++st->st_nlink;
			st->st_size += strlen(o->name);
		}

		return 0;
	} else {
		struct f_inode *f_o = (struct f_inode *)malloc(sizeof(struct f_inode));
		if(get_f_inode(path, f_o))
			return -ENOENT;
		st->st_mode = 0644 | S_IFREG;
		st->st_nlink = 1;
		st->st_size = f_o->size;
		return 0;
	}

	return -ENOENT;
}

static int xmp_access(const char *path, int mask)
{
	int res;

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	int res;

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}


static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
	/*
	DIR *dp;
	struct dirent *de;

	(void) offset;
	(void) fi;
	(void) flags;

	dp = opendir(path);
	if (dp == NULL)
		return -errno;

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0, 0))
			break;
	}

	closedir(dp);
	return 0;
    */
	struct list_node* n;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	if(strcmp(path, "/") == 0) {
		list_for_each (n, &rootDir->file_entries) {
			struct f_inode* o = list_entry(n, struct f_inode, node);
			filler(buf, o->name, NULL, 0, 0);
		}
		list_for_each (n, &rootDir->dir_entries) {
			struct d_inode* o = list_entry(n, struct d_inode, node);
			filler(buf, o->name, NULL, 0, 0);
		}
		return 0;
	}
	/*
	struct d_inode *d_o = (struct d_inode *)malloc(sizeof(struct d_inode));;
	if(get_d_inode(path, d_o))
		return -ENOENT;
	list_for_each (n, &d_o->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		filler(buf, o->name, NULL, 0, 0);
	}
	list_for_each (n, &d_o->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		filler(buf, o->name, NULL, 0, 0);
	}
*/
	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	/*
	int res;

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;
	*/
	struct list_node* n;

	if (strlen(path + 1) > MAX_NAMELEN)
		return -ENAMETOOLONG;

	list_for_each (n, &rootDir->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(path + 1, o->name) == 0)
			return -EEXIST;
	}

	list_for_each (n, &rootDir->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if (strcmp(path + 1, o->name) == 0)
			return -EEXIST;
	}

	struct d_inode* d_o = (struct d_inode *)malloc(sizeof(struct d_inode));
	strcpy(d_o->name, path + 1); /* skip leading '/' */
	d_o->mode = mode | S_IFDIR | 0755;
	d_o->count = 0;
	list_init(&d_o->file_entries);
	list_init(&d_o->dir_entries);
	list_add_prev(&d_o->node, &rootDir->dir_entries);
	return 0;
}

static int xmp_unlink(const char *path)
{
	/*
	int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
	*/
	struct list_node *n, *p;

	list_for_each_safe (n, p, &rootDir->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(path + 1, o->name) == 0) {
			__list_del(n);
			free(o);
			return 0;
		}
	}

	return -ENOENT;
}

static void free_dir_node(struct d_inode *d_node) {
	struct list_node *n, *p;
	list_for_each_safe (n, p, &d_node->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		__list_del(n);
		free(o);
	}
	list_for_each_safe (n, p, &d_node->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		free_dir_node(o);
	}
	return;
}

static int xmp_rmdir(const char *path)
{
	/*
	int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;
	*/

	struct list_node *n, *p;;
	list_for_each_safe (n, p, &rootDir->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if (strcmp(path + 1, o->name) == 0) {
			__list_del(n);
			free_dir_node(o);
			return 0;
		}
	}

	return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
	int res;

	if (flags)
		return -EINVAL;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode,
		     struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
		     struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size,
			struct fuse_file_info *fi)
{
	int res;

	if (fi != NULL)
		res = ftruncate(fi->fh, size);
	else
		res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2],
		       struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_create(const char *path, mode_t mode,
		      struct fuse_file_info *fi)
{
	/*
    int res;

	res = open(path, fi->flags, mode);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
	*/

	struct f_inode* o;
	struct list_node* n;

	if (strlen(path + 1) > MAX_NAMELEN)
		return -ENAMETOOLONG;

	list_for_each (n, &rootDir->file_entries) {
		o = list_entry(n, struct f_inode, node);
		if (strcmp(path + 1, o->name) == 0)
			return -EEXIST;
	}

	o = (struct f_inode *)malloc(sizeof(struct f_inode));
	strcpy(o->name, path + 1); /* skip leading '/' */
	o->mode = mode | S_IFREG | 0644;
	list_add_prev(&o->node, &rootDir->file_entries);

	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	/*
	int res;

	res = open(path, fi->flags);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
	*/
	/*
	(void) fi;
	struct f_inode *o = (struct f_inode *)malloc(sizeof(struct f_inode *));
	if(get_f_inode(path, o) == 1)
		return 0;
	return -ENOENT;
	*/
	return 0;

}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	/*
	int fd;
	int res;

	if(fi == NULL)
		fd = open(path, O_RDONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);

	return res;
	*/
	(void) fi;
	struct f_inode* o;
	struct list_node* n;
	int exist = 0;
	list_for_each (n, &rootDir->file_entries) {
		o = list_entry(n, struct f_inode, node);
		if (strcmp(path + 1, o->name) == 0) {
			exist = 1;
		}
	}
	if(exist == 0)
		return -ENOENT;

	if (offset < o->size) {
		if (offset + size > o->size)
			size = o->size - offset;
		memcpy(buf, o->contents + offset, size);
	} else
		size = 0;

	return size;

}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	/*
	int fd;
	int res;

	(void) fi;
	if(fi == NULL)
		fd = open(path, O_WRONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);
	return res;
	*/

	(void) fi;
	struct f_inode* o;
	struct list_node* n;
	int exist = 0;
	list_for_each (n, &rootDir->file_entries) {
		o = list_entry(n, struct f_inode, node);
		if (strcmp(path + 1, o->name) == 0) {
			exist = 1;
		}
	}
	if(exist == 0)
		return -ENOENT;

	size_t new_size = offset + size;
	if (new_size > o->size) {
		void *new_buf;

		new_buf = realloc(o->contents, new_size);
		if (!new_buf && new_size)
			return -ENOMEM;

		if (new_size > o->size)
			memset(new_buf + o->size, 0, new_size - o->size);

		o->contents = new_buf;
		o->size = new_size;
	}
	memcpy(o->contents + offset, buf, size);

	return size;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	close(fi->fh);
	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void) path;
	(void) isdatasync;
	(void) fi;
	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;

	if (mode)
		return -EOPNOTSUPP;

	if(fi == NULL)
		fd = open(path, O_WRONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = -posix_fallocate(fd, offset, length);

	if(fi == NULL)
		close(fd);
	return res;
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations xmp_oper = {
	.init           = xmp_init,
	.getattr	= xmp_getattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.readdir	= xmp_readdir,
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
#ifdef HAVE_UTIMENSAT
	.utimens	= xmp_utimens,
#endif
	.open		= xmp_open,
	.create 	= xmp_create,
	.read		= xmp_read,
	.write		= xmp_write,
	.statfs		= xmp_statfs,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
};

int main(int argc, char *argv[])
{
	rootDir = (struct d_inode *)malloc(sizeof(struct d_inode));
	list_init(&rootDir->file_entries);
	list_init(&rootDir->dir_entries);
	return fuse_main(argc, argv, &xmp_oper, NULL);
}
