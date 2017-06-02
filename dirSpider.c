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
#include <cspider/spider.h>

#define MAX_NAMELEN 255
typedef unsigned int uint32_t;
struct d_inode {
	char name[MAX_NAMELEN + 1];
	__uid_t uid;		/* User ID of the file's owner.	*/
	__gid_t gid;		/* Group ID of the file's group.*/
	mode_t mode;
	struct list_node file_entries;
	struct list_node dir_entries;
	struct list_node node;
	char *link_path;
};

struct f_inode {
	char name[MAX_NAMELEN + 1];
	void* contents;
	__nlink_t nlink;
	__uid_t uid;		/* User ID of the file's owner.	*/
	__gid_t gid;		/* Group ID of the file's group.*/
	size_t size;
    mode_t mode;
    struct list_node node;
    struct list_node* p_node;
    char *link_path;
};

static struct d_inode *rootDir;

static void *xmp_init(struct fuse_conn_info *conn,
		      struct fuse_config *cfg)
{
	(void) conn;
	cfg->use_ino = 1;
	cfg->kernel_cache = 1;
	cfg->hard_remove = 1;
	cfg->direct_io = 1;

	return NULL;
}

//**********************************************************************************
//Don't forget to free name memory
//**********************************************************************************
static int get_parent_inode(const char *path, struct d_inode **p_node, char **name) {
	char *mpath = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(mpath, path);
	char delim[2] = "/";
	char *last, *next;
	last = strtok(mpath, delim);

	struct d_inode *cur_node = rootDir;
	struct list_node *n;
	while((next = strtok(NULL, delim))) {
		int exist = 0;
		list_for_each (n, &cur_node->dir_entries) {
			struct d_inode* o = list_entry(n, struct d_inode, node);
			if(strcmp(o->name, last) == 0) {
				cur_node = o;
				exist = 1;
				break;
			}
		}
		if(exist == 0) {
			free(mpath);
			return -ENOENT;
		}
		last = next;
	}
	*p_node = cur_node;
	char *target = (char *)malloc((strlen(last)+1)*sizeof(char));
	strcpy(target, last);
	*name = target;
	free(mpath);
	return 0;
}

static int xmp_getattr(const char *path, struct stat *st,
		       struct fuse_file_info *fi)
{
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
	}

	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;
	struct list_node* n;
	list_for_each (n, &ptdir_inode->dir_entries) {
		struct d_inode* d_o = list_entry(n, struct d_inode, node);
		if(strcmp(d_o->name, name) == 0) {
			st->st_mode = d_o->mode;
			st->st_uid = d_o->uid;
			st->st_gid = d_o->gid;
			if((d_o->mode & S_IFLNK) == S_IFLNK) {
				st->st_nlink = 1;
				st->st_size = 1;
				free(name);
				return 0;
			}
			st->st_nlink = 2;
			st->st_size = 0;
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
			free(name);
			return 0;
		}
	}
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode* f_o = list_entry(n, struct f_inode, node);
		if(strcmp(f_o->name, name) == 0) {
			st->st_uid = f_o->uid;
			st->st_gid = f_o->gid;
			if(f_o->p_node != NULL)
				f_o = list_entry(f_o->p_node, struct f_inode, node);
			st->st_mode = f_o->mode;
			if((f_o->mode & S_IFLNK) == S_IFLNK) {
				st->st_nlink = 1;
				st->st_size = 1;
				free(name);
				return 0;
			}
			st->st_nlink = f_o->nlink;
			st->st_size = f_o->size;
			free(name);
			return 0;
		}
	}
	free(name);
	return -ENOENT;
}

static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	struct list_node* n;
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

	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;

	struct d_inode *target_inode;
	list_for_each (n, &ptdir_inode->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if(strcmp(o->name, name) == 0) {
			target_inode = o;
			break;
		}
	}
	if(target_inode == NULL) {
		free(name);
		return -ENOENT;
	}

	list_for_each (n, &target_inode->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		filler(buf, o->name, NULL, 0, 0);
	}
	list_for_each (n, &target_inode->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		filler(buf, o->name, NULL, 0, 0);
	}
	free(name);
	return 0;
}

