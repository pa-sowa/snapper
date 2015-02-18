/*
 * Copyright (c) [2011-2015] Novell, Inc.
 *
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, contact Novell, Inc.
 *
 * To contact Novell about this file by physical or electronic mail, you may
 * find current contact information at www.novell.com.
 */


#include "config.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <asm/types.h>
#ifdef ENABLE_ROLLBACK
#include <libmount/libmount.h>
#endif
#ifdef HAVE_LIBBTRFS
#ifdef HAVE_BTRFS_VERSION_H
#include <btrfs/version.h>
#else
#define BTRFS_LIB_VERSION (100)
#endif
#include <btrfs/ioctl.h>
#include <btrfs/send.h>
#include <btrfs/send-stream.h>
#include <btrfs/send-utils.h>
#include <boost/version.hpp>
#include <boost/thread.hpp>
#endif
#include <boost/algorithm/string.hpp>

#include "snapper/Log.h"
#include "snapper/Btrfs.h"
#include "snapper/BtrfsUtils.h"
#include "snapper/File.h"
#include "snapper/Snapper.h"
#include "snapper/SnapperTmpl.h"
#include "snapper/SnapperDefines.h"
#include "snapper/Acls.h"


namespace snapper
{
    using namespace std;


    Filesystem*
    Btrfs::create(const string& fstype, const string& subvolume, const string& root_prefix)
    {
	if (fstype == "btrfs")
	    return new Btrfs(subvolume, root_prefix);

	return NULL;
    }


    Btrfs::Btrfs(const string& subvolume, const string& root_prefix)
	: Filesystem(subvolume, root_prefix), qgroup(no_qgroup)
    {
    }


    void
    Btrfs::evalConfigInfo(const ConfigInfo& config_info)
    {
	string qgroup_str;
	if (config_info.getValue("QGROUP", qgroup_str) && !qgroup_str.empty())
	{
	    try
	    {
		qgroup = make_qgroup(qgroup_str);
	    }
	    catch (const runtime_error& e)
	    {
		y2err("failed to parse qgroup '" << qgroup_str << "'");
		throw InvalidConfigException();
	    }
	}
    }


    void
    Btrfs::createConfig() const
    {
	SDir subvolume_dir = openSubvolumeDir();

	try
	{
	    create_subvolume(subvolume_dir.fd(), ".snapshots");
	}
	catch (const runtime_error& e)
	{
	    y2err("create subvolume failed, " << e.what());
	    throw CreateConfigFailedException("creating btrfs snapshot failed");
	}

	SFile x(subvolume_dir, ".snapshots");
	struct stat stat;
	if (x.stat(&stat, 0) == 0)
	    x.chmod(stat.st_mode & ~0027, 0);
    }


    void
    Btrfs::deleteConfig() const
    {
	SDir subvolume_dir = openSubvolumeDir();

#ifdef ENABLE_ROLLBACK
	if (subvolume == "/")
	{
	    subvolume_dir.umount(".snapshots");

	    removeFromFstab();
	}
#endif

	try
	{
	    delete_subvolume(subvolume_dir.fd(), ".snapshots");
	}
	catch (const runtime_error& e)
	{
	    y2err("delete subvolume failed, " << e.what());
	    throw DeleteConfigFailedException("deleting btrfs snapshot failed");
	}
    }


    void
    Btrfs::addToFstab(const string& default_subvolume_name) const
    {
#ifdef ENABLE_ROLLBACK
	if (subvolume == "/")
	{
	    try
	    {
		addToFstabHelper(default_subvolume_name);
	    }
	    catch (const runtime_error& e)
	    {
		y2err("adding to fstab failed, " << e.what());
	    }
	}
#endif
    }


    void
    Btrfs::removeFromFstab() const
    {
#ifdef ENABLE_ROLLBACK
	if (subvolume == "/")
	{
	    try
	    {
		removeFromFstabHelper();
	    }
	    catch (const runtime_error& e)
	    {
		y2err("removing from fstab failed, " << e.what());
	    }
	}
#endif
    }


    string
    Btrfs::snapshotDir(unsigned int num) const
    {
	return (subvolume == "/" ? "" : subvolume) + "/.snapshots/" + decString(num) +
	    "/snapshot";
    }


