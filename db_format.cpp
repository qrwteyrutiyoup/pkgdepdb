#include <string.h>

#include <zlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

#include <memory>
#include <algorithm>
#include <utility>

#include "main.h"
#include "db_format.h"

// version
uint16_t
DB::CURRENT = 8;

// magic header
static const char
depdb_magic[] = { 'A', 'r', 'c', 'h',
                  'B', 'S', 'D',  0,
                  'd', 'e', 'p', 's',
                  '~', 'D', 'B', '~' };

namespace DBFlags {
  enum {
    IgnoreRules   = (1<<0),
    PackageLDPath = (1<<1),
    BasePackages  = (1<<2),
    StrictLinking = (1<<3),
    AssumeFound   = (1<<4),
    FileLists     = (1<<5)
  };
}

using HdrFlags = uint16_t;
using Header = struct {
  uint8_t   magic[sizeof(depdb_magic)];
  uint16_t  version;
  HdrFlags  flags;
  uint8_t   reserved[22];
};

// Simple straight forward data serialization by keeping track
// of already-serialized objects.
// Lame, but effective.

enum class ObjRef : uint8_t {
  PKG,
  PKGREF,
  OBJ,
  OBJREF
};

class SerialFile : public SerialStream {
public:
  int  fd;
  bool err;
  size_t ppos, gpos;

  SerialFile(const std::string& file, InOut dir)
  : ppos(0), gpos(0)
  {
    int locktype;
    if (dir == SerialStream::out) {
      fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      locktype = LOCK_EX;
    }
    else {
      fd = ::open(file.c_str(), O_RDONLY);
      locktype = LOCK_SH;
    }
    if (fd < 0) {
      err = true;
      return;
    }
    err = (::flock(fd, locktype) != 0);
    if (err) {
      ::close(fd);
    }
  }

  ~SerialFile()
  {
    if (fd >= 0)
      ::close(fd);
  }

  virtual operator bool() const {
    return fd >= 0 && !err;
  }

  virtual ssize_t
  write(const void *buf, size_t bytes)
  {
    auto r = ::write(fd, buf, bytes);
    ppos += r;
    return r;
  }

  virtual ssize_t
  read(void *buf, size_t bytes)
  {
    auto r = ::read(fd, buf, bytes);
    gpos += r;
    return r;
  }

  virtual size_t tellp() const {
    return ppos;
  }
  virtual size_t tellg() const {
    return gpos;
  }
};

class SerialGZ : public SerialStream {
public:
  gzFile out;
  bool   err;

  SerialGZ(const std::string& file, InOut dir)
  {
    int fd;
    int locktype;
    out = 0;
    if (dir == SerialStream::out) {
      fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      locktype = LOCK_EX;
    }
    else {
      fd = ::open(file.c_str(), O_RDONLY);
      locktype = LOCK_SH;
    }
    if (fd < 0) {
      err = true;
      return;
    }
    err = (::flock(fd, locktype) != 0);
    if (err) {
      ::close(fd);
      err = true;
      return;
    }
    out = gzdopen(fd, (dir == SerialStream::out ? "wb" : "rb"));
    if (!out) {
      err = true;
      ::close(fd);
    }
  }

  ~SerialGZ()
  {
    if (out)
      gzclose(out);
  }

  virtual operator bool() const {
    return out && !err;
  }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
  virtual ssize_t
  write(const void *buf, size_t bytes)
  {
    return gzwrite(out, buf, bytes);
  }

  virtual ssize_t
  read(void *buf, size_t bytes)
  {
    return gzread(out, buf, bytes);
  }
#pragma clang diagnostic pop

  virtual size_t tellp() const {
    return gztell(out);
  }
  virtual size_t tellg() const {
    return gztell(out);
  }
};

SerialIn::SerialIn(DB *db_, SerialStream *in__)
: db(db_), in(*in__), in_(in__), ver8_refs(false)
{ }

SerialIn*
SerialIn::open(DB *db, const std::string& file, bool gz)
{
  SerialStream *in = gz ? (SerialStream*)new SerialGZ(file, SerialStream::in)
                        : (SerialStream*)new SerialFile(file, SerialStream::in);
  if (!in) return 0;
  if (!*in) {
    delete in;
    return 0;
  }

  SerialIn *s = new SerialIn(db, in);
  return s;
}