#define SPIDER_LENGTH 100
static char *spider_title[SPIDER_LENGTH];
static char *spider_url[SPIDER_LENGTH];
static int spider_title_size = 0;
static int spider_url_size = 0;

static char *join_with_base(char *wd, char* pn) {
	char *url_base = "http://www.baidu.com/s";
	char *result = malloc((
			strlen(url_base) +
			strlen("?wd=") +
			strlen(wd) +
			strlen("&pn=") +
			strlen(pn) + 1)*sizeof(char));
	strcpy(result, url_base);
	strcat(result, "?wd=");
	strcat(result, wd);
	strcat(result, "&pn=");
	strcat(result, pn);
	return result;
}

static void process(cspider_t *cspider, char *d, char *url, void *user_data) {
	int i;
	for(i=0; i<spider_url_size; i++) {
		free(spider_url[i]);
	}
	spider_url_size = 0;
	for(i=0; i<spider_title_size; i++) {
		free(spider_title[i]);
	}
	spider_title_size = 0;
	spider_url_size   = xpath(d, "//div[@id='content_left']//h3/a/@href", spider_url, SPIDER_LENGTH);
	spider_title_size = xpath(d, "//div[@id='content_left']//h3/a", spider_title, SPIDER_LENGTH);
}

static void save(void *str, void *user_data) {
	return;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;

	if (strlen(name) > MAX_NAMELEN) {
		free(name);
		return -ENAMETOOLONG;
	}

	struct list_node* n;
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0) {
			free(name);
			return -EEXIST;
		}
	}
	list_for_each (n, &ptdir_inode->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if (strcmp(name, o->name) == 0) {
			free(name);
			return -EEXIST;
		}
	}

	struct d_inode* d_o = (struct d_inode *)malloc(sizeof(struct d_inode));
	strcpy(d_o->name, name);
	free(name);
	d_o->mode = mode | 0755 | S_IFDIR;
	list_init(&d_o->file_entries);
	list_init(&d_o->dir_entries);
	list_add_prev(&d_o->node, &ptdir_inode->dir_entries);


	char *wd  = (char *)malloc((strlen(path)+1)*sizeof(char));
	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[2] = "/";
	char *p = strtok(buf, delim);
	strcpy(wd, p);
	while((p = strtok(NULL, delim))) {
		strcat(wd, "+");
		strcat(wd, p);
	}
	free(buf);

	// Initialize CSpider
	cspider_t *spider = init_cspider();
	char *agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.10; rv:42.0) Gecko/20100101 Firefox/42.0";
	char *pn = "00";
	char *url = join_with_base(wd, pn);
	cs_setopt_url(spider, url);
	cs_setopt_useragent(spider, agent);
	cs_setopt_process(spider, process, NULL);
	cs_setopt_save(spider, save, NULL);
	//cs_setopt_logfile(spider, stdout);
	cs_setopt_threadnum(spider, 5);
	cs_run(spider);
	free(wd);

	struct f_inode *f_o = (struct f_inode *)malloc(sizeof(struct f_inode));
	strcpy(f_o->name, "00");
	f_o->size = 0;
	f_o->mode = S_IFREG | 0644;
	f_o->nlink = 1;

	if(spider_title_size != 0 && spider_title != NULL && spider_title[0]!=NULL && spider_url != NULL && spider_url != NULL) {
		int i=0;
		int content_size = 0;
		for(i=0; i<spider_title_size; i++) {
			content_size += strlen(spider_title[i]) + strlen("\n");
			content_size += strlen(spider_url[i]) + strlen("\n");
		}

		char *contents = (char *)malloc(content_size + 1);
		strcpy(contents, spider_title[0]);
		strcat(contents, "\n");
		strcat(contents, spider_url[0]);
		strcat(contents, "\n");

		for(i=1; i<spider_title_size; i++) {
			strcat(contents, spider_title[i]);
			strcat(contents, "\n");
			strcat(contents, spider_url[i]);
			strcat(contents, "\n");
		}

		f_o->size = strlen(contents);
		f_o->contents = contents;
	}

	list_add_prev(&f_o->node, &d_o->file_entries);

	return 0;
}