    SDir
    Btrfs::openSubvolumeDir() const
    {
	SDir subvolume_dir = Filesystem::openSubvolumeDir();

	struct stat stat;
	if (subvolume_dir.stat(&stat) != 0)
	{
	    throw IOErrorException();
	}

	if (!is_subvolume(stat))
	{
	    y2err("subvolume is not a btrfs snapshot");
	    throw IOErrorException();
	}

	return subvolume_dir;
    }


    SDir
    Btrfs::openInfosDir() const
    {
	SDir subvolume_dir = openSubvolumeDir();
	SDir infos_dir(subvolume_dir, ".snapshots");

	struct stat stat;
	if (infos_dir.stat(&stat) != 0)
	{
	    throw IOErrorException();
	}

	if (!is_subvolume(stat))
	{
	    y2err(".snapshots is not a btrfs snapshot");
	    throw IOErrorException();
	}

	if (stat.st_uid != 0)
	{
	    y2err(".snapshots must have owner root");
	    throw IOErrorException();
	}

	if (stat.st_gid != 0 && stat.st_mode & S_IWGRP)
	{
	    y2err(".snapshots must have group root or must not be group-writable");
	    throw IOErrorException();
	}

	if (stat.st_mode & S_IWOTH)
	{
	    y2err(".snapshots must not be world-writable");
	    throw IOErrorException();
	}

	return infos_dir;
    }


    SDir
    Btrfs::openSnapshotDir(unsigned int num) const
    {
	SDir info_dir = openInfoDir(num);
	SDir snapshot_dir(info_dir, "snapshot");

	return snapshot_dir;
    }


    void
    Btrfs::createSnapshot(unsigned int num, unsigned int num_parent, bool read_only) const
    {
	if (num_parent == 0)
	{
	    SDir subvolume_dir = openSubvolumeDir();
	    SDir info_dir = openInfoDir(num);

	    try
	    {
		create_snapshot(subvolume_dir.fd(), info_dir.fd(), "snapshot", read_only, qgroup);
	    }
	    catch (const runtime_error& e)
	    {
		y2err("create snapshot failed, " << e.what());
		throw CreateSnapshotFailedException();
	    }
	}
	else
	{
	    SDir snapshot_dir = openSnapshotDir(num_parent);
	    SDir info_dir = openInfoDir(num);

	    try
	    {
		create_snapshot(snapshot_dir.fd(), info_dir.fd(), "snapshot", read_only, qgroup);
	    }
	    catch (const runtime_error& e)
	    {
		y2err("create snapshot failed, " << e.what());
		throw CreateSnapshotFailedException();
	    }
	}
    }


#ifdef ENABLE_ROLLBACK

    void
    Btrfs::createSnapshotOfDefault(unsigned int num, bool read_only) const
    {
	SDir subvolume_dir = openSubvolumeDir();
	unsigned long long id = get_default_id(subvolume_dir.fd());
	string name = get_subvolume(subvolume_dir.fd(), id);

	bool found = false;
	MtabData mtab_data;
	if (!getMtabData(subvolume, found, mtab_data))
	{
	    y2err("failed to find device");
	    throw CreateSnapshotFailedException();
	}

	SDir infos_dir = openInfosDir();
	TmpMount tmp_mount(infos_dir, mtab_data.device, "tmp-mnt-XXXXXX", "btrfs", 0,
			   "subvol=" + name);

	SDir tmp_mount_dir(infos_dir, tmp_mount.getName());
	SDir info_dir = openInfoDir(num);

	try
	{
	    create_snapshot(tmp_mount_dir.fd(), info_dir.fd(), "snapshot", read_only, qgroup);
	}
	catch (const runtime_error& e)
	{
	    y2err("create snapshot failed, " << e.what());
	    throw CreateSnapshotFailedException();
	}
    }

#else

    void
    Btrfs::createSnapshotOfDefault(unsigned int num, bool read_only) const
    {
	throw std::logic_error("not implemented");
    }

#endif


    void
    Btrfs::deleteSnapshot(unsigned int num) const
    {
	SDir info_dir = openInfoDir(num);

	try
	{
	    delete_subvolume(info_dir.fd(), "snapshot");
	}
	catch (const runtime_error& e)
	{
	    y2err("delete snapshot failed, " << e.what());
	    throw DeleteSnapshotFailedException();
	}
    }


