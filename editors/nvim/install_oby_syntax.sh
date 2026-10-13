#!/usr/bin/env bash
set -e

# Resolve the directory where this script lives
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

NVIM_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/nvim"
VIM_DIR="$HOME/.vim"

installed=0

# Install for Neovim if directory or binary exists
if [ -d "$NVIM_DIR" ] || command -v nvim >/dev/null 2>&1; then
    mkdir -p "$NVIM_DIR/ftdetect" "$NVIM_DIR/syntax"
    cp "$SCRIPT_DIR/ftdetect/oby.vim" "$NVIM_DIR/ftdetect/oby.vim"
    cp "$SCRIPT_DIR/syntax/oby.vim" "$NVIM_DIR/syntax/oby.vim"
    echo "syntax files successfully installed to: $NVIM_DIR"
    installed=1
fi

# Install for standard Vim if directory exists
if [ -d "$VIM_DIR" ] || [ "$installed" -eq 0 ]; then
    mkdir -p "$VIM_DIR/ftdetect" "$VIM_DIR/syntax"
    cp "$SCRIPT_DIR/ftdetect/oby.vim" "$VIM_DIR/ftdetect/oby.vim"
    cp "$SCRIPT_DIR/syntax/oby.vim" "$VIM_DIR/syntax/oby.vim"
    echo "syntax files successfully installed to: $VIM_DIR"
fi
