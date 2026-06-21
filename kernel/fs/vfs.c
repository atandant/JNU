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

#include <jnu/fs/block.h>
#include <jnu/fs/fat32.h>
#include <jnu/fs/minix.h>
#include <jnu/fs/vfs.h>
#include <jnu/lib/klog.h>
#include <jnu/lib/mutex.h>
#include <jnu/lib/string.h>
#include <uapi/jnu/errno.h>

/*
 * Mount registry. Entry 0 is always the root mount ("/"); the rest are
 * submounts.
 *
 * This is Design B (mountpoint pinning): a submount is pinned to the
 * (covered_mnt, covered_ino) pair of the directory it is mounted on.
 * Every vfs_inode carries a stable identity in (ino->mnt, ino->ino), so
 * a freshly looked-up directory can be recognized as a mountpoint by a
 * small scan of this table.
 *
 * VFS_MOUNT_MAX is a fixed cap. Raising it (or moving to a dynamic
 * list) waits for a future update; eight live mounts is plenty for now.
 */
#define VFS_MOUNT_MAX 8

/*
 * Inode cache (live-inode table).
 *
 * Guarantees a single canonical in-memory vfs_inode per (mnt, ino)
 * while that inode is referenced. This is a correctness mechanism, not
 * a speed optimization: without it, opening the same file twice yields
 * two independent vfs_inode objects, each with its own mutex and its
 * own copy of the on-disk metadata, so concurrent writers serialize on
 * different locks and the last close clobbers the other's size and
 * block pointers (lost update). Sharing one object behind one mutex
 * fixes that.
 *
 * Lifetime: an inode enters the cache with refcount 1 the first time it
 * is canonicalized, gains a reference on every further canonicalize of
 * the same (mnt, ino), and is removed + written back + freed when the
 * last reference is dropped. Idle (refcount 0) inodes are NOT retained;
 * LRU retention of clean inodes for hit-rate is a deliberate future
 * step and is unnecessary for the correctness goal.
 *
 * Filesystem code remains cache-agnostic: ops->lookup / ops->create
 * still build a plain inode, and vfs.c folds it into the cache via
 * vfs_icache_canonicalize().
 *
 * Lock ordering: icache_lock is a true leaf. It is never held while
 * acquiring any other lock, and is never acquired while holding a
 * vfs_inode mutex or the buffer-cache lock — the ops->close writeback
 * that may touch the buffer cache always runs after icache_lock is
 * dropped (canonicalize/iput unlink under the lock, then close):
 *   bufcache_lock < pmm_lock < vfs_inode_lock, and icache_lock alone.
 */
#define VFS_ICACHE_BUCKETS 64

static struct vfs_inode *icache[VFS_ICACHE_BUCKETS];
static struct mutex icache_lock = MUTEX_INITIALIZER;

static size_t icache_hash(const struct vfs_mount *mnt, uint32_t ino)
{
	uintptr_t k = (uintptr_t)mnt;

	k = (k >> 4) ^ (k >> 12) ^ (uintptr_t)(ino * 2654435761u);
	return (size_t)(k % VFS_ICACHE_BUCKETS);
}

/*
 * Fold a freshly built inode into the cache.
 *
 * On entry *pio is an owned, not-yet-cached inode (refcount 0) returned
 * by ops->lookup / ops->create / ops->mount. If another inode for the
 * same (mnt, ino) is already cached, the duplicate is freed and *pio is
 * replaced with the canonical object (its refcount bumped). Otherwise
 * *pio is inserted with refcount 1. Either way, on return *pio is the
 * canonical inode and the caller owns exactly one reference to it.
 */
static void vfs_icache_canonicalize(struct vfs_inode **pio)
{
	struct vfs_inode *cand = *pio;
	size_t h = icache_hash(cand->mnt, cand->ino);
	struct vfs_inode *p;

	mutex_lock(&icache_lock);
	for (p = icache[h]; p; p = p->cache_next) {
		if (p->mnt == cand->mnt && p->ino == cand->ino) {
			p->refcount++;
			mutex_unlock(&icache_lock);
			/* Drop the redundant copy (clean: no writeback). */
			cand->mnt->ops->close(cand);
			*pio = p;
			return;
		}
	}
	cand->refcount = 1;
	cand->cache_next = icache[h];
	icache[h] = cand;
	mutex_unlock(&icache_lock);
}

