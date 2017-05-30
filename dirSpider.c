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

	return NULL;
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

	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[2] = "/ ";
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
		if(exist == 0) {
			free(buf);
			return -ENOENT;
		}
		p = next;
	}

	list_for_each (n, &cur_node->dir_entries) {
		struct d_inode* d_o = list_entry(n, struct d_inode, node);
		if(strcmp(d_o->name, p) == 0) {
			st->st_mode = d_o->mode;
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
			free(buf);
			return 0;
		}
	}

	list_for_each (n, &cur_node->file_entries) {
		struct f_inode* f_o = list_entry(n, struct f_inode, node);
		if(strcmp(f_o->name, p) == 0) {
			st->st_mode = f_o->mode;
			st->st_nlink = 1;
			st->st_size = f_o->size;
			free(buf);
			return 0;
		}
	}
	free(buf);
	return -ENOENT;   //输入输出错误
}

static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
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

	char *mpath = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(mpath, path);
	char delim[2] = "/";
	char *p;
	p = strtok(mpath, delim);
	struct d_inode *cur_node = rootDir;
	while(p) {
		int exist = 0;
		list_for_each (n, &cur_node->dir_entries) {
			struct d_inode* o = list_entry(n, struct d_inode, node);
			if(strcmp(o->name, p) == 0) {
				cur_node = o;
				exist = 1;
				break;
			}
		}
		if(exist == 0) {
			free(mpath);
			return -ENOENT;
		}
		p = strtok(NULL, delim);
	}

	list_for_each (n, &cur_node->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		filler(buf, o->name, NULL, 0, 0);
	}
	list_for_each (n, &cur_node->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		filler(buf, o->name, NULL, 0, 0);
	}
	free(mpath);

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

void process(cspider_t *cspider, char *d, char *url, void *user_data) {
	int i;
	for(i=0; i<spider_url_size; i++) {
		free(spider_url[i]);
	}
	spider_url_size = 0;
	for(i=0; i<spider_url_size; i++) {
		free(spider_title[i]);
	}
	spider_title_size = 0;
	spider_url_size   = xpath(d, "//div[@id='content_left']//h3/a/@href", spider_url, SPIDER_LENGTH);
	spider_title_size = xpath(d, "//div[@id='content_left']//h3/a", spider_title, SPIDER_LENGTH);
}

void save(void *str, void *user_data) {
	return;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	char *wd = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[2] = "/";
	char *p, *next;
	p = strtok(buf, delim);

	struct d_inode *cur_node = rootDir;
	struct list_node* n;
	strcpy(wd, p);
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
		if(exist == 0) {
			free(buf);
			return -ENOENT;
		}
		p = next;
		strcat(wd, "+");
		strcat(wd, p);
	}

	if (strlen(p) > MAX_NAMELEN) {
		free(buf);
		return -ENAMETOOLONG;
	}

	list_for_each (n, &cur_node->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(p, o->name) == 0) {
			free(buf);
			return -EEXIST;
		}
	}

	list_for_each (n, &cur_node->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if (strcmp(p, o->name) == 0) {
			free(buf);
			return -EEXIST;
		}
	}

	struct d_inode* d_o = (struct d_inode *)malloc(sizeof(struct d_inode));
	strcpy(d_o->name, p); /* skip leading '/' */
	d_o->mode = mode | 0755 | S_IFDIR;
	d_o->count = 0;
	list_init(&d_o->file_entries);
	list_init(&d_o->dir_entries);
	list_add_prev(&d_o->node, &cur_node->dir_entries);

	// Initialize CSpider
	cspider_t *spider = init_cspider();
	char *agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.10; rv:42.0) Gecko/20100101 Firefox/42.0";
	char *pn = "00";
	char *url = join_with_base(wd, pn);
	cs_setopt_url(spider, url);
	cs_setopt_useragent(spider, agent);
	cs_setopt_process(spider, process, NULL);
	cs_setopt_save(spider, save, NULL);
	cs_setopt_threadnum(spider, 5);
	cs_run(spider);
	struct f_inode *f_o = (struct f_inode *)malloc(sizeof(struct f_inode));
	strcpy(f_o->name, "00"); /* skip leading '/' */
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
	f_o->mode = S_IFREG | 0644;
	f_o->ref = 1;
	list_add_prev(&f_o->node, &d_o->file_entries);

	free(buf);
	return 0;
}

