"""Scan driver: file discovery, two-pass typedef collection, per-function
analysis, and result aggregation."""

from __future__ import annotations

import glob
import os
from dataclasses import dataclass, field

from .analyze import FunctionAnalyzer, Finding
from .cfg import CFGBuilder
from .lexer import tokenize
from .parser import (collect_enum_constants, collect_macro_definitions,
                     collect_typedefs, filter_tokens, parse_file,
                     resolve_numeric_macros)

C_EXTENSIONS = (".c", ".h")

DEFAULT_EXCLUDES = {
    "build", "build-*", ".git", ".hg", ".svn", "node_modules", "vendor",
    "vendored", "third_party", "third-party", "out", ".venv", "venv",
    "__pycache__", ".idea", ".vscode", "cmake-build-*",
}


@dataclass
class FileResult:
    path: str
    functions: int = 0
    partial: int = 0
    skipped: str = ""
    error: str = ""


@dataclass
class ScanResult:
    findings: list = field(default_factory=list)
    files: list = field(default_factory=list)          # FileResult
    functions_analyzed: int = 0
    functions_partial: int = 0
    files_skipped: int = 0


def _excluded(path, excludes):
    parts = set(os.path.normpath(path).split(os.sep))
    return bool(parts & excludes)


def collect_files(paths, recursive=True, excludes=None, include=None):
    """Return (files, skipped_dirs). Paths may be files or directories."""
    excludes = set(excludes if excludes is not None else DEFAULT_EXCLUDES)
    files = []
    skipped = []
    for p in paths:
        if os.path.isfile(p):
            files.append(p)
        elif os.path.isdir(p):
            for root, dirs, names in os.walk(p):
                dirs[:] = sorted(d for d in dirs
                                 if d not in excludes
                                 and not _excluded(os.path.join(root, d),
                                                   excludes))
                for d in list(dirs):
                    if _excluded(os.path.join(root, d), excludes):
                        skipped.append(os.path.join(root, d))
                for name in sorted(names):
                    if not name.endswith(C_EXTENSIONS):
                        continue
                    fp = os.path.join(root, name)
                    if _excluded(fp, excludes):
                        skipped.append(fp)
                        continue
                    files.append(fp)
        else:
            skipped.append(p)
    # de-dup, stable order
    seen = set()
    out = []
    for f in files:
        ap = os.path.abspath(f)
        if ap not in seen:
            seen.add(ap)
            out.append(f)
    return out, skipped


def analyze_file(path, text, config):
    """Analyze one file's text. Returns (findings, n_funcs, n_partial,
    error_or_None)."""
    findings = set()
    funcs = parse_file(path, text, extra_typedefs=config.get("typedefs",
                                                            ()),
                       extra_macros=config.get("numeric_macros", ()),
                       extra_defines=config.get("defines", ()),
                       extra_enum_constants=config.get("enum_constants",
                                                       ()))
    n_partial = 0
    for func in funcs:
        builder = CFGBuilder(func)
        exit_id = builder.new_node(("exit", 0))
        try:
            entry, nodes = builder.build(func.body, exit_id)
        except Exception:
            n_partial += 1
            continue
        analyzer = FunctionAnalyzer(func, findings, config)
        analyzer.analyze(entry, nodes, exit_id,
                         loop_heads=builder.loop_heads)
        if builder.partial or func.partial:
            n_partial += 1
    return findings, len(funcs), n_partial, None


def scan(paths, config=None, recursive=True, excludes=None,
         collect_typedefs_first=True, include_generated=False):
    """Full scan. Returns ScanResult."""
    config = config or {}
    files, skipped = collect_files(paths, recursive=recursive,
                                   excludes=excludes)
    if include_generated:
        files.extend(sorted(glob.glob(
            os.path.join("build", "*", "include", "cfw", "*.h"))))
    result = ScanResult(files_skipped=len(skipped))

    # pass 1: collect typedef names and macro definitions across all files,
    # then resolve numeric constants once so transitive/cross-file references
    # work regardless of file order.
    typedefs = set()
    macro_definitions = {}
    sources = {}
    tokenized = {}
    for fp in files:
        try:
            with open(fp, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as e:
            result.files.append(FileResult(fp, error=str(e)))
            continue
        sources[fp] = text
        if collect_typedefs_first:
            try:
                tokens = tokenize(text)
                tokenized[fp] = tokens
                typedefs |= collect_typedefs(tokens)
                macro_definitions.update(collect_macro_definitions(tokens))
            except Exception:
                pass
    config = dict(config)
    config["typedefs"] = typedefs
    config["numeric_macros"] = resolve_numeric_macros(macro_definitions)
    if collect_typedefs_first:
        enum_constants = {}
        for fp, tokens in tokenized.items():
            try:
                active, _ = filter_tokens(
                    tokens, typedefs, config["numeric_macros"],
                    extra_defines=config.get("defines"))
                enum_constants.update(collect_enum_constants(
                    active, typedefs, config["numeric_macros"]))
            except Exception:
                pass
        config["enum_constants"] = enum_constants

    all_findings = set()
    for fp, text in sources.items():
        norm = fp.replace(os.sep, "/")
        if include_generated and norm.startswith("build/"):
            continue
        try:
            findings, nfunc, npart, err = analyze_file(fp, text, config)
        except Exception as e:
            result.files.append(FileResult(fp, error=f"{type(e).__name__}: {e}"))
            continue
        fr = FileResult(fp, functions=nfunc, partial=npart)
        result.files.append(fr)
        all_findings |= findings
        result.functions_analyzed += nfunc - npart
        result.functions_partial += npart
    result.findings = sorted(
        all_findings,
        key=lambda f: (f.file, f.line, f.col, f.rule, f.message))
    return result