static int xmp_unlink(const char *path)
{
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;

	struct list_node *n, *p;
	list_for_each_safe (n, p, &ptdir_inode->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0) {
			__list_del(n);
			if(o->p_node != NULL) {
				struct f_inode* p_o = list_entry(o->p_node, struct f_inode, node);
				p_o->nlink--;
				if(p_o->nlink == 0)
					free(p_o);
				free(o);
			} else {
				o->nlink--;
				if(o->nlink == 0)
					free(o);
			}
			free(name);
			return 0;
		}
	}
	free(name);
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
	free(d_node);
	return;
}

static int xmp_rmdir(const char *path)
{
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;

	struct list_node *n, *p;
	list_for_each_safe (n, p, &ptdir_inode->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if (strcmp(name, o->name) == 0) {
			__list_del(n);
			free_dir_node(o);
			return 0;
		}
	}
	return -ENOENT;
}

static int xmp_create(const char *path, mode_t mode,
		      struct fuse_file_info *fi)
{
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;

	if (strlen(name) > MAX_NAMELEN)
		return -ENAMETOOLONG;

	struct list_node *n;
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0)
			return -EEXIST;
	}

	list_for_each (n, &ptdir_inode->dir_entries) {
		struct d_inode *o = list_entry(n, struct d_inode, node);
		if (strcmp(name, o->name) == 0)
			return -EEXIST;
	}

	struct f_inode *f_o = (struct f_inode *)malloc(sizeof(struct f_inode));
	strcpy(f_o->name, name);
	f_o->size = 0;
	f_o->mode = mode | S_IFREG | 0644;
	f_o->nlink = 1;

	if(ptdir_inode != rootDir) {
		char *wd  = (char *)malloc((strlen(path)+1)*sizeof(char));
		char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
		strcpy(buf, path);
		char delim[2] = "/";
		char *last, *next;
		last= strtok(buf, delim);
		strcpy(wd, last);
		last = strtok(NULL, delim);
		while((next = strtok(NULL, delim))) {
			strcat(wd, "+");
			strcat(wd, last);
			last = next;
		}
		free(buf);
		cspider_t *spider = init_cspider();
		char *agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.10; rv:42.0) Gecko/20100101 Firefox/42.0";
		char *url = join_with_base(wd, name);
		cs_setopt_url(spider, url);
		cs_setopt_useragent(spider, agent);
		cs_setopt_process(spider, process, NULL);
		cs_setopt_save(spider, save, NULL);
		cs_setopt_threadnum(spider, 5);
		cs_run(spider);
		free(wd);
		int i=0;
		int content_size = 0;
		for(i=0; i<spider_title_size; i++) {
			content_size += strlen(spider_title[i]) + strlen("\n");
			content_size += strlen(spider_url[i]) + strlen("\n");
		}
		char *contents = (char *)malloc(content_size + 1);
		strcpy(contents, spider_title[0]);
		strcat(contents, "\n");
		strcat(contents, spider_url[0]);
		strcat(contents, "\n");
		for(i=1; i<spider_title_size; i++) {
			strcat(contents, spider_title[i]);
			strcat(contents, "\n");
			strcat(contents, spider_url[i]);
			strcat(contents, "\n");
		}
		f_o->contents = contents;
		f_o->size = strlen(contents);
	}

	list_add_prev(&f_o->node, &ptdir_inode->file_entries);
	free(name);
	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;

	struct list_node *n;
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0) {
			free(name);
			return 0;
		}
	}
	free(name);
	return -ENOENT;

	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;

	struct list_node *n;
	struct f_inode *target_inode;
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0) {
			target_inode = o;
			break;
		}
	}
	free(name);
	if(target_inode != NULL) {
		if(target_inode->p_node != NULL)
			target_inode = list_entry(target_inode->p_node, struct f_inode, node);
		if (offset < target_inode->size) {
			if (offset + size > target_inode->size)
				size = target_inode->size - offset;
			memcpy(buf, target_inode->contents + offset, size);
		} else
			size = 0;
		return size;
	}
	return -ENOENT;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;

	struct list_node *n;
	struct f_inode *target_inode;
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0) {
			target_inode = o;
			break;
		}
	}
	free(name);
	if(target_inode != NULL) {
		if(target_inode->p_node != NULL)
			target_inode = list_entry(target_inode->p_node, struct f_inode, node);
		size_t new_size = offset + size;
		if (new_size > target_inode->size) {
			void *new_buf;

			new_buf = realloc(target_inode->contents, new_size);
			if (!new_buf && new_size)
				return -ENOMEM;

			if (new_size > target_inode->size)
				memset(new_buf + target_inode->size, 0, new_size - target_inode->size);

			target_inode->contents = new_buf;
			target_inode->size = new_size;
		}
		memcpy(target_inode->contents + offset, buf, size);
		return size;
	}
	return -ENOENT;
}

