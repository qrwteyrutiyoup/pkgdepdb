#include <memory>
#include <algorithm>
#include <utility>

#include "main.h"

using ObjClass = uint32_t;

static inline ObjClass
getObjClass(unsigned char ei_class, unsigned char ei_data, unsigned char ei_osabi) {
	return (ei_data << 16) | (ei_class << 8) | ei_osabi;
}

static inline ObjClass
getObjClass(Elf *elf) {
	return getObjClass(elf->ei_class, elf->ei_data, elf->ei_osabi);
}

DB::DB() {
	loaded_version = DB::CURRENT;
	contains_package_depends = false;
}

DB::~DB() {
	for (auto &pkg : packages)
		delete pkg;
}

PackageList::const_iterator
DB::find_pkg_i(const std::string& name) const
{
	return std::find_if(packages.begin(), packages.end(),
		[&name](const Package *pkg) { return pkg->name == name; });
}

Package*
DB::find_pkg(const std::string& name) const
{
	auto pkg = find_pkg_i(name);
	return (pkg != packages.end()) ? *pkg : nullptr;
}

bool
DB::delete_package(const std::string& name)
{
	Package *old; {
		auto pkgiter = find_pkg_i(name);
		if (pkgiter == packages.end())
			return true;

		old = *pkgiter;
		packages.erase(packages.begin() + (pkgiter - packages.begin()));
	}

	for (auto &elfsp : old->objects) {
		Elf *elf = elfsp.get();
		required_found.erase(elf);
		required_missing.erase(elf);
		// remove the object from the list
		auto self = std::find(objects.begin(), objects.end(), elf);
		objects.erase(self);

		// for each object which depends on this object, search for a replacing object
		for (auto &found : required_found) {
			// does this object depend on 'elf'?
			auto ref = std::find(found.second.begin(), found.second.end(), elf);
			if (ref == found.second.end())
				continue;
			// erase
			found.second.erase(ref);
			// search for this dependency anew
			Elf *seeker = found.first;
			Elf *other;
			if (!seeker->owner || !package_library_path.size())
				other = find_for(seeker, elf->basename, nullptr);
			else {
				auto iter = package_library_path.find(seeker->owner->name);
				if (iter == package_library_path.end())
					other = find_for(seeker, elf->basename, nullptr);
				else
					other = find_for(seeker, elf->basename, &iter->second);
			}
			if (!other) {
				// it's missing now
				required_missing[seeker].insert(elf->basename);
			} else {
				// replace it with a new object
				found.second.insert(other);
			}
		}
	}

	delete old;

	std::remove_if(objects.begin(), objects.end(),
		[](rptr<Elf> &obj) { return 1 == obj->refcount; });

	return true;
}

static bool
pathlist_contains(const std::string& list, const std::string& path)
{
	size_t at = 0;
	size_t to = list.find_first_of(':', 0);
	while (to != std::string::npos) {
		if (list.compare(at, to-at, path) == 0)
			return true;
		at = to+1;
		to = list.find_first_of(':', at);
	}
	if (list.compare(at, std::string::npos, path) == 0)
		return true;
	return false;
}

bool
DB::elf_finds(Elf *elf, const std::string& path, const StringList *extrapaths) const
{
	// DT_RPATH first
	if (elf->rpath_set && pathlist_contains(elf->rpath, path))
		return true;

	// LD_LIBRARY_PATH - ignored

	// DT_RUNPATH
	if (elf->runpath_set && pathlist_contains(elf->runpath, path))
		return true;

	// Trusted Paths
	if (path == "/lib" ||
	    path == "/usr/lib")
	{
		return true;
	}

	if (std::find(library_path.begin(), library_path.end(), path) != library_path.end())
		return true;

	if (extrapaths) {
		if (std::find(extrapaths->begin(), extrapaths->end(), path) != extrapaths->end())
			return true;
	}

	return false;
}