    bool
    Btrfs::isSnapshotMounted(unsigned int num) const
    {
	return true;
    }


    void
    Btrfs::mountSnapshot(unsigned int num) const
    {
    }


    void
    Btrfs::umountSnapshot(unsigned int num) const
    {
    }


    bool
    Btrfs::isSnapshotReadOnly(unsigned int num) const
    {
	SDir snapshot_dir = openSnapshotDir(num);
	return is_subvolume_read_only(snapshot_dir.fd());
    }


    bool
    Btrfs::checkSnapshot(unsigned int num) const
    {
	try
	{
	    SDir info_dir = openInfoDir(num);

	    struct stat stat;
	    int r = info_dir.stat("snapshot", &stat, AT_SYMLINK_NOFOLLOW);
	    return r == 0 && is_subvolume(stat);
	}
	catch (const IOErrorException& e)
	{
	    return false;
	}
    }


#ifdef HAVE_LIBBTRFS


    class StreamProcessor;


    class tree_node
    {
    public:

	typedef map<string, tree_node>::iterator iterator;
	typedef map<string, tree_node>::const_iterator const_iterator;

	tree_node() : status(0) {}

	int status;

	map<string, tree_node> childs;

	tree_node* find(const string& name);

	tree_node* insert(const string& name);

	bool erase(const string& name);

	bool rename(const string& o, const string& n);

	void dump(const string& prefix = "") const;

	unsigned int check(StreamProcessor* processor, const string& name, unsigned int status) const;
	void check(StreamProcessor* processor, const string& prefix = "");

	void result(cmpdirs_cb_t cb, const string& prefix = "") const;

    };


    tree_node*
    tree_node::find(const string& name)
    {
	string::size_type pos = name.find('/');
	if (pos == string::npos)
	{
	    iterator it = childs.find(name);
	    if (it == childs.end())
		return NULL;

	    return &it->second;
	}
	else
	{
	    string a = name.substr(0, pos);
	    iterator it = childs.find(a);
	    if (it == childs.end())
		return NULL;

	    string b = name.substr(pos + 1);
	    return it->second.find(b);
	}
    }


    tree_node*
    tree_node::insert(const string& name)
    {
	string::size_type pos = name.find('/');
	if (pos == string::npos)
	{
	    iterator it = childs.find(name);
	    if (it == childs.end())
		it = childs.insert(childs.end(), make_pair(name, tree_node()));

	    return &it->second;
	}
	else
	{
	    string a = name.substr(0, pos);
	    iterator it = childs.find(a);
	    if (it == childs.end())
		it = childs.insert(childs.end(), make_pair(a, tree_node()));

	    string b = name.substr(pos + 1);
	    return it->second.insert(b);
	}
    }


    bool
    tree_node::erase(const string& name)
    {
	string::size_type pos = name.find('/');
	if (pos == string::npos)
	{
	    iterator it = childs.find(name);
	    if (it == childs.end())
		return false;

	    if (it->second.childs.empty())
		childs.erase(it);
	    else
		it->second.status = 0;

	    return true;
	}
	else
	{
	    string a = name.substr(0, pos);
	    iterator it = childs.find(a);
	    if (it == childs.end())
		return false;

	    string b = name.substr(pos + 1);
	    it->second.erase(b);

	    if (it->second.status == 0 && it->second.childs.empty())
		childs.erase(it);

	    return true;
	}
    }


    bool
    tree_node::rename(const string& o, const string& n)
    {
	tree_node* oo = find(o);
	if (!oo)
	    return false;

	tree_node* nn = find(n);
	if (nn)
	    return false;

	nn = insert(n);
	swap(nn->childs, oo->childs);
	nn->status = oo->status;
	erase(o);

	return true;
    }


    void
    tree_node::dump(const string& prefix) const
    {
	for (const_iterator it = childs.begin(); it != childs.end(); ++it)
	{
	    if (prefix.empty())
	    {
		y2deb(it->first << "  " << statusToString(it->second.status));
		it->second.dump(it->first);
	    }
	    else
	    {
		y2deb(prefix + "/" + it->first << "  " << statusToString(it->second.status));
		it->second.dump(prefix + "/" + it->first);
	    }
	}
    }