static int xmp_rename (const char *from, const char *to, unsigned int flags) {
	if (flags)
		return -EINVAL;

	char *fr_name;
	struct d_inode *fr_ptdir_inode;
	if(get_parent_inode(from, &fr_ptdir_inode, &fr_name) || fr_name == NULL || fr_ptdir_inode == NULL)
		return -ENOENT;

	char *to_name;
	struct d_inode *to_ptdir_inode;
	if(get_parent_inode(to, &to_ptdir_inode, &to_name) || to_name == NULL || to_ptdir_inode == NULL) {
		free(fr_name);
		return -ENOENT;
	}

	if (strlen(to_name) > MAX_NAMELEN) {
		free(to_name);
		return -ENAMETOOLONG;
	}

	struct list_node *n;
	list_for_each (n, &to_ptdir_inode->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(to_name, o->name) == 0) {
			free(fr_name);
			free(to_name);
			return -EEXIST;
		}
	}

	list_for_each (n, &to_ptdir_inode->dir_entries) {
		struct d_inode *o = list_entry(n, struct d_inode, node);
		if (strcmp(to_name, o->name) == 0) {
			free(fr_name);
			free(to_name);
			return -EEXIST;
		}
	}

	int filetype = -1;
	struct list_node *target_node;
	list_for_each (n, &fr_ptdir_inode->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if (strcmp(fr_name, o->name) == 0) {
			filetype = 0;
			target_node = n;
			__list_del(n);
			break;
		}
	}
	list_for_each (n, &fr_ptdir_inode->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(fr_name, o->name) == 0) {
			filetype = 1;
			target_node = n;
			__list_del(n);
			break;
		}
	}
	free(fr_name);
	if(target_node == NULL) {
		free(to_name);
		return -ENOENT;
	}

	switch(filetype) {
		case 1: {
			struct f_inode* f_o = list_entry(target_node, struct f_inode, node);
			strcpy(f_o->name, to_name);
			free(to_name);
			list_add_prev(&f_o->node, &to_ptdir_inode->file_entries);
			return 0;
			break;
		}
		case 0: {
			struct d_inode* d_o = list_entry(target_node, struct d_inode, node);
			strcpy(d_o->name, to_name);
			free(to_name);
			list_add_prev(&d_o->node, &to_ptdir_inode->dir_entries);
			return 0;
			break;
		}
		case -1:
			break;
		default:
			break;
	}
	free(to_name);
	return -EINVAL;
}