SerialOut::SerialOut(DB *db_, SerialStream *out__)
: db(db_), out(*out__), out_(out__)
{ }

SerialOut*
SerialOut::open(DB *db, const std::string& file, bool gz)
{
  SerialStream *out = gz ? (SerialStream*)new SerialGZ(file, SerialStream::out)
                         : (SerialStream*)new SerialFile(file, SerialStream::out);
  if (!out) return 0;
  if (!*out) {
    delete out;
    return 0;
  }

  SerialOut *s = new SerialOut(db, out);
  return s;
}

bool SerialOut::GetObjRef(const Elf *e, size_t *out) {
  auto exists = objref.find(e);
  if (exists != objref.end()) {
    *out = exists->second;
    return true;
  }
  objref[e] = *out = objref.size();
  return false;
}

bool SerialOut::GetPkgRef(const Package *p, size_t *out) {
  auto exists = pkgref.find(p);
  if (exists != pkgref.end()) {
    *out = exists->second;
    return true;
  }
  pkgref[p] = *out = pkgref.size();
  return false;
}

static bool write_obj(SerialOut &out, const Elf *obj);
static bool read_obj (SerialIn  &in,  rptr<Elf> &obj);

bool
write_objlist(SerialOut &out, const ObjectList& list)
{
  auto len = static_cast<uint32_t>(list.size());
  out.out.write((const char*)&len, sizeof(len));
  for (auto &obj : list) {
    if (!write_obj(out, obj))
      return false;
  }
  return out.out;
}

bool
read_objlist(SerialIn &in, ObjectList& list)
{
  uint32_t len;
  in >= len;
  list.resize(len);
  for (size_t i = 0; i != len; ++i) {
    if (!read_obj(in, list[i]))
      return false;
  }
  return in.in;
}

bool
write_objset(SerialOut &out, const ObjectSet& list)
{
  auto len = static_cast<uint32_t>(list.size());
  out.out.write((const char*)&len, sizeof(len));
  for (auto &obj : list) {
    if (!write_obj(out, obj))
      return false;
  }
  return out.out;
}

bool
read_objset(SerialIn &in, ObjectSet& list)
{
#if 0
  ObjectList lst;
  if (!read_objlist(in, lst))
    return false;
  list.~ObjectSet();
  new (&list) ObjectSet(lst.begin(), lst.end());
#else
  uint32_t len;
  in >= len;
  for (size_t i = 0; i != len; ++i) {
    rptr<Elf> obj;
    if (!read_obj(in, obj))
      return false;
    list.insert(obj);
  }
#endif
  return in.in;
}

bool
write_stringlist(SerialOut &out, const std::vector<std::string> &list)
{
  auto len = static_cast<uint32_t>(list.size());
  out.out.write((const char*)&len, sizeof(len));
  for (auto &s : list)
    out <= s;
  return out.out;
}

bool
read_stringlist(SerialIn &in, std::vector<std::string> &list)
{
  static std::string s;
  uint32_t len;
  in >= len;
  list.reserve(len);
  for (uint32_t i = 0; i != len; ++i) {
    in >= s;
    list.emplace_back(std::move(s));
  }
  return in.in;
}

bool
write_stringset(SerialOut &out, const StringSet &list)
{
  auto len = static_cast<uint32_t>(list.size());
  out.out.write((const char*)&len, sizeof(len));
  for (auto &s : list)
    out <= s;
  return out.out;
}

bool
read_stringset(SerialIn &in, StringSet &list)
{
#if 1
  StringList lst;
  if (!read_stringlist(in, lst))
    return false;
  list.~StringSet();
  new (&list) StringSet(lst.begin(), lst.end());
#else
  // "HINTS"... yeah right :P
  uint32_t len;
  in >= len;
  StringSet::iterator hint = list.end();
  for (uint32_t i = 0; i != len; ++i) {
    std::string str;
    in >= str;
    hint = list.emplace_hint(hint, std::move(str));
  }
#endif
  return in.in;
}

