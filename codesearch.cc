#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <google/dense_hash_set>

#include <locale>
#include <list>
#include <iostream>
#include <string>

#include <re2/re2.h>

#include "smart_git.h"
#include "timer.h"

using google::dense_hash_set;
using re2::RE2;
using re2::StringPiece;
using namespace std;

#define CHUNK_SIZE (1 << 20)

struct search_file {
    string path;
    const char *ref;
    git_oid oid;
};

struct chunk_file {
    search_file *file;
    unsigned int left;
    unsigned int right;
};

#define CHUNK_MAGIC 0xC407FADE

struct chunk {
    int size;
    unsigned magic;
    vector<chunk_file> files;
    char data[0];

    chunk()
        : size(0), magic(CHUNK_MAGIC), files() {
    }

    chunk_file &get_chunk_file(search_file *sf, const char *p) {
        if (files.empty() || files.back().file != sf) {
            int off = p - data;
            files.push_back(chunk_file());
            chunk_file &cf = files.back();
            cf.file = sf;
            cf.left = cf.right = off;
        }
        return files.back();
    }
};

#define CHUNK_SPACE  (CHUNK_SIZE - (sizeof(chunk)))

chunk *alloc_chunk() {
    void *p;
    if (posix_memalign(&p, CHUNK_SIZE, CHUNK_SIZE) != 0)
        return NULL;
    return new(p) chunk;
};

class chunk_allocator {
public:
    chunk_allocator() : current_() {
        new_chunk();
    }

    char *alloc(size_t len) {
        assert(len < CHUNK_SPACE);
        if ((current_->size + len) > CHUNK_SPACE)
            new_chunk();
        char *out = current_->data + current_->size;
        current_->size += len;
        return out;
    }

    list<chunk*>::iterator begin () {
        return chunks_.begin();
    }

    typename list<chunk*>::iterator end () {
        return chunks_.end();
    }

    chunk *current_chunk() {
        return current_;
    }

protected:
    void new_chunk() {
        current_ = alloc_chunk();
        chunks_.push_back(current_);
    }

    list<chunk*> chunks_;
    chunk *current_;
};

/*
 * We special-case data() == NULL to provide an "empty" element for
 * dense_hash_set.
 *
 * StringPiece::operator== will consider a zero-length string equal to a
 * zero-length string with a NULL data().
 */
struct eqstr {
    bool operator()(const StringPiece& lhs, const StringPiece& rhs) const {
        if (lhs.data() == NULL && rhs.data() == NULL)
            return true;
        if (lhs.data() == NULL || rhs.data() == NULL)
            return false;
        return lhs == rhs;
    }
};

struct hashstr {
    locale loc;
    size_t operator()(const StringPiece &str) const {
        const collate<char> &coll = use_facet<collate<char> >(loc);
        return coll.hash(str.data(), str.data() + str.size());
    }
};

const StringPiece empty_string(NULL, 0);

typedef dense_hash_set<StringPiece, hashstr, eqstr> string_hash;

class code_counter {
public:
    code_counter(git_repository *repo)
        : repo_(repo), stats_()
    {
        lines_.set_empty_key(empty_string);
    }

    void walk_ref(const char *ref) {
        smart_object<git_commit> commit;
        smart_object<git_tree> tree;
        resolve_ref(commit, ref);
        git_commit_tree(tree, commit);

        walk_tree(ref, "", tree);
    }

    void dump_stats() {
        printf("Bytes: %ld (dedup: %ld)\n", stats_.bytes, stats_.dedup_bytes);
        printf("Lines: %ld (dedup: %ld)\n", stats_.lines, stats_.dedup_lines);
    }

    bool match(RE2& pat) {
        list<chunk*>::iterator it;
        StringPiece match;
        int matches = 0;

        for (it = alloc_.begin(); it != alloc_.end(); it++) {
            StringPiece str((*it)->data, (*it)->size);
            int pos = 0;
            while (pos < str.size()) {
                    if (!pat.Match(str, pos, str.size(), RE2::UNANCHORED, &match, 1))
                        break;
                    assert(memchr(match.data(), '\n', match.size()) == NULL);
                    StringPiece line = find_line(str, match);
                    printf("%.*s\n", line.size(), line.data());
                    print_files(line.data());
                    pos = line.size() + line.data() - str.data();
                    if (++matches == 10)
                        return true;
                }
        }
        return matches > 0;
    }
protected:
    void print_files (const char *p) {
        chunk *c = find_chunk(p);
        int off = p - c->data;
        for(vector<chunk_file>::iterator it = c->files.begin();
            it != c->files.end(); it++) {
            if (off >= it->left && off < it->right) {
                printf(" (%s:%s)\n", it->file->ref, it->file->path.c_str());
            }
        }
    }
    StringPiece find_line(const StringPiece& chunk, const StringPiece& match) {
        const char *start, *end;
        assert(match.data() >= chunk.data());
        assert(match.data() < chunk.data() + chunk.size());
        assert(match.size() < (chunk.size() - (match.data() - chunk.data())));
        start = static_cast<const char*>
            (memrchr(chunk.data(), '\n', match.data() - chunk.data()));
        if (start == NULL)
            start = chunk.data();
        else
            start++;
        end = static_cast<const char*>
            (memchr(match.data() + match.size(), '\n',
                    chunk.size() - (match.data() - chunk.data()) - match.size()));
        if (end == NULL)
            end = chunk.data() + chunk.size();
        return StringPiece(start, end - start);
    }

