# Thin wrapper around the justfile. The recipes live there. Run `make help` to list the targets.

.PHONY: help dev release test asan tidy spell check

help:
	@just --list

dev:
	just build

release:
	just release

test:
	just test

asan:
	just asan

# Pass files with FILES, for example: make tidy FILES="client.c main.c"
tidy:
	just tidy $(FILES)

spell:
	just spell

check:
	just check