/*
 * Drop one reference to an inode.
 *
 * Cached inodes (refcount >= 1) are decremented and, on the 1->0
 * transition, unlinked from the cache and handed to ops->close for
 * writeback + free. Inodes that were never canonicalized (refcount 0 —
 * the short-lived temporaries some filesystem operations build) are
 * freed directly. This is the single teardown path behind vfs_close().
 */
static void vfs_iput(struct vfs_inode *ino)
{
	struct vfs_inode *victim = NULL;
	size_t h;

	if (!ino)
		return;

	h = icache_hash(ino->mnt, ino->ino);
	mutex_lock(&icache_lock);
	if (ino->refcount == 0) {
		/* Never cached: a transient temporary. Free it directly. */
		mutex_unlock(&icache_lock);
		ino->mnt->ops->close(ino);
		return;
	}
	if (--ino->refcount == 0) {
		struct vfs_inode **pp = &icache[h];

		while (*pp && *pp != ino)
			pp = &(*pp)->cache_next;
		if (*pp)
			*pp = ino->cache_next;
		ino->cache_next = NULL;
		victim = ino;
	}
	mutex_unlock(&icache_lock);

	if (victim)
		victim->mnt->ops->close(victim);
}

/*
 * Upper bound for a path after lexical normalization. Matches the
 * userspace JNU_PATH_MAX (256); kept local so vfs.c does not depend on
 * the syscall headers.
 */
#define VFS_PATH_MAX 256

struct vfs_mountpoint {
	bool used;
	struct vfs_mount
	    *covered_mnt;     /* parent fs holding the dir, NULL=root */
	uint32_t covered_ino; /* inode of the covered directory */
	struct vfs_mount mnt; /* the mounted filesystem */
};

static struct vfs_mountpoint mounts[VFS_MOUNT_MAX];
static bool root_mounted = false;

/* The root mount lives in slot 0. */
#define root_mount (mounts[0].mnt)

static int next_component(const char *path, char *comp, size_t max_len,
			  const char **next_path);

void vfs_init(void)
{
	/* Minimal init; slab handles inode allocation later if needed */
}

static const struct vfs_ops *ops_for_fstype(const char *fstype)
{
	if (strcmp(fstype, "minix") == 0)
		return &minix_ops;
	if (strcmp(fstype, "fat32") == 0)
		return &fat32_ops;
	return NULL;
}

int vfs_mount(const char *bdev_name, const char *fstype, const char *target)
{
	const struct vfs_ops *ops;
	struct block_device *bdev;
	int err;

	ops = ops_for_fstype(fstype);
	if (!ops) {
		pr_err("vfs: unknown fstype '%s'\n", fstype);
		return -ENODEV;
	}

	bdev = block_lookup(bdev_name);
	if (!bdev) {
		pr_err("vfs: block device '%s' not found\n", bdev_name);
		return -ENODEV;
	}

	/* Root mount: target must be "/", and only once. */
	if (strcmp(target, "/") == 0) {
		if (root_mounted) {
			pr_err("vfs: root already mounted\n");
			return -EBUSY;
		}
		mounts[0].covered_mnt = NULL;
		mounts[0].covered_ino = 0;
		root_mount.bdev = bdev;
		root_mount.ops = ops;
		err = ops->mount(&root_mount, bdev);
		if (err) {
			pr_err("vfs: mount failed: %d\n", err);
			return err;
		}
		/* Pin the root inode as the canonical cache entry. */
		vfs_icache_canonicalize(&root_mount.root);
		mounts[0].used = true;
		root_mounted = true;
		pr_info("vfs: mounted %s on %s type %s\n", bdev_name, target,
			fstype);
		return 0;
	}

	/* Submount: the root fs must exist so the target dir can be found. */
	if (!root_mounted)
		return -ENOENT;

	/* Resolve the target directory → its (covered_mnt, covered_ino). */
	struct vfs_inode *tdir = NULL;
	err = vfs_open(target, &tdir);
	if (err)
		return err;
	if (!tdir->is_dir) {
		vfs_close(tdir);
		return -ENOTDIR;
	}
	struct vfs_mount *covered_mnt = tdir->mnt;
	uint32_t covered_ino = tdir->ino;
	vfs_close(tdir);

	/* Reject a second mount on a directory that is already a mountpoint. */
	size_t slot = 0;
	for (size_t i = 1; i < VFS_MOUNT_MAX; i++) {
		if (mounts[i].used && mounts[i].covered_mnt == covered_mnt &&
		    mounts[i].covered_ino == covered_ino)
			return -EBUSY;
		if (!mounts[i].used && slot == 0)
			slot = i;
	}
	if (slot == 0) {
		pr_err("vfs: mount table full (max %d); raising the cap waits "
		       "for a future update\n",
		       VFS_MOUNT_MAX);
		return -ENOSPC;
	}

	mounts[slot].mnt.bdev = bdev;
	mounts[slot].mnt.ops = ops;
	err = ops->mount(&mounts[slot].mnt, bdev);
	if (err) {
		pr_err("vfs: mount failed: %d\n", err);
		return err;
	}
	/* Pin the submount's root inode as its canonical cache entry. */
	vfs_icache_canonicalize(&mounts[slot].mnt.root);
	mounts[slot].covered_mnt = covered_mnt;
	mounts[slot].covered_ino = covered_ino;
	mounts[slot].used = true;
	pr_info("vfs: mounted %s on %s type %s\n", bdev_name, target, fstype);
	return 0;
}

