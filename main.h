#ifndef WDEPTRACK_MAIN_H__
#define WDEPTRACK_MAIN_H__

#include <memory>
#include <string>
#include <vector>

#include <elf.h>

#ifndef READPKGELF_V_MAJ
# error "READPKGELF_V_MAJ not defined"
#endif
#ifndef READPKGELF_V_MIN
# error "READPKGELF_V_MIN not defined"
#endif
#ifndef READPKGELF_V_PAT
# error "READPKGELF_V_PAT not defined"
#endif

#define RPKG_IND_STRING2(x) #x
#define RPKG_IND_STRING(x) RPKG_IND_STRING2(x)
#define VERSION_STRING \
	RPKG_IND_STRING(READPKGELF_V_MAJ) "." \
	RPKG_IND_STRING(READPKGELF_V_MIN) "." \
	RPKG_IND_STRING(READPKGELF_V_PAT)

#ifdef GITINFO
# define FULL_VERSION_STRING \
    VERSION_STRING "-git: " GITINFO
#else
# define FULL_VERSION_STRING VERSION_STRING
#endif

enum {
	Debug,
	Warn,
	Error
};

void log(int level, const char *msg, ...);

/// Package class
/// reads a package archive, extracts information
class Package {
public:
	Package();
	Package(const std::string& path);

public:
	/// Represents an object file
	class Object {
	public:
		Object(const std::string& filepath);

	public:
		std::string              path;
		std::vector<std::string> need;
		std::string              rpath;
	};

public:
	inline operator bool()  { return !error; }
	inline bool operator!() { return error; }

	void show_needed();

private:
	bool add_entry  (struct archive *tar, struct archive_entry *entry);
	bool care_about (struct archive_entry *entry) const;
	bool read_object(struct archive *tar, const std::string &filename, size_t size);
	bool read_info  (struct archive *tar, size_t size);

public:
	std::string         name;
	std::vector<std::shared_ptr<Object> > objects;

private:
	bool error;
};

class Elf {
protected:
	Elf(const char *data, size_t size);
public:
	static Elf* open(const char *data, size_t size);

public:
	inline operator bool() const  { return !error; }
	inline bool operator!() const { return error;  }

public:
	std::string              rpath;
	std::string              runpath;
	std::vector<std::string> needed;

protected:
	bool        error;
	const char *data;
	size_t      size;

	unsigned char *elf_ident;
	unsigned char  ei_class;
	unsigned char  ei_data;
	unsigned char  ei_version;
	unsigned char  ei_osabi;
	unsigned char  ei_abiversion;

	// type identification - for when we start the database stuff
	unsigned int   db_ident;
};

#endif
