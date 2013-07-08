#include <stdio.h>

#include "main.h"

static void
json_in_quote(FILE *out, const std::string& str)
{
	for (size_t i = 0; i != str.length(); ++i) {
		switch (str[i]) {
			case '"':  fputc('\\', out); fputc('"', out); break;
			case '\\': fputc('\\', out); fputc('\\', out); break;
			case '\b': fputc('\\', out); fputc('b', out); break;
			case '\f': fputc('\\', out); fputc('f', out); break;
			case '\n': fputc('\\', out); fputc('n', out); break;
			case '\r': fputc('\\', out); fputc('r', out); break;
			case '\t': fputc('\\', out); fputc('t', out); break;
			default:
				fputc(str[i], out);
				break;
		}
	}
}

static void
json_quote(FILE *out, const std::string& str)
{
	fprintf(out, "\"");
	json_in_quote(out, str);
	fprintf(out, "\"");
}

static void
print_objname(const Elf *obj)
{
	putchar('"');
	json_in_quote(stdout, obj->dirname);
	putchar('/');
	json_in_quote(stdout, obj->basename);
	putchar('"');
}

void
DB::show_found_json()
{
	printf("{ \"found_objects\": {");
	const char *mainsep = "\n\t";
	for (auto &fnd : required_found) {
		const Elf *obj = fnd.first;
		printf("%s", mainsep); mainsep = ",\n\t";
		print_objname(obj);
		printf(": [");

		ObjectSet &set = fnd.second;
		const char *sep = "\n\t\t";
		for (auto &s : set) {
			printf("%s", sep); sep = ",\n\t\t";
			json_quote(stdout, s->basename);
		}
		printf("\n\t]");
	}
	printf("\n} }\n");
}

void
DB::show_missing_json()
{
	printf("{ \"missing_objects\": {");
	const char *mainsep = "\n\t";
	for (auto &mis : required_missing) {
		const Elf *obj = mis.first;
		printf("%s", mainsep); mainsep = ",\n\t";
		print_objname(obj);
		printf(": [");

		StringSet &set = mis.second;
		const char *sep = "\n\t\t";
		for (auto &s : set) {
			printf("%s", sep); sep = ",\n\t\t";
			json_quote(stdout, s);
		}
		printf("\n\t]");
	}
	printf("\n} }\n");
}

static void
json_obj(size_t id, FILE *out, const Elf *obj)
{
	fprintf(out, "\n\t\t{\n"
	             "\t\t\t\"id\": %lu", (unsigned long)id);

	fprintf(out, ",\n\t\t\t\"dirname\": ");
	json_quote(out, obj->dirname);
	fprintf(out, ",\n\t\t\t\"basename\": ");
	json_quote(out, obj->basename);
	fprintf(out, ",\n\t\t\t\"ei_class\": %u", (unsigned)obj->ei_class);
	fprintf(out, ",\n\t\t\t\"ei_data\":  %u", (unsigned)obj->ei_data);
	fprintf(out, ",\n\t\t\t\"ei_osabi\": %u", (unsigned)obj->ei_osabi);
	if (obj->rpath_set) {
		fprintf(out, ",\n\t\t\t\"rpath\": ");
		json_quote(out, obj->rpath);
	}
	if (obj->runpath_set) {
		fprintf(out, ",\n\t\t\t\"runpath\": ");
		json_quote(out, obj->runpath);
	}
	if (obj->needed.size()) {
		fprintf(out, ",\n\t\t\t\"needed\": [");
		bool comma = false;
		for (auto &need : obj->needed) {
			if (comma) fputc(',', out);
			comma = true;
			fprintf(out, "\n\t\t\t\t");
			json_quote(out, need);
		}
		fprintf(out, "\n\t\t\t]");
	}

	fprintf(out, "\n\t\t}");
}