/*
 * If *inode is a directory that something is mounted on, replace it with
 * a fresh clone of the submount's root inode. Loops so that stacked
 * mounts (a mount whose root is itself a mountpoint) resolve fully.
 *
 * On success *inode is a live, owned inode (possibly the original). On
 * error the original inode is closed and *inode is set to NULL.
 */
static int cross_mounts(struct vfs_inode **inode)
{
	struct vfs_inode *cur = *inode;

	for (;;) {
		struct vfs_mount *sub = NULL;

		for (size_t i = 1; i < VFS_MOUNT_MAX; i++) {
			if (mounts[i].used &&
			    mounts[i].covered_mnt == cur->mnt &&
			    mounts[i].covered_ino == cur->ino) {
				sub = &mounts[i].mnt;
				break;
			}
		}
		if (!sub)
			break;

		struct vfs_inode *subroot = NULL;
		int err = sub->ops->lookup(sub->root, ".", &subroot);
		if (!err)
			vfs_icache_canonicalize(&subroot);
		vfs_iput(cur);
		if (err) {
			*inode = NULL;
			return err;
		}
		cur = subroot;
	}

	*inode = cur;
	return 0;
}

/*
 * Collapse "." and ".." components of an absolute path lexically into
 * `out` (which must hold at least VFS_PATH_MAX bytes). Requires an
 * absolute path. The result always begins with '/', has no "."/".."
 * components, and the parent of "/" is "/".
 *
 * TODO(symlinks): lexical ".." collapsing is only correct while JNU has
 * no symbolic links. Once symlinks land this must become per-component
 * resolution that consults the filesystem at each step, because ".."
 * after a symlink does not commute with lexical removal.
 */
static int normalize_path(const char *path, char *out)
{
	char comp[VFS_NAME_MAX];
	size_t len;
	int res;

	if (path[0] != '/')
		return -EINVAL;

	out[0] = '/';
	len = 1;

	while ((res = next_component(path, comp, sizeof(comp), &path)) != 0) {
		if (res < 0)
			return res;

		if (strcmp(comp, ".") == 0)
			continue;

		if (strcmp(comp, "..") == 0) {
			/* Pop the last component; parent of "/" is "/". */
			while (len > 1 && out[len - 1] != '/')
				len--;
			if (len > 1)
				len--; /* drop the separating slash */
			continue;
		}

		/* Append "/comp" (no leading slash when out is still "/"). */
		size_t clen = strlen(comp);
		size_t need = (len == 1 ? 0 : 1) + clen;

		if (len + need + 1 > VFS_PATH_MAX)
			return -ENAMETOOLONG;
		if (len != 1)
			out[len++] = '/';
		memcpy(out + len, comp, clen);
		len += clen;
	}

	out[len] = '\0';
	return 0;
}

static const char *skip_slash(const char *p)
{
	while (*p == '/')
		p++;
	return p;
}

static int next_component(const char *path, char *comp, size_t max_len,
			  const char **next_path)
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

static bool path_has_more(const char *path)
{
	path = skip_slash(path);
	return *path != '\0';
}