static int xmp_unlink(const char *path)
{
	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[2] = "/";
	char *last, *next;
	last = strtok(buf, delim);

	struct d_inode *cur_node = rootDir;
	struct list_node *n, *p;
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
			free(buf);
			return -ENOENT;
		}
		last = next;
	}

	list_for_each_safe (n, p, &cur_node->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(last, o->name) == 0) {
			__list_del(n);
			free(o);
			free(buf);
			return 0;
		}
	}
	free(buf);
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
	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[2] = "/";
	char *last, *next;
	last = strtok(buf, delim);

	struct d_inode *cur_node = rootDir;
	struct list_node *n, *p;
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
			free(buf);
			return -ENOENT;
		}
		last = next;
	}

	list_for_each_safe (n, p, &cur_node->dir_entries) {
		struct d_inode* o = list_entry(n, struct d_inode, node);
		if (strcmp(last, o->name) == 0) {
			__list_del(n);
			free_dir_node(o);
			return 0;
		}
	}
	return 0;
}

static int xmp_create(const char *path, mode_t mode,
		      struct fuse_file_info *fi)
{
	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	char *wd = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[2] = "/";
	char *last, *next;
	last = strtok(buf, delim);
	struct d_inode *cur_node = rootDir;
	struct list_node *n;
	int init = 0;
	while((next = strtok(NULL, delim))) {
		if(init == 0) {
			strcpy(wd, last);
			init = 1;
		} else {
			strcat(wd, "+");
			strcat(wd, last);
		}
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
			free(buf);
			return -ENOENT;
		}
		last = next;
	}

	if (strlen(last) > MAX_NAMELEN)
		return -ENAMETOOLONG;

	list_for_each (n, &cur_node->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(last, o->name) == 0)
			return -EEXIST;
	}

	list_for_each (n, &cur_node->dir_entries) {
		struct d_inode *o = list_entry(n, struct d_inode, node);
		if (strcmp(last, o->name) == 0)
			return -EEXIST;
	}

	struct f_inode *o = (struct f_inode *)malloc(sizeof(struct f_inode));
	strcpy(o->name, last);
	o->size = 0;
	o->ref = 1;
	o->mode = mode | S_IFREG | 0644;
	if(init == 1) {
		cspider_t *spider = init_cspider();
		char *agent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.10; rv:42.0) Gecko/20100101 Firefox/42.0";
		char *url = join_with_base(wd, last);
		cs_setopt_url(spider, url);
		cs_setopt_useragent(spider, agent);
		cs_setopt_process(spider, process, NULL);
		cs_setopt_save(spider, save, NULL);
		cs_setopt_threadnum(spider, 5);
		cs_run(spider);
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
		o->contents = contents;
		o->size = strlen(contents);
	}
	list_add_prev(&o->node, &cur_node->file_entries);

	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	char *buf = (char *)malloc((strlen(path)+1)*sizeof(char));
	strcpy(buf, path);
	char delim[2] = "/";
	char *last, *next;
	last = strtok(buf, delim);

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
			free(buf);
			return -ENOENT;
		}
		last = next;
	}
	int exist = 0;
	list_for_each (n, &cur_node->file_entries) {
		struct f_inode *o = list_entry(n, struct f_inode, node);
		if (strcmp(last, o->name) == 0) {
			exist = 1;
			break;
		}
	}
	if(exist == 0)
		return -ENOENT;
	return 0;

}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
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

	int exist = 0;
	struct f_inode *target_node;
	list_for_each (n, &cur_node->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(last, o->name) == 0) {
			target_node = o;
			exist = 1;
			break;
		}
	}
	if(exist == 0) {
		free(mpath);
		return -ENOENT;
	}

	if (offset < target_node->size) {
		if (offset + size > target_node->size)
			size = target_node->size - offset;
		memcpy(buf, target_node->contents + offset, size);
	} else
		size = 0;
	return size;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
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

	int exist = 0;
	struct f_inode *target_node;
	list_for_each (n, &cur_node->file_entries) {
		struct f_inode* o = list_entry(n, struct f_inode, node);
		if (strcmp(last, o->name) == 0) {
			target_node = o;
			exist = 1;
			break;
		}
	}
	if(exist == 0) {
		free(mpath);
		return -ENOENT;
	}

	size_t new_size = offset + size;
	if (new_size > target_node->size) {
		void *new_buf;

		new_buf = realloc(target_node->contents, new_size);
		if (!new_buf && new_size)
			return -ENOMEM;

		if (new_size > target_node->size)
			memset(new_buf + target_node->size, 0, new_size - target_node->size);

		target_node->contents = new_buf;
		target_node->size = new_size;
	}
	memcpy(target_node->contents + offset, buf, size);

	return size;
}

static struct fuse_operations xmp_oper = {
	.init       = xmp_init,
	.getattr	= xmp_getattr,
	.readdir	= xmp_readdir,
	.mkdir		= xmp_mkdir,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.create 	= xmp_create,
	.open       = xmp_open,
	.read		= xmp_read,
	.write		= xmp_write,
};

int main(int argc, char *argv[])
{
	rootDir = (struct d_inode *)malloc(sizeof(struct d_inode));
	list_init(&rootDir->file_entries);
	list_init(&rootDir->dir_entries);
	return fuse_main(argc, argv, &xmp_oper, NULL);
}
