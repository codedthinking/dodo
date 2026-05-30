# Git Internals Research Notes

## Purpose

Research notes on Git's internal data structures and algorithms, based on the [Git Book, Chapter 10](https://git-scm.com/book/en/v2/Git-Internals-Git-Objects). These notes inform long-range planning for version-controlled data workflows.

## Core Concept: Content-Addressable Storage

Git is a content-addressable filesystem. Every piece of content is identified by the SHA-1 hash of its content plus a header. This means:

- Identical content always produces the same hash (deduplication for free).
- Objects are immutable: changing content creates a new object with a new hash.
- Any corruption is detectable by recomputing the hash.

Object storage format: `<type> <size>\0<content>`, compressed with zlib. Objects are stored in `.git/objects/<first-2-chars>/<remaining-38-chars>`.

## The Four Object Types

### 1. Blob

Stores raw file content. No filename, no metadata -- just bytes.

```
blob <size>\0<content>
```

Two files with identical content share the same blob object, regardless of filename or location.

### 2. Tree

Stores a directory listing: a mapping from names to blobs or subtrees. Each entry has:

| Field | Example | Meaning |
|-------|---------|---------|
| mode  | `100644` | regular file |
| mode  | `100755` | executable |
| mode  | `040000` | subdirectory (tree) |
| mode  | `120000` | symlink |
| type  | `blob` or `tree` | object type |
| SHA-1 | 40-char hex | pointer to the object |
| name  | `README` | filename or directory name |

Example:
```
100644 blob a906cb2a...  README
100644 blob 8f941393...  Rakefile
040000 tree 99f1a6d1...  lib
```

Trees are recursive: a tree can point to subtrees, forming a full directory hierarchy.

### 3. Commit

Points to a single root tree (the project snapshot) and records metadata:

```
tree <SHA-1>
parent <SHA-1>          (zero for initial, one normally, multiple for merges)
author <name> <email> <timestamp> <tz>
committer <name> <email> <timestamp> <tz>

<commit message>
```

The parent pointer(s) form the history chain. A merge commit has two or more parents.

### 4. Tag (annotated)

Points to any object (usually a commit) with metadata:

```
object <SHA-1>
type commit
tag v1.0
tagger <name> <email> <timestamp> <tz>

<tag message>
```

Lightweight tags are just refs (see below), not objects.

## The DAG (Directed Acyclic Graph)

Commits form a DAG through their parent pointers:

```
c1 <-- c2 <-- c3       (linear history)

c1 <-- c2 <-- c4       (branch + merge)
        \      ^
         c3 --/
```

Properties of the DAG:
- **Directed**: edges point from child to parent (backward in time).
- **Acyclic**: no commit can be its own ancestor.
- **Reachability**: a commit is "reachable" if you can walk parent pointers to it from some ref.
- **Unreachable objects** are garbage collected.

## Merkle Tree Properties

Git's object graph is a Merkle tree (hash tree):

1. Each blob is hashed from its content.
2. Each tree is hashed from its entries (which include child hashes).
3. Each commit is hashed from its tree hash, parent hashes, and metadata.

This means:
- **Any change propagates upward**: modifying a single file changes its blob hash, which changes the tree hash, which changes the commit hash.
- **Integrity verification**: to verify a full snapshot, you only need to verify the root commit hash. If it matches, every object in the graph is guaranteed intact.
- **Efficient comparison**: two trees with the same hash are identical. Diff only needs to recurse into subtrees with different hashes.

## References (Refs)

Refs are human-readable pointers stored as plain text files containing a SHA-1 hash.

### Ref types

| Type | Location | Content | Mutable? | Purpose |
|------|----------|---------|----------|---------|
| Branch | `refs/heads/<name>` | commit SHA-1 | yes, advances on commit | current line of work |
| Lightweight tag | `refs/tags/<name>` | commit SHA-1 | no (by convention) | named snapshot |
| Annotated tag | `refs/tags/<name>` | tag object SHA-1 | no | named snapshot + metadata |
| Remote | `refs/remotes/<remote>/<branch>` | commit SHA-1 | read-only bookmark | last known remote state |
| HEAD | `.git/HEAD` | `ref: refs/heads/<name>` or SHA-1 | yes | current checkout |

### HEAD

HEAD is a symbolic reference: it usually contains `ref: refs/heads/master`, not a SHA-1 directly. When HEAD contains a raw SHA-1, the repo is in "detached HEAD" state.

On commit, Git:
1. Reads HEAD to find the current branch ref.
2. Reads that ref to find the parent commit SHA-1.
3. Creates a new commit object with that parent.
4. Updates the branch ref to point to the new commit.

### Branching

A branch is just a 41-byte file (40-char SHA-1 + newline). Creating a branch = writing a new ref file. Switching branches = updating HEAD to point to the new ref. This is why branching in Git is nearly instant regardless of repository size.

## Objects vs Refs: Summary

| | Objects | Refs |
|---|---------|------|
| Identity | content hash (SHA-1) | human-chosen name |
| Storage | `.git/objects/` (compressed) | `.git/refs/` (plain text) |
| Mutability | immutable | mutable (branches move) |
| Purpose | store data and structure | name entry points into the DAG |
| Types | blob, tree, commit, tag | branch, tag, remote, HEAD |

The object store is the "database." Refs are the "index" into it.

## Key Architectural Insights

1. **Snapshots, not diffs**: each commit stores a complete tree, not a delta from the previous state. Storage efficiency comes from deduplication (shared blobs) and pack files, not from the data model itself.

2. **Names live outside the object graph**: filenames are stored in tree objects, not in blobs. The same blob can appear under different names in different trees.

3. **History is separate from content**: the commit graph (history) and the tree/blob graph (content) are orthogonal. You can have the same content snapshot pointed to by different commits with different histories.

4. **Branches are cheap metadata**: they are not copies of anything. They are 41-byte files that move forward as commits are added. This is fundamentally different from systems where branches copy the file tree.

5. **Merge is a graph operation**: a merge commit has multiple parents, joining two lines of the DAG. The content resolution (conflict handling) is separate from the structural operation of creating a multi-parent commit.