bool
DB::install_package(Package* &&pkg)
{
	if (!delete_package(pkg->name))
		return false;

	packages.push_back(pkg);
	if (pkg->depends.size() || pkg->optdepends.size())
		contains_package_depends = true;

	const StringList *libpaths = 0;
	if (package_library_path.size()) {
		auto iter = package_library_path.find(pkg->name);
		if (iter != package_library_path.end())
			libpaths = &iter->second;
	}

	for (auto &obj : pkg->objects)
		objects.push_back(obj);

	for (auto &obj : pkg->objects) {
		ObjClass objclass = getObjClass(obj);
		// check if the object is required
		for (auto missing = required_missing.begin(); missing != required_missing.end();) {
			Elf *seeker = missing->first;
			if (getObjClass(seeker) != objclass ||
			    !elf_finds(seeker, obj->dirname, libpaths))
			{
				++missing;
				continue;
			}

			bool needs = (0 != missing->second.erase(obj->basename));

			if (needs) // the library is indeed required and found:
				required_found[seeker].insert(obj);

			if (0 == missing->second.size())
				required_missing.erase(missing++);
			else
				++missing;
		}

		// search for whatever THIS object requires
		link_object(obj, pkg);
	}

	return true;
}

Elf*
DB::find_for(Elf *obj, const std::string& needed, const StringList *extrapath) const
{
	log(Debug, "dependency of %s/%s   :  %s\n", obj->dirname.c_str(), obj->basename.c_str(), needed.c_str());
	ObjClass objclass = getObjClass(obj);
	for (auto &lib : objects) {
		if (getObjClass(lib) != objclass) {
			log(Debug, "  skipping %s/%s (objclass)\n", lib->dirname.c_str(), lib->basename.c_str());
			continue;
		}
		if (lib->basename    != needed) {
			log(Debug, "  skipping %s/%s (wrong name)\n", lib->dirname.c_str(), lib->basename.c_str());
			continue;
		}
		if (!elf_finds(obj, lib->dirname, extrapath)) {
			log(Debug, "  skipping %s/%s (not visible)\n", lib->dirname.c_str(), lib->basename.c_str());
			continue;
		}
		// same class, same name, and visible...
		return lib;
	}
	return 0;
}

void
DB::link_object(Elf *obj, Package *owner)
{
	if (ignore_file_rules.size()) {
		std::string full = obj->dirname + "/" + obj->basename;
		if (ignore_file_rules.find(full) != ignore_file_rules.end())
			return;
	}
	const StringList *libpaths = 0;
	if (package_library_path.size()) {
		auto iter = package_library_path.find(owner->name);
		if (iter != package_library_path.end())
			libpaths = &iter->second;
	}

	ObjectSet req_found;
	StringSet req_missing;
	for (auto &needed : obj->needed) {
		Elf *found = find_for(obj, needed, libpaths);
		if (found)
			req_found.insert(found);
		else
			req_missing.insert(needed);
	}
	if (req_found.size())
		required_found[obj]   = std::move(req_found);
	if (req_missing.size())
		required_missing[obj] = std::move(req_missing);
}

void
DB::relink_all()
{
	required_found.clear();
	required_missing.clear();
	if (!packages.size())
		return;
	unsigned long pkgcount = packages.size();
	double        fac   = 100.0 / double(pkgcount);
	unsigned long count = 0;
	unsigned int  pc    = 0;
	if (!opt_quiet) {
		printf("relinking: 0%% (0 / %lu packages)", pkgcount);
		fflush(stdout);
	}
	for (auto &pkg : packages) {
		for (auto &obj : pkg->objects) {
			link_object(obj, pkg);
		}
		if (!opt_quiet) {
			++count;
			unsigned int newpc = fac * double(count);
			if (newpc != pc) {
				pc = newpc;
				printf("\rrelinking: %3u%% (%lu / %lu packages)",
				       pc, count, pkgcount);
				fflush(stdout);
			}
		}
	}
	if (!opt_quiet) {
		printf("\rrelinking: 100%% (%lu / %lu packages)\n",
		       count, pkgcount);
	}
}

void
DB::fix_paths()
{
	for (auto &obj : objects) {
		fixpathlist(obj->rpath);
		fixpathlist(obj->runpath);
	}
}

bool
DB::empty() const
{
	return packages.size()         == 0 &&
	       objects.size()          == 0 &&
	       required_found.size()   == 0 &&
	       required_missing.size() == 0;
}

bool
DB::ld_clear()
{
	if (library_path.size()) {
		library_path.clear();
		return true;
	}
	return false;
}

static std::string
fixcpath(const std::string& dir)
{
	std::string s(dir);
	fixpath(s);
	return std::move(dir);
}