static int vfs_parent(const char *path, struct vfs_inode **dir, char *name,
		      size_t name_len)
{
	struct vfs_inode *curr;
	char norm[VFS_PATH_MAX];
	char comp[VFS_NAME_MAX];
	const char *p;
	int err;

	if (!root_mounted || !root_mount.root)
		return -ENOENT;

	/* Strip "." / ".." up front so the walk only sees real names. */
	err = normalize_path(path, norm);
	if (err)
		return err;
	p = norm;

	err = root_mount.ops->lookup(root_mount.root, ".", &curr);
	if (err)
		return err;
	vfs_icache_canonicalize(&curr);
	err = cross_mounts(&curr);
	if (err)
		return err;

	while (1) {
		int res = next_component(p, comp, sizeof(comp), &p);

		if (res < 0) {
			vfs_iput(curr);
			return res;
		}
		if (res == 0) {
			vfs_iput(curr);
			return -EINVAL;
		}
		if (!path_has_more(p)) {
			if (strlen(comp) >= name_len) {
				vfs_iput(curr);
				return -ENAMETOOLONG;
			}
			memcpy(name, comp, strlen(comp) + 1);
			*dir = curr;
			return 0;
		}

		if (!curr->is_dir) {
			vfs_iput(curr);
			return -ENOTDIR;
		}

		{
			struct vfs_inode *next = NULL;

			err = curr->mnt->ops->lookup(curr, comp, &next);
			if (!err)
				vfs_icache_canonicalize(&next);
			vfs_iput(curr);
			if (err)
				return err;
			curr = next;
			err = cross_mounts(&curr);
			if (err)
				return err;
		}
	}
}