static int xmp_link (const char *from, const char *to) {
	char *fr_name;
	struct d_inode *fr_ptdir_inode;
	if(get_parent_inode(from, &fr_ptdir_inode, &fr_name) || fr_name == NULL || fr_ptdir_inode == NULL)
		return -ENOENT;

	char *to_name;
	struct d_inode *to_ptdir_inode;
	if(get_parent_inode(to, &to_ptdir_inode, &to_name) || to_name == NULL || to_ptdir_inode == NULL) {
		free(fr_name);
		return -ENOENT;
	}

	if (strlen(to_name) > MAX_NAMELEN) {
		free(to_name);
		return -ENAMETOOLONG;
	}

	struct list_node *n;
	list_for_each (n, &to_ptdir_inode->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(to_name, o->name) == 0) {
			free(fr_name);
			free(to_name);
			return -EEXIST;
		}
	}

	list_for_each (n, &to_ptdir_inode->dir_entries) {
		struct d_inode *o = list_entry(n, struct d_inode, node);
		if (strcmp(to_name, o->name) == 0) {
			free(fr_name);
			free(to_name);
			return -EEXIST;
		}
	}

	int filetype = -1;
	struct list_node *target_node;
	list_for_each (n, &fr_ptdir_inode->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(fr_name, o->name) == 0) {
			filetype = 1;
			target_node = n;
			break;
		}
	}
	free(fr_name);
	if(target_node == NULL) {
		free(to_name);
		return -ENOENT;
	}

	switch(filetype) {
		case 1: {
			struct f_inode* p_f_o = list_entry(target_node, struct f_inode, node);
			struct f_inode *f_o = (struct f_inode *)malloc(sizeof(struct f_inode));
			strcpy(f_o->name, to_name);
			if(p_f_o->p_node != NULL) {
				f_o->p_node = p_f_o->p_node;
				struct f_inode* pp_f_o = list_entry(p_f_o->p_node, struct f_inode, node);
				pp_f_o->nlink++;
			}
			else {
				f_o->p_node = &p_f_o->node;
				p_f_o->nlink++;
			}
			free(to_name);
			list_add_prev(&f_o->node, &to_ptdir_inode->file_entries);
			return 0;
			break;
		}
		case -1:
			break;
		default:
			break;
	}
	free(to_name);
	return -EINVAL;
}

static size_t min(size_t a, size_t b) {
	return a > b ? b : a;
}

static int xmp_readlink (const char *path, char *buf, size_t size) {
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -errno;
	struct list_node *n;
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0) {
			if(o->link_path != NULL) {
				size_t m_size = min(strlen(o->link_path), size-1);
				memcpy(buf, o->link_path, m_size);
				buf[m_size] = '\0';
				return 0;
			}
		}
	}
	list_for_each (n, &ptdir_inode->dir_entries) {
		struct d_inode *o = list_entry(n, struct d_inode, node);
		if (strcmp(name, o->name) == 0) {
			if(o->link_path != NULL) {
				size_t m_size = min(strlen(o->link_path), size-1);
				memcpy(buf, o->link_path, m_size);
				buf[m_size] = '\0';
				return 0;
			}
		}
	}
	return 0;
}