bool
DB::ld_append(const std::string& dir)
{
	return ld_insert(fixcpath(dir), library_path.size()-1);
}

bool
DB::ld_prepend(const std::string& dir)
{
	return ld_insert(fixcpath(dir), 0);
}

bool
DB::ld_delete(size_t i)
{
	if (!library_path.size() || i >= library_path.size())
		return false;
	library_path.erase(library_path.begin() + i);
	return true;
}

bool
DB::ld_delete(const std::string& dir_)
{
	if (!dir_.length())
		return false;
	if (dir_[0] >= '0' && dir_[0] <= '9') {
		return ld_delete(strtoul(dir_.c_str(), nullptr, 0));
	}
	std::string dir(dir_);
	fixpath(dir);
	auto old = std::find(library_path.begin(), library_path.end(), dir);
	if (old != library_path.end()) {
		library_path.erase(old);
		return true;
	}
	return false;
}

bool
DB::ld_insert(const std::string& dir_, size_t i)
{
	std::string dir(dir_);
	fixpath(dir);
	if (!library_path.size())
		i = 0;
	else if (i >= library_path.size())
		i = library_path.size()-1;

	auto old = std::find(library_path.begin(), library_path.end(), dir);
	if (old == library_path.end()) {
		library_path.insert(library_path.begin() + i, dir);
		return true;
	}
	size_t oldidx = old - library_path.begin();
	if (oldidx == i)
		return false;
	// exists
	library_path.erase(old);
	library_path.insert(library_path.begin() + i, dir);
	return true;
}

bool
DB::pkg_ld_insert(const std::string& package, const std::string& dir_, size_t i)
{
	std::string dir(dir_);
	fixpath(dir);
	StringList &path(package_library_path[package]);

	if (!path.size())
		i = 0;
	else if (i >= path.size())
		i = path.size()-1;

	auto old = std::find(path.begin(), path.end(), dir);
	if (old == path.end()) {
		path.insert(path.begin() + i, dir);
		return true;
	}
	size_t oldidx = old - path.begin();
	if (oldidx == i)
		return false;
	// exists
	path.erase(old);
	path.insert(path.begin() + i, dir);
	return true;
}

bool
DB::pkg_ld_delete(const std::string& package, const std::string& dir_)
{
	std::string dir(dir_);
	fixpath(dir);
	auto iter = package_library_path.find(package);
	if (iter == package_library_path.end())
		return false;

	StringList &path(iter->second);
	auto old = std::find(path.begin(), path.end(), dir);
	if (old != path.end()) {
		path.erase(old);
		if (!path.size())
			package_library_path.erase(iter);
		return true;
	}
	return false;
}

bool
DB::pkg_ld_delete(const std::string& package, size_t i)
{
	auto iter = package_library_path.find(package);
	if (iter == package_library_path.end())
		return false;

	StringList &path(iter->second);
	if (i >= path.size())
		return false;
	path.erase(path.begin()+i);
	if (!path.size())
		package_library_path.erase(iter);
	return true;
}

bool
DB::pkg_ld_clear(const std::string& package)
{
	auto iter = package_library_path.find(package);
	if (iter == package_library_path.end())
		return false;

	package_library_path.erase(iter);
	return true;
}

bool
DB::ignore_file(const std::string& filename)
{
	return std::get<1>(ignore_file_rules.insert(fixcpath(filename)));
}

bool
DB::unignore_file(const std::string& filename)
{
	return (ignore_file_rules.erase(fixcpath(filename)) > 0);
}

bool
DB::unignore_file(size_t id)
{
	if (id >= ignore_file_rules.size())
		return false;
	auto iter = ignore_file_rules.begin();
	while (id) {
		++iter;
		--id;
	}
	ignore_file_rules.erase(iter);
	return true;
}

void
DB::show_info()
{
	if (opt_use_json)
		return show_info_json();

	printf("DB version: %u\n", loaded_version);
	printf("DB name:    [%s]\n", name.c_str());
	printf("Additional Library Paths:\n");
	unsigned id = 0;
	for (auto &p : library_path)
		printf("  %u: %s\n", id++, p.c_str());
	if (ignore_file_rules.size()) {
		printf("Ignoring the following files:\n");
		id = 0;
		for (auto &ign : ignore_file_rules)
			printf("  %u: %s\n", id++, ign.c_str());
	}
	if (package_library_path.size()) {
		printf("Package-specific library paths:\n");
		id = 0;
		for (auto &iter : package_library_path) {
			printf("  %s:\n", iter.first.c_str());
			id = 0;
			for (auto &path : iter.second)
				printf("    %u: %s\n", id++, path.c_str());
		}
	}
}