    struct BtrfsSendReceiveException : public SnapperException
    {
	explicit BtrfsSendReceiveException() throw() {}
	virtual const char* what() const throw() { return "Btrfs send/receive error"; }
    };


    class StreamProcessor
    {
    public:

	StreamProcessor(const SDir& base, const SDir& dir1, const SDir& dir2);

	const SDir& base;
	const SDir& dir1;
	const SDir& dir2;

	void process(cmpdirs_cb_t cb);

	tree_node files;

	void created(const string& name);
	void deleted(const string& name);

    private:

	struct subvol_uuid_search sus;

	bool get_root_id(const string& path, u64* root_id);

	bool dumper(int fd);

#if BOOST_VERSION < 104100
	bool dumper_ret;
#endif

	void do_send(u64 parent_root_id, const vector<u64>& clone_sources);

    };


    unsigned int
    tree_node::check(StreamProcessor* processor, const string& name, unsigned int status) const
    {
	if (status & CREATED) status = CREATED;
	if (status & DELETED) status = DELETED;

	if (status & (CONTENT | PERMISSIONS | OWNER | GROUP | XATTRS | ACL))
	{
	    // TODO check for content sometimes not required
	    status &= ~(CONTENT | PERMISSIONS | OWNER | GROUP | XATTRS | ACL);

	    string dirname = snapper::dirname(name);
	    string basename = snapper::basename(name);

	    SDir subdir1 = SDir::deepopen(processor->dir1, dirname);
	    SDir subdir2 = SDir::deepopen(processor->dir2, dirname);

	    status |= cmpFiles(SFile(subdir1, basename), SFile(subdir2, basename));
	}

	return status;
    }


    void
    tree_node::check(StreamProcessor* processor, const string& prefix)
    {
	for (iterator it = childs.begin(); it != childs.end(); ++it)
	{
	    if (prefix.empty())
	    {
		it->second.status = check(processor, it->first, it->second.status);
		it->second.check(processor, it->first);
	    }
	    else
	    {
		it->second.status = check(processor, prefix + "/" + it->first, it->second.status);
		it->second.check(processor, prefix + "/" + it->first);
	    }
	}
    }


    void
    tree_node::result(cmpdirs_cb_t cb, const string& prefix) const
    {
	for (const_iterator it = childs.begin(); it != childs.end(); ++it)
	{
	    if (prefix.empty())
	    {
		if (it->second.status != 0)
		    (cb)("/" + it->first, it->second.status);
		it->second.result(cb, it->first);
	    }
	    else
	    {
		if (it->second.status != 0)
		    (cb)("/" + prefix + "/" + it->first, it->second.status);
		it->second.result(cb, prefix + "/" + it->first);
	    }
	}
    }


    StreamProcessor::StreamProcessor(const SDir& base, const SDir& dir1, const SDir& dir2)
	: base(base), dir1(dir1), dir2(dir2)
    {
	memset(&sus, 0, sizeof(sus));
	int r = subvol_uuid_search_init(base.fd(), &sus);
	if (r < 0)
	{
	    y2err("failed to initialize subvol search (" << stringerror(r) << ")");
	    throw BtrfsSendReceiveException();
	}
    }


    bool
    StreamProcessor::get_root_id(const string& path, u64* root_id)
    {
	struct subvol_info* si;
	si = subvol_uuid_search(&sus, 0, NULL, 0, path.c_str(), subvol_search_by_path);
	if (!si)
	    return false;

	*root_id = si->root_id;
	return true;
    }


    void
    StreamProcessor::created(const string& name)
    {
	tree_node* node = files.find(name);
	if (!node)
	{
	    node = files.insert(name);
	    node->status = CREATED;
	}
	else
	{
	    node->status &= ~(CREATED | DELETED);
	    node->status |= CONTENT | PERMISSIONS | OWNER | GROUP | XATTRS | ACL;
	}
    }


    void
    StreamProcessor::deleted(const string& name)
    {
	tree_node* node = files.find(name);
	if (!node)
	{
	    node = files.insert(name);
	    node->status = DELETED;
	}
	else
	{
	    files.erase(name);
	}
    }


    int
    process_subvol(const char* path, const u8* uuid, u64 ctransid, void* user)
    {
	return 0;
    }


