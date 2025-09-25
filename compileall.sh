#!/bin/bash
set -e  # exit if any command fails

for file in *.c; do
    [ -e "$file" ] || continue  # skip if no .c files
    output="${file%.c}"
    echo "Compiling $file -> $output"
    gcc "$file" -o "$output" -lnuma
done