bool
DB::is_broken(const Elf *obj) const
{
	return required_missing.find(const_cast<Elf*>(obj)) != required_missing.end();
}

bool
DB::is_broken(const Package *pkg) const
{
	for (auto &obj : pkg->objects) {
		if (is_broken(obj))
			return true;
	}
	return false;
}

void
DB::show_packages(bool filter_broken)
{
	if (opt_use_json)
		return show_packages_json(filter_broken);

	printf("Packages:%s\n", (filter_broken ? " (filter: 'broken')" : ""));
	for (auto &pkg : packages) {
		if (filter_broken && !is_broken(pkg))
			continue;
		printf("  -> %s - %s\n", pkg->name.c_str(), pkg->version.c_str());
		if (opt_verbosity >= 1) {
			for (auto &dep : pkg->depends)
				printf("    depends on: %s\n", dep.c_str());
			for (auto &dep : pkg->optdepends)
				printf("    depends optionally on: %s\n", dep.c_str());
			if (filter_broken) {
				for (auto &obj : pkg->objects) {
					if (is_broken(obj)) {
						printf("    broken: %s / %s\n", obj->dirname.c_str(), obj->basename.c_str());
						if (opt_verbosity >= 2) {
							auto list = required_missing.find(obj);
							for (auto &missing : list->second)
								printf("      misses: %s\n", missing.c_str());
						}
					}
				}
			}
			else {
				for (auto &obj : pkg->objects)
					printf("    contains %s / %s\n", obj->dirname.c_str(), obj->basename.c_str());
			}
		}
	}
}

void
DB::show_objects()
{
	if (opt_use_json)
		return show_objects_json();

	if (!objects.size()) {
		printf("Objects: none\n");
		return;
	}
	printf("Objects:\n");
	for (auto &obj : objects) {
		printf("  -> %s / %s\n", obj->dirname.c_str(), obj->basename.c_str());
		if (opt_verbosity < 1)
			continue;
		printf("     class: %u (%s)\n", (unsigned)obj->ei_class, obj->classString());
		printf("     data:  %u (%s)\n", (unsigned)obj->ei_data,  obj->dataString());
		printf("     osabi: %u (%s)\n", (unsigned)obj->ei_osabi, obj->osabiString());
		if (obj->rpath_set)
			printf("     rpath: %s\n", obj->rpath.c_str());
		if (obj->runpath_set)
			printf("     runpath: %s\n", obj->runpath.c_str());
		if (opt_verbosity < 2)
			continue;
		printf("     finds:\n"); {
			auto &set = required_found[obj];
			for (auto &found : set)
				printf("       -> %s / %s\n", found->dirname.c_str(), found->basename.c_str());
		}
		printf("     misses:\n"); {
			auto &set = required_missing[obj];
			for (auto &miss : set)
				printf("       -> %s\n", miss.c_str());
		}
	}
	printf("\n`found` entry set size: %lu\n",
	       (unsigned long)required_found.size());
	printf("`missing` entry set size: %lu\n",
	       (unsigned long)required_missing.size());
}

void
DB::show_missing()
{
	if (opt_use_json)
		return show_missing_json();

	if (!required_missing.size()) {
		printf("Missing: nothing\n");
		return;
	}
	printf("Missing:\n");
	for (auto &mis : required_missing) {
		Elf       *obj = mis.first;
		StringSet &set = mis.second;
		printf("  -> %s / %s\n", obj->dirname.c_str(), obj->basename.c_str());
		for (auto &s : set)
			printf("    misses: %s\n", s.c_str());
	}
}

void
DB::show_found()
{
	if (opt_use_json)
		return show_found_json();

	printf("Found:\n");
	for (auto &fnd : required_found) {
		Elf       *obj = fnd.first;
		ObjectSet &set = fnd.second;
		printf("  -> %s / %s\n", obj->dirname.c_str(), obj->basename.c_str());
		for (auto &s : set)
			printf("    finds: %s\n", s->basename.c_str());
	}
}