    int
    process_snapshot(const char* path, const u8* uuid, u64 ctransid, const u8* parent_uuid,
		     u64 parent_ctransid, void* user)
    {
	return 0;
    }


    int
    process_mkfile(const char* path, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("mkfile path:'" << path << "'");
#endif

	processor->created(path);

	return 0;
    }


    int
    process_mkdir(const char* path, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("mkdir path:'" << path << "'");
#endif

	processor->created(path);

	return 0;
    }


    int
    process_mknod(const char* path, u64 mode, u64 dev, void* user)
    {
#ifdef DEBUG_PROCESS
	y2deb("mknod path:'" << path << "'");
#endif

	return 0;
    }


    int
    process_mkfifo(const char* path, void* user)
    {
#ifdef DEBUG_PROCESS
	y2deb("mkfifo path:'" << path << "'");
#endif

	return 0;
    }


    int
    process_mksock(const char* path, void* user)
    {
#ifdef DEBUG_PROCESS
	y2deb("mksock path:'" << path << "'");
#endif

	return 0;
    }


    int
    process_symlink(const char* path, const char* lnk, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("symlink path:'" << path << "' lnk:'" << lnk << "'");
#endif

	processor->created(path);

	return 0;
    }


    void
    merge(StreamProcessor* processor, tree_node* tmp, const string& from, const string& to,
	  const string& prefix = "")
    {
	for (tree_node::iterator it = tmp->childs.begin(); it != tmp->childs.end(); ++it)
	{
	    if (prefix.empty())
	    {
		string x = to + "/" + it->first;

		tree_node* node = processor->files.find(x);
		if (!node)
		{
		    node = processor->files.insert(x);
		    node->status = it->second.status;
		}
		else
		{
		    node->status &= ~(CREATED | DELETED);
		    node->status |= CONTENT | PERMISSIONS | OWNER | GROUP | XATTRS | ACL;
		}

		merge(processor, &it->second, from, to, x);
	    }
	    else
	    {
		string x = to + "/" + prefix + "/" + it->first;

		tree_node* node = processor->files.find(x);
		if (!node)
		{
		    node = processor->files.insert(x);
		    node->status = it->second.status;
		}
		else
		{
		    node->status &= ~(CREATED | DELETED);
		    node->status |= CONTENT | PERMISSIONS | OWNER | GROUP | XATTRS | ACL;
		}

		merge(processor, &it->second, from, to, x);
	    }
	}
    }