static int xmp_symlink (const char *from, const char *to) {
	char *fr_name;
	struct d_inode *fr_ptdir_inode;
	if(get_parent_inode(from, &fr_ptdir_inode, &fr_name) || fr_name == NULL || fr_ptdir_inode == NULL)
		return -ENOENT;

	char *to_name;
	struct d_inode *to_ptdir_inode;
	if(get_parent_inode(to, &to_ptdir_inode, &to_name) || to_name == NULL || to_ptdir_inode == NULL) {
		free(fr_name);
		return -ENOENT;
	}

	if (strlen(to_name) > MAX_NAMELEN) {
		free(to_name);
		return -ENAMETOOLONG;
	}

	struct list_node *n;
	list_for_each (n, &to_ptdir_inode->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(to_name, o->name) == 0) {
			free(fr_name);
			free(to_name);
			return -EEXIST;
		}
	}

	list_for_each (n, &to_ptdir_inode->dir_entries) {
		struct d_inode *o = list_entry(n, struct d_inode, node);
		if (strcmp(to_name, o->name) == 0) {
			free(fr_name);
			free(to_name);
			return -EEXIST;
		}
	}

	int filetype = -1;
	struct list_node *target_node;
	list_for_each (n, &fr_ptdir_inode->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if (strcmp(fr_name, o->name) == 0) {
			filetype = 0;
			target_node = n;
			break;
		}
	}
	list_for_each (n, &fr_ptdir_inode->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(fr_name, o->name) == 0) {
			filetype = 1;
			target_node = n;
			break;
		}
	}
	free(fr_name);
	if(target_node == NULL) {
		free(to_name);
		return -ENOENT;
	}

	switch(filetype) {
		case 1: {
			struct f_inode *f_o = (struct f_inode *)malloc(sizeof(struct f_inode));
			strcpy(f_o->name, to_name);
			char *f_o_path = (char *)malloc(strlen(from) + 1);
			strcpy(f_o_path, from);
			f_o->link_path = f_o_path;
			f_o->mode = S_IFLNK | 0777;
			f_o->nlink = 1;
			f_o->size = 0;
			free(to_name);
			list_add_prev(&f_o->node, &to_ptdir_inode->file_entries);
			return 0;
			break;
		}
		case 0: {
			struct d_inode *d_o = (struct d_inode *)malloc(sizeof(struct d_inode));
			strcpy(d_o->name, to_name);
			char *d_o_path = (char *)malloc(strlen(from) + 1);
			strcpy(d_o_path, from);
			d_o->link_path = d_o_path;
			d_o->mode = S_IFLNK | 0777;
			free(to_name);
			list_add_prev(&d_o->node, &to_ptdir_inode->dir_entries);
			return 0;
			break;
		}
		case -1:
			break;
		default:
			break;
	}
	free(to_name);
	return -EINVAL;
}


static int xmp_chmod (const char *path, mode_t mode, struct fuse_file_info *fi) {
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;
	struct list_node *n;
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0) {
			o->mode = mode | S_IFREG;
			return 0;
		}
	}

	list_for_each (n, &ptdir_inode->dir_entries) {
		struct d_inode *o = list_entry(n, struct d_inode, node);
		if (strcmp(name, o->name) == 0) {
			o->mode = mode | S_IFDIR;
			return 0;
		}
	}
	return -ENOENT;
}


static int xmp_chown (const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
	char *name;
	struct d_inode *ptdir_inode;
	if(get_parent_inode(path, &ptdir_inode, &name) || name == NULL || ptdir_inode == NULL)
		return -ENOENT;
	struct list_node *n;
	list_for_each (n, &ptdir_inode->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(name, o->name) == 0) {
			o->uid = uid;
			o->gid = gid;
			return 0;
		}
	}

	list_for_each (n, &ptdir_inode->dir_entries) {
		struct d_inode *o = list_entry(n, struct d_inode, node);
		if (strcmp(name, o->name) == 0) {
			o->uid = uid;
			o->gid = gid;
			return 0;
		}
	}
	return -ENOENT;
}

static void xmp_destroy (void * exit) {
	free_dir_node(rootDir);
	int i;
	for(i=0; i<spider_url_size; i++) {
		free(spider_url[i]);
	}
	for(i=0; i<spider_title_size; i++) {
		free(spider_title[i]);
	}
	return;
}

static struct fuse_operations xmp_oper = {
	.init       = xmp_init,
	.getattr	= xmp_getattr,
	.rename     = xmp_rename,
	.readdir	= xmp_readdir,
	.mkdir		= xmp_mkdir,
	.rmdir		= xmp_rmdir,
	.create 	= xmp_create,
	.open       = xmp_open,
	.read		= xmp_read,
	.write		= xmp_write,
	.unlink		= xmp_unlink,
	.link       = xmp_link,
	.symlink    = xmp_symlink,
	.readlink   = xmp_readlink,
	.chmod      = xmp_chmod,
	.chown      = xmp_chown,
	.destroy    = xmp_destroy,
};

int main(int argc, char *argv[])
{
	rootDir = (struct d_inode *)malloc(sizeof(struct d_inode));
	list_init(&rootDir->file_entries);
	list_init(&rootDir->dir_entries);
	return fuse_main(argc, argv, &xmp_oper, NULL);
}
