#!/usr/bin/env python3
"""MANUAL.md -> build/manual/manual.html (then printed to PDF by headless Chromium)."""
import markdown, re, os, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
md = open(os.path.join(root, "MANUAL.md"), encoding="utf-8").read()
# python-markdown needs a blank line before a list that follows a paragraph
lines, out = md.split("\n"), []
for i, l in enumerate(lines):
    if re.match(r"^\s*([-*]|\d+\.)\s", l) and i and out and out[-1].strip() and not re.match(r"^\s*([-*]|\d+\.)\s", out[-1]) and not out[-1].startswith("|"):
        out.append("")
    out.append(l)
html = markdown.markdown("\n".join(out), extensions=["tables", "fenced_code", "sane_lists"])
html = html.replace('src="docs/', f'src="file://{root}/docs/')
css = open(os.path.join(root, "scripts", "manual.css"), encoding="utf-8").read()
os.makedirs(os.path.join(root, "build", "manual"), exist_ok=True)
open(os.path.join(root, "build", "manual", "manual.html"), "w", encoding="utf-8").write(
    f'<!doctype html><html><head><meta charset="utf-8"><title>Sculpt Manual</title><style>{css}</style></head><body>{html}</body></html>')
print("build/manual/manual.html")
