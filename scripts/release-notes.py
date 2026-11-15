#!/usr/bin/env python3
"""Extract one exact, nonempty version section from a Keep a Changelog file."""

import argparse
from pathlib import Path
import re
import sys


RELEASE_HEADING = re.compile(r"^## \[([^\]]+)\](?:[ \t].*)?$")


def extract_notes(changelog, tag):
    if not tag or tag.casefold() == "unreleased":
        raise ValueError("release tags must name a version, not Unreleased")

    lines = changelog.splitlines()
    matches = [index for index, line in enumerate(lines)
               if (match := RELEASE_HEADING.fullmatch(line)) and match[1] == tag]
    if not matches:
        raise ValueError(f"no changelog section for tag {tag!r}; add its version heading before tagging")
    if len(matches) != 1:
        raise ValueError(f"duplicate changelog sections for tag {tag!r}; keep only one")

    start = matches[0] + 1
    end = next((index for index in range(start, len(lines))
                if lines[index].startswith("## ")), len(lines))
    while start < end and not lines[start].strip():
        start += 1
    while end > start and not lines[end - 1].strip():
        end -= 1
    body = lines[start:end]
    if not any(line.strip() and not line.lstrip().startswith("#") for line in body):
        raise ValueError(f"changelog section for tag {tag!r} is empty; add release notes before tagging")
    return "\n".join(body) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="tag matching a changelog version heading exactly")
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    args = parser.parse_args(argv)
    try:
        notes = extract_notes(args.changelog.read_text(encoding="utf-8-sig"), args.tag)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    sys.stdout.write(notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
