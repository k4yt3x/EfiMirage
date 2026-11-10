set positional-arguments

# Show the available tasks.
default:
    @just --list

# Fetch pinned dependencies and prepare the build tools (network access).
bootstrap:
    @scripts/workspace.sh bootstrap

# Check host tools, dependency revisions, and build prerequisites (offline).
doctor:
    @scripts/workspace.sh doctor

# Check script syntax, justfile formatting, and build-pipeline regression tests.
check:
    @scripts/workspace.sh check

# Check, build both components, verify, and publish a complete bundle.
build profile="release":
    @scripts/workspace.sh build "$1"

# Build only the EFI images into out/ (release, debug, or noopt).
efi profile="release":
    @scripts/workspace.sh efi "$1"

# Build only the Windows console tool into out/.
windows profile="release":
    @scripts/workspace.sh windows "$1"

# Build the upstream native-subsystem variant; never publish it.
native profile="release":
    @scripts/workspace.sh native "$1"

# Verify the published bundle's PE headers and SHA-256 checksums.
verify profile="release":
    @scripts/workspace.sh verify "$1"

# Delete out/ and dist/, retaining downloaded dependencies and source edits.
[confirm("Remove all generated files in out/ and dist/?")]
clean:
    @scripts/workspace.sh clean
