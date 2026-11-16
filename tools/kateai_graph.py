#!/usr/bin/env python3
"""Generate .kateai/project_graph.json for the Kate AI project tools.

Kate AI regenerates this cache when it opens a workspace, but its built-in scan
stops at 300 nodes and visits directories in name order, so a CMake build tree
beside the sources fills the graph before src/ or docs/ is reached. This writer
uses the same node/edge schema as Kate AI's ProjectGraph, skips every build
directory, and spends the 300-node budget on the source, tools, tests and docs.

Usage: tools/kateai_graph.py [workspace] [output]
"""
import hashlib
import json
import os
import sys
from pathlib import Path

MAX_NODES = 300
PREVIEW = 16 * 1024
MAX_FILE = 64 * 1024
BINARY = {
    'o', 'so', 'a', 'dll', 'exe', 'bin', 'obj', 'lib', 'png', 'jpg', 'jpeg',
    'gif', 'ico', 'bmp', 'webp', 'pdf', 'zip', 'tar', 'gz', 'xz', 'bz2', '7z',
    'class', 'jar', 'pyc', 'wasm', 'db', 'sqlite', 'mp3', 'mp4', 'avi', 'mov',
    'wav', 'woff', 'woff2', 'ttf', 'metallib', 'spv', 'nsp', 'nup', 'nca',
}
# A node budget per tree rather than first-come-first-served: dm/host alone can
# fill Kate AI's 300-node graph before scripts/ or docs/ is reached. The full
# dm/games profile tree is too large for the budget; its README and the games
# table are enough for code queries.
TREES = [
    ('src', 70),
    ('dm/host', 45),
    ('dm/gfx/nvn', 40),
    ('dm/gfx/tools', 25),
    ('dm/gfx/vk', 5),
    ('dm/gfx/gl', 5),
    ('dm/gfx/tex', 5),
    ('dm/tools', 25),
    ('scripts', 20),
    ('tests', 20),
    ('docs', 30),
    ('tools', 10),
    ('packaging', 10),
    ('dm/games/README.md', 4),
    ('dm/third_party/README.md', 4),
]
LANG = {
    'c': 'cpp', 'h': 'header', 'cc': 'cpp', 'cpp': 'cpp', 'cxx': 'cpp',
    'hpp': 'header', 'hxx': 'header', 'm': 'objective-c', 'mm': 'objective-cpp',
    'py': 'python', 'sh': 'shell', 'bash': 'shell', 'js': 'javascript',
    'ts': 'typescript', 'tsx': 'tsx', 'jsx': 'jsx', 'json': 'json',
    'md': 'markdown', 'txt': 'text', 'yml': 'yaml', 'yaml': 'yaml',
    'toml': 'toml', 'cmake': 'cmake', 'rs': 'rust', 'swift': 'swift',
    'S': 'asm', 's': 'asm', 'tsv': 'tsv', 'xml': 'xml', 'css': 'css',
    'html': 'html', 'bat': 'batch', 'ps1': 'powershell',
}


def node_id(path: str, kind: str) -> str:
    return hashlib.sha256(path.encode()).hexdigest() + '_' + kind


def skip_dir(name: str) -> bool:
    return (name.startswith('.') or name in {
        'node_modules', '__pycache__', 'dist', 'target', 'venv', 'out',
    } or name == 'build' or name.startswith(('build-', 'build_', 'cmake-build-')))


def binary(path: Path) -> bool:
    if path.suffix.lower().lstrip('.') in BINARY:
        return True
    try:
        with path.open('rb') as f:
            return b'\0' in f.read(1024)
    except OSError:
        return True


class Graph:
    def __init__(self, root: Path):
        self.root = root
        self.nodes = []
        self.edges = []
        self.ids = set()
        self.limit = MAX_NODES

    def add(self, path: Path, kind: str, parent: str | None) -> str:
        sid = node_id(str(path), kind)
        if sid in self.ids:
            return sid
        if len(self.nodes) >= self.limit:
            return ''
        self.ids.add(sid)
        node = {
            'id': sid,
            'name': path.name if parent is not None else (self.root.name or str(self.root)),
            'type': kind,
            'path': str(path),
            'content': '',
            'dependencies': [],
            'dependents': [],
            'complexity': 0,
            'linesOfCode': 0,
            'isReadOnly': False,
            'lastModified': int(path.stat().st_mtime) if path.exists() else 0,
            'metadata': {},
        }
        if kind == 'file':
            try:
                if path.stat().st_size > MAX_FILE or binary(path):
                    return ''
                text = path.read_text(errors='replace')[:PREVIEW]
            except OSError:
                return ''
            node['content'] = text
            node['linesOfCode'] = text.count('\n') + (1 if text else 0)
            node['metadata']['language'] = LANG.get(path.suffix.lower().lstrip('.'), 'text')
        self.nodes.append(node)
        if parent:
            self.edges.append({
                'sourceId': parent, 'targetId': sid, 'relationship': 'contains',
                'details': '', 'isSafe': True,
            })
        return sid

    def ensure_dir(self, path: Path) -> str:
        """Add path and its missing ancestors below the workspace root."""
        if path == self.root:
            return node_id(str(self.root), 'directory')
        parent = self.ensure_dir(path.parent) if path.parent != self.root else node_id(str(self.root), 'directory')
        return self.add(path, 'directory', parent) if parent else ''

    def tree(self, path: Path) -> None:
        if len(self.nodes) >= self.limit:
            return
        parent = self.ensure_dir(path) if path.is_dir() else self.ensure_dir(path.parent)
        if not parent:
            return
        if path.is_file():
            self.add(path, 'file', parent)
            return
        try:
            entries = sorted(path.iterdir(), key=lambda p: p.name)
        except OSError:
            return
        for entry in entries:
            if len(self.nodes) >= self.limit:
                return
            if entry.is_dir():
                if skip_dir(entry.name):
                    continue
                sid = self.add(entry, 'directory', parent)
                if sid:
                    self._walk(entry, sid)
            elif entry.is_file():
                self.add(entry, 'file', parent)

    def _walk(self, directory: Path, parent: str) -> None:
        try:
            entries = sorted(directory.iterdir(), key=lambda p: p.name)
        except OSError:
            return
        for entry in entries:
            if len(self.nodes) >= self.limit:
                return
            if entry.is_dir():
                if skip_dir(entry.name):
                    continue
                sid = self.add(entry, 'directory', parent)
                if sid:
                    self._walk(entry, sid)
            elif entry.is_file():
                self.add(entry, 'file', parent)


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else '.').resolve()
    out = Path(argv[2] if len(argv) > 2 else root / '.kateai/project_graph.json')
    if not root.is_dir():
        print(f'not a directory: {root}', file=sys.stderr)
        return 2

    graph = Graph(root)
    root_id = graph.add(root, 'directory', None)
    # Top-level files first (README, CMakeLists, license, ...), then the trees.
    for entry in sorted(root.iterdir(), key=lambda p: p.name):
        if entry.is_file() and not entry.name.startswith('.'):
            graph.add(entry, 'file', root_id)
    for rel, quota in TREES:
        path = root / rel
        if path.exists():
            graph.limit = min(MAX_NODES, len(graph.nodes) + quota)
            graph.tree(path)
    graph.limit = MAX_NODES

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({'nodes': graph.nodes, 'edges': graph.edges}, indent=4) + '\n')
    print(f'{out}: {len(graph.nodes)} nodes, {len(graph.edges)} edges')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