static bool
write_obj(SerialOut &out, const Elf *obj)
{
  // check if the object has already been serialized

  size_t ref = (size_t)-1;
  if (out.GetObjRef(obj, &ref)) {
    out <= ObjRef::OBJREF <= ref;
    return true;
  }

  // OBJ ObjRef; and remember our pointer in the ObjOutMap
  out <= ObjRef::OBJ;

  // Serialize the actual object data
  out <= obj->dirname_
      <= obj->basename_
      <= obj->ei_class_
      <= obj->ei_data_
      <= obj->ei_osabi_
      <= (uint8_t)obj->rpath_set_
      <= (uint8_t)obj->runpath_set_
      <= obj->rpath_
      <= obj->runpath_;

  if (!write_stringlist(out, obj->needed_))
    return false;

  return true;
}

static bool
read_obj(SerialIn &in, rptr<Elf> &obj)
{
  ObjRef r;
  in >= r;
  size_t ref;
  if (r == ObjRef::OBJREF) {
    in >= ref;
    if (in.ver8_refs) {
      if (ref >= in.objref.size()) {
        log(Error, "db error: objref out of range\n");
        return false;
      }
      obj = in.objref[ref];
    } else {
      auto existing = in.old_objref.find(ref);
      if (existing == in.old_objref.end()) {
        log(Error, "db error: failed to find previously deserialized object\n");
        return false;
      }
      obj = existing->second;
    }
    return true;
  }
  if (r != ObjRef::OBJ) {
    log(Error, "object expected, object-ref value: %u\n", (unsigned)r);
    return false;
  }

  // Remember the one we're constructing now:
  obj = new Elf;
  if (in.ver8_refs)
    in.objref.push_back(obj.get());
  else {
    ref = in.in.tellg();
    in.old_objref[ref] = obj.get();
  }

  // Read out the object data

  // Serialize the actual object data
  uint8_t rpset, runpset;
  in >= obj->dirname_
     >= obj->basename_
     >= obj->ei_class_
     >= obj->ei_data_
     >= obj->ei_osabi_
     >= rpset
     >= runpset
     >= obj->rpath_
     >= obj->runpath_;
  obj->rpath_set_   = rpset;
  obj->runpath_set_ = runpset;

  if (!read_stringlist(in, obj->needed_))
    return false;

  return true;
}

static bool
write_pkg(SerialOut &out, Package *pkg, unsigned hdrver, HdrFlags flags)
{
  // check if the package has already been serialized
  size_t ref = (size_t)-1;
  if (out.GetPkgRef(pkg, &ref)) {
    out <= ObjRef::PKGREF <= ref;
    return true;
  }

  // PKG ObjRef; and remember our pointer in the PkgOutMap
  out <= ObjRef::PKG;

  // Now serialize the actual package data:
  out <= pkg->name_
      <= pkg->version_;
  if (!write_objlist(out, pkg->objects_))
    return false;

  if (hdrver >= 3) {
    if (!write_stringlist(out, pkg->depends_) ||
        !write_stringlist(out, pkg->optdepends_))
    {
      return false;
    }
  }
  if (hdrver >= 4) {
    if (!write_stringlist(out, pkg->provides_)  ||
        !write_stringlist(out, pkg->conflicts_) ||
        !write_stringlist(out, pkg->replaces_))
    {
      return false;
    }
  }
  if (hdrver >= 5 && !write_stringset(out, pkg->groups_))
    return false;

  if (flags & DBFlags::FileLists && !write_stringlist(out, pkg->filelist_))
    return false;

  return true;
}