    int
    process_rename(const char* _from, const char* _to, void* user)
    {
	string from(_from);
	string to(_to);

	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("rename from:'" << from << "' to:'" << to << "'");
#endif

	tree_node* it1 = processor->files.find(from);
	if (!it1)
	{
	    processor->deleted(from);
	    processor->created(to);

	    string dirname = snapper::dirname(from);
	    string basename = snapper::basename(from);

	    struct stat buf;
	    SDir tmpdir1 = SDir::deepopen(processor->dir1, dirname);
	    if (tmpdir1.stat(basename, &buf, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(buf.st_mode))
	    {
		SDir tmpdir2(tmpdir1, basename);

		vector<string> entries = tmpdir2.entries_recursive();
		for (vector<string>::const_iterator it = entries.begin(); it != entries.end(); ++it)
		{
		    processor->deleted(from + "/" + *it);
		    processor->created(to + "/" + *it);
		}
	    }
	}
	else
	{
	    tree_node* it2 = processor->files.find(to);
	    if (!it2)
	    {
		processor->files.rename(from, to);
	    }
	    else
	    {
		tree_node tmp;
		swap(it1->childs, tmp.childs);

		processor->deleted(from);
		processor->created(to);

		merge(processor, &tmp, from, to);
	    }
	}

	return 0;
    }


    int
    process_link(const char* path, const char* lnk, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("link path:'" << path << "' lnk:'" << lnk << "'");
#endif

	processor->created(path);

	return 0;
    }


    int
    process_unlink(const char* path, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("unlink path:'" << path << "'");
#endif

	processor->deleted(path);

	return 0;
    }


    int
    process_rmdir(const char* path, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("rmdir path:'" << path << "'");
#endif

	processor->deleted(path);

	return 0;
    }


    int
    process_write(const char* path, const void* data, u64 offset, u64 len, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("write path:'" << path << "'");
#endif

	tree_node* node = processor->files.insert(path);
	node->status |= CONTENT;

	return 0;
    }


    int
    process_clone(const char* path, u64 offset, u64 len, const u8* clone_uuid, u64 clone_ctransid,
		  const char* clone_path, u64 clone_offset, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("clone path:'" << path << "'");
#endif

	tree_node* node = processor->files.insert(path);
	node->status |= CONTENT;

	return 0;
    }


    int
    process_set_xattr(const char* path, const char* name, const void* data, int len, void* user)
    {
#ifdef DEBUG_PROCESS
	y2deb("set_xattr path:'" << path << "'");
#endif

#ifdef ENABLE_XATTRS
	StreamProcessor* processor = (StreamProcessor*) user;

	tree_node* node = processor->files.insert(path);
	node->status |= XATTRS;

	if (is_acl_signature(name))
	{
	    #ifdef DEBUG_PROCESS
		y2deb("adding acl flag, signature:'" << name << "'");
	    #endif
	    node->status |= ACL;
	}
#endif

	return 0;
    }


    int
    process_remove_xattr(const char* path, const char* name, void* user)
    {
#ifdef DEBUG_PROCESS
	y2deb("remove_xattr path:'" << path << "'");
#endif

#ifdef ENABLE_XATTRS
	StreamProcessor* processor = (StreamProcessor*) user;

	tree_node* node = processor->files.insert(path);
	node->status |= XATTRS;

	if (is_acl_signature(name))
	{
	    #ifdef DEBUG_PROCESS
		y2deb("adding acl flag, signature:'" << name << "'");
	    #endif
	    node->status |= ACL;
	}
#endif

	return 0;
    }


    int
    process_truncate(const char* path, u64 size, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("truncate path:'" << path << "' size:" << size);
#endif

	tree_node* node = processor->files.insert(path);
	node->status |= CONTENT;

	return 0;
    }


    int
    process_chmod(const char* path, u64 mode, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("chmod path:'" << path << "'");
#endif

	tree_node* node = processor->files.insert(path);
	node->status |= PERMISSIONS;

	return 0;
    }


    int
    process_chown(const char* path, u64 uid, u64 gid, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("chown path:'" << path << "'");
#endif

	tree_node* node = processor->files.insert(path);
	node->status |= OWNER | GROUP;

	return 0;
    }


    int
    process_utimes(const char* path, struct timespec* at, struct timespec* mt, struct timespec* ct,
		   void* user)
    {
	return 0;
    }


    int
    process_update_extent(const char* path, u64 offset, u64 len, void* user)
    {
	StreamProcessor* processor = (StreamProcessor*) user;

#ifdef DEBUG_PROCESS
	y2deb("update_extent path:'" << path << "'");
#endif

	tree_node* node = processor->files.insert(path);
	node->status |= CONTENT;

	return 0;
    }


    struct btrfs_send_ops send_ops = {
	/* .subvol = */ process_subvol,
	/* .snapshot = */ process_snapshot,
	/* .mkfile = */ process_mkfile,
	/* .mkdir = */ process_mkdir,
	/* .mknod = */ process_mknod,
	/* .mkfifo = */ process_mkfifo,
	/* .mksock = */ process_mksock,
	/* .symlink = */ process_symlink,
	/* .rename = */ process_rename,
	/* .link = */ process_link,
	/* .unlink = */ process_unlink,
	/* .rmdir = */ process_rmdir,
	/* .write = */ process_write,
	/* .clone = */ process_clone,
	/* .set_xattr = */ process_set_xattr,
	/* .remove_xattr = */ process_remove_xattr,
	/* .truncate = */ process_truncate,
	/* .chmod = */ process_chmod,
	/* .chown = */ process_chown,
	/* .utimes = */ process_utimes,
	/* .update_extent = */ process_update_extent,
    };


    bool
    StreamProcessor::dumper(int fd)
    {
	while (true)
	{
	    boost::this_thread::interruption_point();

	     // remove the fourth parameter for older versions of libbtrfs
	    int r;

#if BTRFS_LIB_VERSION < 101
	    r = btrfs_read_and_process_send_stream(fd, &send_ops, &*this, 0);
#else
	    r = btrfs_read_and_process_send_stream(fd, &send_ops, &*this, 0, 1);
#endif

	    if (r < 0)
	    {
		y2err("btrfs_read_and_process_send_stream failed");

#if BOOST_VERSION < 104100
		dumper_ret = false;
#endif

		return false;
	    }

	    if (r)
	    {
#if BOOST_VERSION < 104100
		dumper_ret = true;
#endif

		return true;
	    }

	}
    }


    void
    StreamProcessor::do_send(u64 parent_root_id, const vector<u64>& clone_sources)
    {
	int pipefd[2];
	int r1 = pipe(pipefd);
	if (r1 < 0)
	{
	    y2err("pipe failed errno:" << errno << " (" << stringerror(errno) << ")");
	    throw BtrfsSendReceiveException();
	}

	struct btrfs_ioctl_send_args io_send;
	memset(&io_send, 0, sizeof(io_send));
	io_send.send_fd = pipefd[1];
	io_send.clone_sources = const_cast<u64*>(&clone_sources.front());
	io_send.clone_sources_count = clone_sources.size();
	io_send.parent_root = parent_root_id;
	io_send.flags = BTRFS_SEND_FLAG_NO_FILE_DATA;

#if BOOST_VERSION >= 104100

	boost::packaged_task<bool> pt(boost::bind(&StreamProcessor::dumper, this, pipefd[0]));
	boost::unique_future<bool> uf = pt.get_future();

	boost::thread task(boost::move(pt));

	int r2 = ioctl(dir2.fd(), BTRFS_IOC_SEND, &io_send);
	if (r2 < 0)
	{
	    y2err("send ioctl failed errno:" << errno << " (" << stringerror(errno) << ")");
	}

	close(pipefd[1]);

	uf.wait();

	close(pipefd[0]);

	if (r2 < 0 || !uf.get())
	{
	    throw BtrfsSendReceiveException();
	}

#else

	boost::thread dumper_thread(boost::bind(&StreamProcessor::dumper, this, pipefd[0]));

	int r2 = ioctl(dir2.fd(), BTRFS_IOC_SEND, &io_send);
	if (r2 < 0)
	{
	    y2err("send ioctl failed errno:" << errno << " (" << stringerror(errno) << ")");
	}

	close(pipefd[1]);

	dumper_thread.join();

	close(pipefd[0]);

	if (r2 < 0 || !dumper_ret)
	{
	    throw BtrfsSendReceiveException();
	}

#endif
    }


    static bool
    is_subvolume_ro(const SDir& dir)
    {
	u64 flags;
	if (ioctl(dir.fd(), BTRFS_IOC_SUBVOL_GETFLAGS, &flags) < 0)
	{
	    throw IOErrorException();
	}

	return flags & BTRFS_SUBVOL_RDONLY;
    }


    void
    StreamProcessor::process(cmpdirs_cb_t cb)
    {
	y2mil("dir1:'" << dir1.fullname() << "' dir2:'" << dir2.fullname() << "'");

	if (!is_subvolume_ro(dir1) || !is_subvolume_ro(dir2))
	{
	    y2err("not read-only snapshots");
	    throw BtrfsSendReceiveException();
	}

	u64 parent_root_id = 0;
	string name1 = string(dir1.fullname(), base.fullname().size() + 1);
	if (!get_root_id(name1, &parent_root_id))
	{
	    y2err("could not resolve root_id for " << name1);
	    throw BtrfsSendReceiveException();
	}

	vector<u64> clone_sources;
	clone_sources.push_back(parent_root_id);

	do_send(parent_root_id, clone_sources);

	files.check(&*this);
	files.result(cb);
    }


    void
    Btrfs::cmpDirs(const SDir& dir1, const SDir& dir2, cmpdirs_cb_t cb) const
    {
	y2mil("special btrfs cmpDirs");

	try
	{
	    StopWatch stopwatch;

	    const SDir subvolume(openSubvolumeDir());

	    StreamProcessor processor(subvolume, dir1, dir2);

	    processor.process(cb);

	    y2mil("stopwatch " << stopwatch << " for comparing directories");
	}
	catch (const SnapperException& e)
	{
	    y2err("special btrfs cmpDirs failed, " << e.what());
	    y2mil("cmpDirs fallback");

	    snapper::cmpDirs(dir1, dir2, cb);
	}
    }


#else


    void
    Btrfs::cmpDirs(const SDir& dir1, const SDir& dir2, cmpdirs_cb_t cb) const
    {
	snapper::cmpDirs(dir1, dir2, cb);
    }


#endif



#ifdef ENABLE_ROLLBACK

    void
    Btrfs::setDefault(unsigned int num) const
    {
	try
	{
	    if (num == 0)
	    {
		SDir subvolume_dir = openSubvolumeDir();
		unsigned long long id = get_id(subvolume_dir.fd());
		set_default_id(subvolume_dir.fd(), id);
	    }
	    else
	    {
		SDir snapshot_dir = openSnapshotDir(num);
		unsigned long long id = get_id(snapshot_dir.fd());

		SDir subvolume_dir = openSubvolumeDir();
		set_default_id(subvolume_dir.fd(), id);
	    }
	}
	catch (const runtime_error& e)
	{
	    y2err("set default failed, " << e.what());
	    throw IOErrorException();
	}
    }

#else

    void
    Btrfs::setDefault(unsigned int num) const
    {
	throw std::logic_error("not implemented");
    }

#endif


#ifdef ENABLE_ROLLBACK

    class MntTable
    {

    public:

	MntTable(const string& root_prefix)
	    : root_prefix(root_prefix), table(mnt_new_table())
	{
	    if (!table)
		throw runtime_error("mnt_new_table failed");

	    mnt_table_enable_comments(table, 1);
	}

	~MntTable()
	{
	    mnt_reset_table(table);
	}

	void parse_fstab()
	{
	    if (mnt_table_parse_fstab(table, target_fstab().c_str()) != 0)
		throw runtime_error("mnt_table_parse_fstab failed");
	}

	void replace_file()
	{
	    if (mnt_table_replace_file(table, target_fstab().c_str()) != 0)
		throw runtime_error("mnt_table_replace_file failed");
	}

	struct libmnt_fs* find_target(const string& path, int directon)
	{
	    return mnt_table_find_target(table, path.c_str(), directon);
	}

	void add_fs(struct libmnt_fs* fs)
	{
	    if (mnt_table_add_fs(table, fs) != 0)
		throw runtime_error("mnt_table_add_fs failed");
	}

	void remove_fs(struct libmnt_fs* fs)
	{
	    if (mnt_table_remove_fs(table, fs) != 0)
		throw runtime_error("mnt_table_remove_fs failed");
	}

    private:

	string target_fstab() const
	{
	    return prepend_root_prefix(root_prefix, "/etc/fstab");
	}

	const string root_prefix;

	struct libmnt_table* table;

    };


    void
    Btrfs::addToFstabHelper(const string& default_subvolume_name) const
    {
	string subvol_option = default_subvolume_name;
	if (!subvol_option.empty())
	    subvol_option += "/";
	subvol_option += ".snapshots";

	MntTable mnt_table(root_prefix);
	mnt_table.parse_fstab();

	libmnt_fs* root = mnt_table.find_target(subvolume, MNT_ITER_FORWARD);
	if (!root)
	    throw runtime_error("root entry not found");

	libmnt_fs* snapshots = mnt_copy_fs(NULL, root);
	if (!snapshots)
	    throw runtime_error("mnt_copy_fs failed");

	mnt_fs_set_target(snapshots, "/.snapshots");

	char* options = mnt_fs_strdup_options(snapshots);
	mnt_optstr_remove_option(&options, "defaults");
	mnt_optstr_set_option(&options, "subvol", subvol_option.c_str());
	mnt_fs_set_options(snapshots, options);
	free(options);

	mnt_table.add_fs(snapshots);
	mnt_table.replace_file();
    }


    void
    Btrfs::removeFromFstabHelper() const
    {
	MntTable mnt_table(root_prefix);
	mnt_table.parse_fstab();

	string mountpoint = (subvolume == "/" ? "" : subvolume) +  "/.snapshots";
	libmnt_fs* snapshots = mnt_table.find_target(mountpoint, MNT_ITER_FORWARD);
	if (!snapshots)
	    return;

	mnt_table.remove_fs(snapshots);
	mnt_table.replace_file();
    }

#endif
}