template<class OBJLIST>
static void
json_objlist(FILE *out, const OBJLIST &list)
{
	if (!list.size())
		return;
	// let's group them...
	size_t i = 0;
	size_t count = list.size()-1;
	auto iter = list.begin();
	for (; i != count; ++i, ++iter) {
		if ((i & 0xF) == 0)
			fprintf(out, "\n\t\t\t\t");
		fprintf(out, "%lu, ", (*iter)->json.id);
	}
	if ((i & 0xF) == 0)
		fprintf(out, "%s\n\t\t\t\t", (i ? "" : ","));
	fprintf(out, "%lu", (*iter)->json.id);
}

template<class STRLIST>
static void
json_strlist(FILE *out, const STRLIST &list)
{
	bool comma = false;
	for (auto &i : list) {
		if (comma) fputc(',', out);
		comma = true;
		fprintf(out, "\n\t\t\t\t");
		json_quote(out, i);
	}
}

static void
json_pkg(FILE *out, const Package *pkg)
{
	fprintf(out, "\n\t\t{");
	const char *sep = "\n";
	if (pkg->name.size()) {
		fprintf(out, "%s\t\t\t\"name\": ", sep);
		json_quote(out, pkg->name);
		sep = ",\n";
	}
	if (pkg->version.size()) {
		fprintf(out, "%s\t\t\t\"version\": ", sep);
		json_quote(out, pkg->version);
		sep = ",\n";
	}
	if (pkg->objects.size()) {
		fprintf(out, "%s\t\t\t\"objects\": [", sep);
		json_objlist(out, pkg->objects);
		fprintf(out, "\n\t\t\t]");
		sep = ",\n";
	}

	fprintf(out, "\n\t\t}");
}

static void
json_obj_found(FILE *out, const Elf *obj, const ObjectSet &found)
{
	fprintf(out, "\n\t\t{"
	             "\n\t\t\t\"obj\": %lu"
	             "\n\t\t\t\"found\": [",
	        (unsigned long)obj->json.id);
	json_objlist(out, found);
	fprintf(out, "\n\t\t\t]"
	             "\n\t\t}");
}

static void
json_obj_missing(FILE *out, const Elf *obj, const StringSet &missing)
{
	fprintf(out, "\n\t\t{"
	             "\n\t\t\t\"obj\": %lu"
	             "\n\t\t\t\"missing\": [",
	        (unsigned long)obj->json.id);
	json_strlist(out, missing);
	fprintf(out, "\n\t\t\t]"
	             "\n\t\t}");
}

bool
db_store_json(DB *db, const std::string& filename)
{
	FILE *out = fopen(filename.c_str(), "wb");
	if (!out) {
		log(Error, "failed to open file `%s' for reading\n", filename.c_str());
		return false;
	}

	log(Message, "writing json database file\n");

	guard close_out([out]() { fclose(out); });

	// we put the objects first as they don't directly depend on anything
	size_t id = 0;
	fprintf(out, "{\n"
	             "\t\"objects\": [");
	bool comma = false;
	for (auto &obj : db->objects) {
		if (comma) fputc(',', out);
		comma = true;
		obj->json.id = id;
		json_obj(id, out, obj);
		++id;
	}
	fprintf(out, "\n\t],\n"
	             "\t\"packages\": [");

	// packages have a list of objects
	// above we numbered them with IDs to reuse here now
	comma = false;
	for (auto &pkg : db->packages) {
		if (comma) fputc(',', out);
		comma = true;
		json_pkg(out, pkg);
	}

	fprintf(out, "\n\t]");

	if (!db->required_found.empty()) {
		fprintf(out, ",\n\t\"found\": [");
		comma = false;
		for (auto &found : db->required_found) {
			if (comma) fputc(',', out);
			comma = true;
			json_obj_found(out, found.first, found.second);
		}
		fprintf(out, "\n\t]");
	}


	if (!db->required_missing.empty()) {
		fprintf(out, ",\n\t\"missing\": [");
		comma = false;
		for (auto &mis : db->required_missing) {
			if (comma) fputc(',', out);
			comma = true;
			json_obj_missing(out, mis.first, mis.second);
		}
		fprintf(out, "\n\t]");
	}

	fprintf(out, "\n}\n");
	return true;
}