static bool
read_pkg(SerialIn &in, Package *&pkg, unsigned hdrver, HdrFlags flags)
{
  ObjRef r;
  in >= r;
  size_t ref;
  if (r == ObjRef::PKGREF) {
    in >= ref;
    if (in.ver8_refs) {
      if (ref >= in.pkgref.size()) {
        log(Error, "db error: pkgref out of range\n");
        return false;
      }
      pkg = in.pkgref[ref];
    } else {
      auto existing = in.old_pkgref.find(ref);
      if (existing == in.old_pkgref.end()) {
        log(Error, "db error: failed to find previously deserialized package\n");
        return false;
      }
      pkg = existing->second;
    }
    return true;
  }
  if (r != ObjRef::PKG) {
    log(Error, "package expected, object-ref value: %u\n", (unsigned)r);
    return false;
  }

  // Remember the one we're constructing now:
  pkg = new Package;
  if (in.ver8_refs)
    in.pkgref.push_back(pkg);
  else {
    ref = in.in.tellg();
    in.old_pkgref[ref] = pkg;
  }

  // Now serialize the actual package data:
  in >= pkg->name_
     >= pkg->version_;
  if (!read_objlist(in, pkg->objects_))
    return false;
  for (auto &o : pkg->objects_)
    o->owner_ = pkg;

  if (hdrver >= 3) {
    if (!read_stringlist(in, pkg->depends_) ||
        !read_stringlist(in, pkg->optdepends_))
    {
      return false;
    }
  }
  if (hdrver >= 4) {
    if (!read_stringlist(in, pkg->provides_) ||
        !read_stringlist(in, pkg->conflicts_) ||
        !read_stringlist(in, pkg->replaces_))
    {
      return false;
    }
  }
  if (hdrver >= 5 && !read_stringset(in, pkg->groups_))
    return false;

  if (flags & DBFlags::FileLists && !read_stringlist(in, pkg->filelist_))
    return false;

  return true;
}

static inline bool
ends_with_gz(const std::string& str)
{
  size_t pos = str.find_last_of('.');
  return (pos == str.length()-3 &&
          str.compare(pos, 3, ".gz") == 0);
}

static bool
db_store(DB *db, const std::string& filename)
{
  bool mkgzip = ends_with_gz(filename);
  std::unique_ptr<SerialOut> sout(SerialOut::open(db, filename, mkgzip));

  if (mkgzip)
    log(Message, "writing compressed database\n");
  else
    log(Message, "writing database\n");

  SerialOut &out(*sout);

  if (!sout || !out.out) {
    log(Error, "failed to open output file %s for writing\n", filename.c_str());
    return false;
  }

  Header hdr;
  memset(&hdr, 0, sizeof(hdr));

  memcpy(hdr.magic, depdb_magic, sizeof(hdr.magic));
  hdr.version = 1;

  // flags:
  if (db->ignore_file_rules_.size())
    hdr.flags |= DBFlags::IgnoreRules;
  if (db->package_library_path_.size())
    hdr.flags |= DBFlags::PackageLDPath;
  if (db->base_packages_.size())
    hdr.flags |= DBFlags::BasePackages;
  if (db->strict_linking_)
    hdr.flags |= DBFlags::StrictLinking;
  if (db->assume_found_rules_.size())
    hdr.flags |= DBFlags::AssumeFound;
  if (db->contains_filelists_)
    hdr.flags |= DBFlags::FileLists;

  // Figure out which database format version this will be
  if (hdr.flags & DBFlags::FileLists)
    hdr.version = 7;
  else if (hdr.flags & DBFlags::AssumeFound)
    hdr.version = 6;
  else if (db->contains_groups_)
    hdr.version = 5;
  else if (db->contains_package_depends_)
    hdr.version = 4;
  else if (hdr.flags)
      hdr.version = 2;

  // okay

  // ver8 introduces faster refs...
  if (hdr.version < 8)
    hdr.version = 8;

  out <= hdr;
  out <= db->name_;
  if (!write_stringlist(out, db->library_path_))
    return false;

  out <= (uint32_t)db->packages_.size();
  for (auto &pkg : db->packages_) {
    if (!write_pkg(out, pkg, hdr.version, hdr.flags))
      return false;
  }

  uint32_t cnt_found = 0,
           cnt_missing = 0;
  {
    out <= (uint32_t)db->objects_.size();
    for (auto &obj : db->objects_) {
      if (!write_obj(out, obj))
        return false;
      if (!obj->req_found_.empty())
        ++cnt_found;
      if (!obj->req_missing_.empty())
        ++cnt_missing;
    }
  }

  out <= cnt_found;
  for (Elf *obj : db->objects_) {
    if (obj->req_found_.empty())
      continue;
    if (!write_obj(out, obj))
      return false;
    if (!write_objset(out, obj->req_found_))
      return false;
  }
  out <= cnt_missing;
  for (Elf *obj : db->objects_) {
    if (obj->req_missing_.empty())
      continue;
    if (!write_obj(out, obj))
      return false;
    if (!write_stringset(out, obj->req_missing_))
      return false;
  }

  if (hdr.flags & DBFlags::IgnoreRules) {
    if (!write_stringset(out, db->ignore_file_rules_))
      return false;
  }
  if (hdr.flags & DBFlags::AssumeFound) {
    if (!write_stringset(out, db->assume_found_rules_))
      return false;
  }

  if (hdr.flags & DBFlags::PackageLDPath) {
    out <= (uint32_t)db->package_library_path_.size();
    for (auto iter : db->package_library_path_) {
      out <= iter.first;
      if (!write_stringlist(out, iter.second))
        return false;
    }
  }

  if (hdr.flags & DBFlags::BasePackages) {
    if (!write_stringset(out, db->base_packages_))
      return false;
  }

  return out.out;
}