    void walk_tree(const char *ref, const string& pfx, git_tree *tree) {
        string path;
        int entries = git_tree_entrycount(tree);
        int i;
        for (i = 0; i < entries; i++) {
            const git_tree_entry *ent = git_tree_entry_byindex(tree, i);
            path = pfx + "/" + git_tree_entry_name(ent);
            smart_object<git_object> obj;
            git_tree_entry_2object(obj, repo_, ent);
            if (git_tree_entry_type(ent) == GIT_OBJ_TREE) {
                walk_tree(ref, path, obj);
            } else if (git_tree_entry_type(ent) == GIT_OBJ_BLOB) {
                update_stats(ref, path, obj);
            }
        }
    }

    chunk* find_chunk(const char *p) {
        chunk *out = reinterpret_cast<chunk*>
            (reinterpret_cast<uintptr_t>(p) & ~(CHUNK_SIZE - 1));
        assert(out->magic == CHUNK_MAGIC);
        return out;
    }

    void update_stats(const char *ref, const string& path, git_blob *blob) {
        size_t len = git_blob_rawsize(blob);
        const char *p = static_cast<const char*>(git_blob_rawcontent(blob));
        const char *end = p + len;
        const char *f;
        string_hash::iterator it;
        search_file *sf = new search_file;
        sf->path = path;
        sf->ref = ref;
        git_oid_cpy(&sf->oid, git_object_id(reinterpret_cast<git_object*>(blob)));
        chunk *c;
        const char *line;

        while ((f = static_cast<const char*>(memchr(p, '\n', end - p))) != 0) {
            it = lines_.find(StringPiece(p, f - p));
            if (it == lines_.end()) {
                stats_.dedup_bytes += (f - p) + 1;
                stats_.dedup_lines ++;

                // Include the trailing '\n' in the chunk buffer
                char *alloc = alloc_.alloc(f - p + 1);
                memcpy(alloc, p, f - p + 1);
                lines_.insert(StringPiece(alloc, f - p));
                c = alloc_.current_chunk();
                line = alloc;
            } else {
                line = it->data();
                c = find_chunk(line);
            }
            chunk_file &cf = c->get_chunk_file(sf, line);
            cf.left = min(static_cast<long>(cf.left), p - blob_data);
            cf.right = max(static_cast<long>(cf.right), f - blob_data);
            assert(cf.left < CHUNK_SPACE);
            assert(cf.right < CHUNK_SPACE);
            p = f + 1;
            stats_.lines++;
        }

        stats_.bytes += len;
    }

    void resolve_ref(smart_object<git_commit> &out, const char *refname) {
        git_reference *ref;
        const git_oid *oid;
        git_oid tmp;
        smart_object<git_object> obj;
        if (git_oid_fromstr(&tmp, refname) == GIT_SUCCESS) {
            git_object_lookup(obj, repo_, &tmp, GIT_OBJ_ANY);
        } else {
            git_reference_lookup(&ref, repo_, refname);
            git_reference_resolve(&ref, ref);
            oid = git_reference_oid(ref);
            git_object_lookup(obj, repo_, oid, GIT_OBJ_ANY);
        }
        if (git_object_type(obj) == GIT_OBJ_TAG) {
            git_tag_target(out, obj);
        } else {
            out = obj.release();
        }
    }

    git_repository *repo_;
    string_hash lines_;
    struct {
        unsigned long bytes, dedup_bytes;
        unsigned long lines, dedup_lines;
    } stats_;
    chunk_allocator alloc_;
};

int main(int argc, char **argv) {
    git_repository *repo;
    git_repository_open(&repo, ".git");

    code_counter counter(repo);

    for (int i = 1; i < argc; i++) {
        timer tm;
        struct timeval elapsed;
        printf("Walking %s...", argv[i]);
        fflush(stdout);
        counter.walk_ref(argv[i]);
        elapsed = tm.elapsed();
        printf(" done in %d.%06ds\n",
               (int)elapsed.tv_sec, (int)elapsed.tv_usec);
    }
    counter.dump_stats();
    RE2::Options opts;
    opts.set_never_nl(true);
    opts.set_one_line(false);
    opts.set_posix_syntax(true);
    while (true) {
        printf("regex> ");
        string line;
        getline(cin, line);
        if (cin.eof())
            break;
        RE2 re(line, opts);
        if (re.ok()) {
            timer tm;
            struct timeval elapsed;
            if (!counter.match(re)) {
                printf("no match\n");
            }
            elapsed = tm.elapsed();
            printf("Match completed in %d.%06ds.\n",
                   (int)elapsed.tv_sec, (int)elapsed.tv_usec);
        }
    }

    return 0;
}