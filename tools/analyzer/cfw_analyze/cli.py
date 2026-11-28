"""Command-line interface for the cfw static analyzer."""

from __future__ import annotations

import argparse
import json
import os
import sys

from .driver import scan


def build_config(args):
    config = {}
    mapping = {
        "alloc": args.alloc_fn,
        "calloc": args.calloc_fn,
        "realloc": args.realloc_fn,
        "free": args.free_fn,
        "borrow": args.borrow_fn,
    }
    for key, vals in mapping.items():
        if vals:
            config[key] = vals
    if args.out_param:
        # NAME:ARGIDX
        out = {}
        for spec in args.out_param:
            if ":" in spec:
                name, idx = spec.rsplit(":", 1)
                try:
                    out[name] = int(idx)
                except ValueError:
                    print(f"warning: ignoring bad --out-param {spec!r}",
                          file=sys.stderr)
            else:
                out[spec] = 2
        config["out_param"] = out
    if args.define:
        config["defines"] = list(args.define)
    return config


def format_human(result):
    lines = []
    for f in result.findings:
        loc = f"{f.file}:{f.line}"
        lines.append(f"{loc}: {f.rule} [{f.severity}/{f.confidence}] "
                     f"{f.message}")
        if f.trace:
            lines.append(f"    trace: {f.trace}")
    lines.append("")
    lines.append(
        f"scanned {len(result.files)} file(s); "
        f"{result.functions_analyzed} function(s) analyzed, "
        f"{result.functions_partial} partially/unsupported, "
        f"{result.files_skipped} path(s) skipped; "
        f"{len(result.findings)} finding(s)")
    for fr in result.files:
        if fr.error:
            lines.append(f"  error {fr.path}: {fr.error}")
        elif fr.partial:
            lines.append(f"  partial {fr.path}: {fr.partial} function(s)")
    return "\n".join(lines)


def format_json(result):
    return json.dumps({
        "findings": [
            {"rule": f.rule, "severity": f.severity,
             "confidence": f.confidence, "file": f.file, "line": f.line,
             "col": f.col, "message": f.message, "trace": f.trace}
            for f in result.findings],
        "summary": {
            "files": len(result.files),
            "functions_analyzed": result.functions_analyzed,
            "functions_partial": result.functions_partial,
            "paths_skipped": result.files_skipped,
            "findings": len(result.findings),
            "file_errors": [
                {"file": fr.path, "error": fr.error}
                for fr in result.files if fr.error],
        },
    }, indent=2)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="cfw-analyze",
        description="Conservative static analyzer for memory-safety bugs "
                    "in C11 code. Reads source text only; never compiles, "
                    "links, or executes the analyzed project.")
    ap.add_argument("paths", nargs="+",
                    help="C source files or directories to scan")
    ap.add_argument("-r", "--recursive", action="store_true", default=True,
                    help="recurse into directories (default)")
    ap.add_argument("--no-recursive", dest="recursive",
                    action="store_false")
    ap.add_argument("--exclude", action="append", default=[],
                    metavar="NAME",
                    help="additional directory/file name to exclude "
                         "(repeatable)")
    ap.add_argument("--include", action="append", default=[],
                    metavar="GLOB",
                    help="only scan files matching this suffix/glob "
                         "(repeatable)")
    ap.add_argument("--include-generated", action="store_true",
                    help="also read generated CMake headers under "
                         "build/*/include/cfw for macro constants")
    ap.add_argument("--json", action="store_true",
                    help="emit JSON instead of human-readable output")
    ap.add_argument("--exit-zero", action="store_true",
                    help="always exit 0 even when findings are present")
    ap.add_argument("--exit-status", type=int, default=1, metavar="N",
                    help="exit status when findings are present (default 1)")
    ap.add_argument("--alloc-fn", action="append", default=[],
                    metavar="NAME",
                    help="treat NAME as an allocating function")
    ap.add_argument("--calloc-fn", action="append", default=[],
                    metavar="NAME",
                    help="treat NAME as a calloc-style allocator")
    ap.add_argument("--realloc-fn", action="append", default=[],
                    metavar="NAME",
                    help="treat NAME as realloc-style")
    ap.add_argument("--free-fn", action="append", default=[],
                    metavar="NAME",
                    help="treat NAME as a release/deallocator taking the "
                         "pointer as first argument")
    ap.add_argument("--borrow-fn", action="append", default=[],
                    metavar="NAME",
                    help="treat NAME as borrowing pointer arguments "
                         "(no ownership transfer, no escape)")
    ap.add_argument("--out-param", action="append", default=[],
                    metavar="NAME[:IDX]",
                    help="treat NAME as an allocator that writes the result "
                         "through a pointer argument (default index 2)")
    ap.add_argument("--define", action="append", default=[],
                    metavar="NAME[=VALUE]",
                    help="pretend NAME is defined for preprocessor "
                         "conditionals (repeatable; VALUE may be numeric)")
    args = ap.parse_args(argv)

    config = build_config(args)
    excludes = None
    if args.exclude:
        from .driver import DEFAULT_EXCLUDES
        excludes = set(DEFAULT_EXCLUDES) | set(args.exclude)

    result = scan(args.paths, config=config, recursive=args.recursive,
                  excludes=excludes,
                  include_generated=args.include_generated)

    if args.json:
        print(format_json(result))
    else:
        print(format_human(result))

    if result.findings and not args.exit_zero:
        return args.exit_status
    return 0