static bool
db_read(DB *db, const std::string& filename)
{
  bool gzip = ends_with_gz(filename);
  std::unique_ptr<SerialIn> sin(SerialIn::open(db, filename, gzip));

  if (gzip)
    log(Message, "reading compressed database\n");
  else
    log(Message, "reading database\n");

  SerialIn &in(*sin);
  if (!sin || !in.in) {
    //log(Error, "failed to open input file %s for reading\n", filename.c_str());
    return true; // might not exist...
  }

  Header hdr;
  in >= hdr;
  if (memcmp(hdr.magic, depdb_magic, sizeof(hdr.magic)) != 0) {
    log(Error, "not a valid database file: %s\n", filename.c_str());
    return false;
  }

  db->loaded_version_ = hdr.version;
  // supported versions:
  if (hdr.version > DB::CURRENT)
  {
    log(Error, "cannot read depdb version %u files, (known up to %u)\n",
        (unsigned)hdr.version,
        (unsigned)DB::CURRENT);
    return false;
  }

  if (hdr.version >= 8)
    in.ver8_refs = true;

  if (hdr.version >= 3)
    db->contains_package_depends_ = true;
  if (hdr.version >= 5)
    db->contains_groups_ = true;
  if (hdr.flags & DBFlags::FileLists)
    db->contains_filelists_ = true;

  in >= db->name_;
  if (!read_stringlist(in, db->library_path_)) {
    log(Error, "failed reading library paths\n");
    return false;
  }

  uint32_t len;

  in >= len;
  db->packages_.resize(len);
  for (uint32_t i = 0; i != len; ++i) {
    if (!read_pkg(in, db->packages_[i], hdr.version, hdr.flags)) {
      log(Error, "failed reading packages\n");
      return false;
    }
  }

  if (!read_objlist(in, db->objects_)) {
    log(Error, "failed reading object list\n");
    return false;
  }

  in >= len;
  rptr<Elf> obj;
  for (uint32_t i = 0; i != len; ++i) {
    if (!read_obj(in, obj) ||
        !read_objset(in, obj->req_found_))
    {
      log(Error, "failed reading map of found dependencies\n");
      return false;
    }
  }

  in >= len;
  for (uint32_t i = 0; i != len; ++i) {
    if (!read_obj(in, obj) ||
        !read_stringset(in, obj->req_missing_))
    {
      log(Error, "failed reading map of missing dependencies\n");
      return false;
    }
  }

  if (hdr.version < 2)
    return true;

  if (hdr.flags & DBFlags::IgnoreRules) {
    if (!read_stringset(in, db->ignore_file_rules_))
      return false;
  }
  if (hdr.flags & DBFlags::AssumeFound) {
    if (!read_stringset(in, db->assume_found_rules_))
      return false;
  }

  if (hdr.flags & DBFlags::PackageLDPath) {
    in >= len;
    for (uint32_t i = 0; i != len; ++i) {
      std::string pkg;
      in >= pkg;
      if (!read_stringlist(in, db->package_library_path_[pkg]))
        return false;
    }
  }

  if (hdr.flags & DBFlags::BasePackages) {
    if (!read_stringset(in, db->base_packages_))
      return false;
  }

  return true;
}


// There we go:

bool
DB::store(const std::string& filename)
{
  return db_store(this, filename);
}

bool
DB::read(const std::string& filename)
{
  if (!empty()) {
    log(Error, "internal usage error: DB::read on a non-empty db!\n");
    return false;
  }
  return db_read(this, filename);
}