int vfs_open(const char *path, struct vfs_inode **out)
{
	char norm[VFS_PATH_MAX];
	char comp[VFS_NAME_MAX];
	const char *p;
	int err;

	if (!root_mounted || !root_mount.root)
		return -ENOENT;

	/* Strip "." / ".." up front so the walk only sees real names. */
	err = normalize_path(path, norm);
	if (err)
		return err;
	p = norm;

	struct vfs_inode *curr = NULL;

	/* Clone root to start the walk, then honor any mount at the start. */
	err = root_mount.ops->lookup(root_mount.root, ".", &curr);
	if (err)
		return err;
	vfs_icache_canonicalize(&curr);
	err = cross_mounts(&curr);
	if (err)
		return err;

	while (1) {
		int res = next_component(p, comp, sizeof(comp), &p);
		if (res < 0) {
			vfs_iput(curr);
			return res;
		}
		if (res == 0)
			break;

		if (!curr->is_dir) {
			vfs_iput(curr);
			return -ENOTDIR;
		}

		if (!(curr->mode & 0111)) {
			vfs_iput(curr);
			return -EACCES;
		}

		struct vfs_inode *next = NULL;
		err = curr->mnt->ops->lookup(curr, comp, &next);
		if (!err)
			vfs_icache_canonicalize(&next);
		vfs_iput(curr);

		if (err)
			return err;

		curr = next;
		err = cross_mounts(&curr);
		if (err)
			return err;
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
	ssize_t ret;
	if (!(ino->mode & 0444))
		return -EACCES;
	mutex_lock(&ino->lock);
	ret = ino->mnt->ops->read(ino, offset, len, buf);
	mutex_unlock(&ino->lock);
	return ret;
}

ssize_t vfs_write(struct vfs_inode *ino, uint64_t offset, size_t len,
		  const void *buf)
{
	if (!ino || !ino->mnt || !ino->mnt->ops || !ino->mnt->ops->write)
		return -EINVAL;
	if (ino->is_dir)
		return -EISDIR;
	ssize_t ret;
	if (!(ino->mode & 0222))
		return -EACCES;
	mutex_lock(&ino->lock);
	ret = ino->mnt->ops->write(ino, offset, len, buf);
	mutex_unlock(&ino->lock);
	return ret;
}

int vfs_truncate(struct vfs_inode *ino, uint64_t size)
{
	if (!ino || !ino->mnt || !ino->mnt->ops || !ino->mnt->ops->truncate)
		return -EINVAL;
	int ret;
	if (ino->is_dir)
		return -EISDIR;
	mutex_lock(&ino->lock);
	ret = ino->mnt->ops->truncate(ino, size);
	mutex_unlock(&ino->lock);
	return ret;
}

int vfs_create(const char *path, uint16_t mode, struct vfs_inode **out)
{
	struct vfs_inode *dir;
	char name[VFS_NAME_MAX];
	int err;

	err = vfs_parent(path, &dir, name, sizeof(name));
	if (err)
		return err;
	if (!dir->mnt->ops->create) {
		vfs_close(dir);
		return -ENOSYS;
	}
	mutex_lock(&dir->lock);
	err = dir->mnt->ops->create(dir, name, mode, out);
	mutex_unlock(&dir->lock);
	if (!err)
		vfs_icache_canonicalize(out);
	vfs_close(dir);
	return err;
}

int vfs_unlink(const char *path)
{
	struct vfs_inode *dir;
	char name[VFS_NAME_MAX];
	int err;

	err = vfs_parent(path, &dir, name, sizeof(name));
	if (err)
		return err;
	mutex_lock(&dir->lock);
	err =
	    dir->mnt->ops->unlink ? dir->mnt->ops->unlink(dir, name) : -ENOSYS;
	mutex_unlock(&dir->lock);
	vfs_close(dir);
	return err;
}

int vfs_mkdir(const char *path, uint16_t mode)
{
	struct vfs_inode *dir;
	char name[VFS_NAME_MAX];
	int err;

	err = vfs_parent(path, &dir, name, sizeof(name));
	if (err)
		return err;
	mutex_lock(&dir->lock);
	err = dir->mnt->ops->mkdir ? dir->mnt->ops->mkdir(dir, name, mode)
				   : -ENOSYS;
	mutex_unlock(&dir->lock);
	vfs_close(dir);
	return err;
}

int vfs_rmdir(const char *path)
{
	struct vfs_inode *dir;
	char name[VFS_NAME_MAX];
	int err;

	err = vfs_parent(path, &dir, name, sizeof(name));
	if (err)
		return err;
	mutex_lock(&dir->lock);
	err = dir->mnt->ops->rmdir ? dir->mnt->ops->rmdir(dir, name) : -ENOSYS;
	mutex_unlock(&dir->lock);
	vfs_close(dir);
	return err;
}

int vfs_rename(const char *old_path, const char *new_path)
{
	struct vfs_inode *old_dir;
	struct vfs_inode *new_dir;
	char old_name[VFS_NAME_MAX];
	char new_name[VFS_NAME_MAX];
	int err;

	err = vfs_parent(old_path, &old_dir, old_name, sizeof(old_name));
	if (err)
		return err;
	err = vfs_parent(new_path, &new_dir, new_name, sizeof(new_name));
	if (err) {
		vfs_close(old_dir);
		return err;
	}
	if (old_dir != new_dir) {
		if (old_dir < new_dir) {
			mutex_lock(&old_dir->lock);
			mutex_lock(&new_dir->lock);
		} else {
			mutex_lock(&new_dir->lock);
			mutex_lock(&old_dir->lock);
		}
	} else {
		mutex_lock(&old_dir->lock);
	}

	if (old_dir->mnt != new_dir->mnt) {
		err = -EXDEV;
	} else if (!old_dir->mnt->ops->rename) {
		err = -ENOSYS;
	} else {
		err = old_dir->mnt->ops->rename(old_dir, old_name, new_dir,
						new_name);
	}

	if (old_dir != new_dir) {
		mutex_unlock(&new_dir->lock);
		mutex_unlock(&old_dir->lock);
	} else {
		mutex_unlock(&old_dir->lock);
	}
	vfs_close(new_dir);
	vfs_close(old_dir);
	return err;
}

int vfs_fsync(struct vfs_inode *ino)
{
	int ret;
	if (!ino || !ino->mnt || !ino->mnt->ops || !ino->mnt->ops->fsync)
		return -EINVAL;
	mutex_lock(&ino->lock);
	ret = ino->mnt->ops->fsync(ino);
	mutex_unlock(&ino->lock);
	return ret;
}

int vfs_readdir(struct vfs_inode *dir, size_t index, struct vfs_dirent *out)
{
	int ret;
	if (!dir || !dir->mnt || !dir->mnt->ops || !dir->mnt->ops->readdir)
		return -EINVAL;
	if (!dir->is_dir)
		return -ENOTDIR;
	mutex_lock(&dir->lock);
	ret = dir->mnt->ops->readdir(dir, index, out);
	mutex_unlock(&dir->lock);
	return ret;
}

void vfs_close(struct vfs_inode *ino)
{
	/*
	 * All teardown goes through the inode cache: vfs_iput drops a
	 * reference and only writes back + frees the canonical object on
	 * the last put. Inodes that were never cached (transient
	 * filesystem temporaries, refcount 0) are freed immediately.
	 */
	if (ino && ino->mnt && ino->mnt->ops && ino->mnt->ops->close)
		vfs_iput(ino);
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
		pr_err("vfs_selftest: could not open /test.txt (err=%d)\n",
		       err);
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
