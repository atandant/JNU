/*
 * kernel/fs/vfs.c — Minimal read-only Virtual File System.
 *
 * Mounts a single root filesystem in v0.0.1. Paths must start with '/'
 * and are resolved by walking the directory tree via the filesystem's
 * lookup operation.
 *
 * Copyright (c) 2026 The JNU Authors.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <jnu/block.h>
#include <jnu/errno.h>
#include <jnu/klog.h>
#include <jnu/minix.h>
#include <jnu/string.h>
#include <jnu/vfs.h>

static struct vfs_mount root_mount;
static bool root_mounted = false;

void vfs_init(void)
{
	/* Minimal init; slab handles inode allocation later if needed */
}

int vfs_mount(const char *bdev_name, const char *fstype, const char *target)
{
	if (strcmp(target, "/") != 0) {
		pr_err("vfs: only '/' mount supported in v0.0.1\n");
		return -ENOSYS;
	}

	if (root_mounted) {
		pr_err("vfs: root already mounted\n");
		return -EBUSY;
	}

	struct block_device *bdev = block_lookup(bdev_name);
	if (!bdev) {
		pr_err("vfs: block device '%s' not found\n", bdev_name);
		return -ENODEV;
	}

	if (strcmp(fstype, "minix") == 0) {
		root_mount.ops = &minix_ops;
	} else {
		pr_err("vfs: unknown fstype '%s'\n", fstype);
		return -ENODEV;
	}

	root_mount.bdev = bdev;
	int err = root_mount.ops->mount(&root_mount, bdev);
	if (err) {
		pr_err("vfs: mount failed: %d\n", err);
		return err;
	}

	root_mounted = true;
	pr_info("vfs: mounted %s on %s type %s\n", bdev_name, target, fstype);
	return 0;
}

static const char *skip_slash(const char *p)
{
	while (*p == '/')
		p++;
	return p;
}

static int next_component(const char *path, char *comp, size_t max_len, const char **next_path)
{
	path = skip_slash(path);
	if (!*path) {
		*next_path = NULL;
		return 0;
	}

	size_t i = 0;
	while (path[i] && path[i] != '/') {
		if (i >= max_len - 1)
			return -ENAMETOOLONG;
		comp[i] = path[i];
		i++;
	}
	comp[i] = '\0';
	*next_path = path + i;
	return 1;
}

int vfs_open(const char *path, struct vfs_inode **out)
{
	if (!root_mounted || !root_mount.root)
		return -ENOENT;

	if (path[0] != '/')
		return -EINVAL;

	struct vfs_inode *curr = NULL;
	char comp[VFS_NAME_MAX];
	const char *p = path;

	/* Clone root to start the walk */
	int err = root_mount.ops->lookup(root_mount.root, ".", &curr);
	if (err)
		return err;

	while (1) {
		int res = next_component(p, comp, sizeof(comp), &p);
		if (res < 0) {
			curr->mnt->ops->close(curr);
			return res;
		}
		if (res == 0)
			break;

		if (!curr->is_dir) {
			curr->mnt->ops->close(curr);
			return -ENOTDIR;
		}
		
		if (!(curr->mode & 0111)) {
			curr->mnt->ops->close(curr);
			return -EACCES;
		}
		
		if (strcmp(comp, "..") == 0 && curr->ino == root_mount.root->ino) {
			continue;
		}

		struct vfs_inode *next = NULL;
		err = curr->mnt->ops->lookup(curr, comp, &next);
		curr->mnt->ops->close(curr);
		
		if (err)
			return err;

		curr = next;
	}

	*out = curr;
	return 0;
}

ssize_t vfs_read(struct vfs_inode *ino, uint64_t offset, size_t len, void *buf)
{
	if (!ino || !ino->mnt || !ino->mnt->ops || !ino->mnt->ops->read)
		return -EINVAL;
	if (ino->is_dir)
		return -EISDIR;
	if (!(ino->mode & 0444))
		return -EACCES;
	return ino->mnt->ops->read(ino, offset, len, buf);
}

int vfs_readdir(struct vfs_inode *dir, size_t index, struct vfs_dirent *out)
{
	if (!dir || !dir->mnt || !dir->mnt->ops || !dir->mnt->ops->readdir)
		return -EINVAL;
	if (!dir->is_dir)
		return -ENOTDIR;
	return dir->mnt->ops->readdir(dir, index, out);
}

void vfs_close(struct vfs_inode *ino)
{
	if (ino && ino->mnt && ino->mnt->ops && ino->mnt->ops->close)
		ino->mnt->ops->close(ino);
}

int vfs_selftest(void)
{
	if (!root_mounted) {
		pr_warn("vfs_selftest: skipped, no root mount\n");
		return 0;
	}

	struct vfs_inode *ino;
	int err = vfs_open("/test.txt", &ino);
	if (err) {
		pr_err("vfs_selftest: could not open /test.txt (err=%d)\n", err);
		return err;
	}
	
	if (ino->is_dir) {
		pr_err("vfs_selftest: /test.txt is a directory\n");
		vfs_close(ino);
		return -EISDIR;
	}
	
	char buf[32];
	memset(buf, 0, sizeof(buf));
	ssize_t n = vfs_read(ino, 0, sizeof(buf) - 1, buf);
	vfs_close(ino);
	
	if (n < 0) {
		pr_err("vfs_selftest: read failed (err=%d)\n", (int)n);
		return (int)n;
	}
	
	if (strncmp(buf, "Hello from JNU", 14) != 0) {
		pr_err("vfs_selftest: unexpected content: '%s'\n", buf);
		return -EIO;
	}
	
	pr_info("vfs_selftest: read test.txt [ OK ]\n");
	return 0;
}
