#!/usr/bin/env bash
# download_model.sh — download a quantized GGUF model for use with www
#
# All models are Q4_K_M quantization: a good balance of quality vs size.
# They are sourced from bartowski's HuggingFace repos, which are well-maintained
# quantizations of the original models.
#
# Usage: ./scripts/download_model.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODELS_DIR="$(dirname "$SCRIPT_DIR")/models"

# ---------------------------------------------------------------------------
# Model catalogue
# Each entry: "Display Name|filename|huggingface_repo|approx_size"
# ---------------------------------------------------------------------------
declare -a MODELS=(
    "Llama 3.1 8B Instruct (recommended, 4.9 GB)|Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf|bartowski/Meta-Llama-3.1-8B-Instruct-GGUF|4.9 GB"
    "Mistral 7B Instruct v0.3 (fast, 4.4 GB)|Mistral-7B-Instruct-v0.3-Q4_K_M.gguf|bartowski/Mistral-7B-Instruct-v0.3-GGUF|4.4 GB"
    "Qwen 2.5 7B Instruct (strong, 4.7 GB)|Qwen2.5-7B-Instruct-Q4_K_M.gguf|bartowski/Qwen2.5-7B-Instruct-GGUF|4.7 GB"
    "Llama 3.2 3B Instruct (lighter, 2.0 GB)|Llama-3.2-3B-Instruct-Q4_K_M.gguf|bartowski/Llama-3.2-3B-Instruct-GGUF|2.0 GB"
    "Qwen 2.5 1.5B Instruct (Pi-friendly, 1.0 GB)|Qwen2.5-1.5B-Instruct-Q4_K_M.gguf|bartowski/Qwen2.5-1.5B-Instruct-GGUF|1.0 GB"
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

print_banner() {
    echo ""
    echo "  ╔══════════════════════════════════════════╗"
    echo "  ║        www — model downloader            ║"
    echo "  ╚══════════════════════════════════════════╝"
    echo ""
}

check_downloader() {
    if command -v wget &>/dev/null; then
        echo "wget"
    elif command -v curl &>/dev/null; then
        echo "curl"
    else
        echo ""
    fi
}

download_file() {
    local url="$1"
    local dest="$2"
    local tool="$3"

    if [[ "$tool" == "wget" ]]; then
        wget --show-progress -O "$dest" "$url"
    else
        curl -L --progress-bar -o "$dest" "$url"
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

print_banner

# Check we have something to download with
DOWNLOADER=$(check_downloader)
if [[ -z "$DOWNLOADER" ]]; then
    echo "  Error: neither wget nor curl found. Install one and try again."
    exit 1
fi
echo "  Using: $DOWNLOADER"
echo ""

# Create models directory
mkdir -p "$MODELS_DIR"
echo "  Models will be saved to: $MODELS_DIR"
echo ""

# Print menu
echo "  Available models:"
echo ""
for i in "${!MODELS[@]}"; do
    IFS='|' read -r name _ _ size <<< "${MODELS[$i]}"
    printf "  [%d] %s\n" "$((i+1))" "$name"
done
echo ""
echo "  [c] Enter a custom HuggingFace URL"
echo "  [q] Quit"
echo ""

read -r -p "  Choose a model: " choice

# Handle quit
if [[ "$choice" == "q" || "$choice" == "Q" ]]; then
    echo "  Bye!"
    exit 0
fi

# Handle custom URL
if [[ "$choice" == "c" || "$choice" == "C" ]]; then
    read -r -p "  HuggingFace URL: " custom_url
    filename=$(basename "$custom_url")
    dest="$MODELS_DIR/$filename"
    echo ""
    echo "  Downloading $filename ..."
    download_file "$custom_url" "$dest" "$DOWNLOADER"
    echo ""
    echo "  Saved to: $dest"
    echo ""
    echo "  Run the server with:"
    echo "    ./build/www --model $dest --port 8080"
    exit 0
fi

# Validate numeric choice
if ! [[ "$choice" =~ ^[0-9]+$ ]] || (( choice < 1 || choice > ${#MODELS[@]} )); then
    echo "  Invalid choice. Exiting."
    exit 1
fi

idx=$((choice - 1))
IFS='|' read -r name filename repo size <<< "${MODELS[$idx]}"

dest="$MODELS_DIR/$filename"
url="https://huggingface.co/${repo}/resolve/main/${filename}"

echo ""
echo "  Model : $name"
echo "  Size  : $size"
echo "  Dest  : $dest"
echo ""

# Skip download if file already exists and looks complete
if [[ -f "$dest" ]]; then
    actual_size=$(du -sh "$dest" 2>/dev/null | cut -f1)
    echo "  File already exists ($actual_size on disk)."
    read -r -p "  Re-download? [y/N]: " confirm
    if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
        echo ""
        echo "  Using existing file."
        echo ""
        echo "  Run the server with:"
        echo "    ./build/www --model $dest --port 8080"
        exit 0
    fi
fi

echo "  Downloading..."
echo ""
download_file "$url" "$dest" "$DOWNLOADER"

# Verify the file is not empty / suspiciously small (< 100 MB = likely an error page)
file_bytes=$(wc -c < "$dest" 2>/dev/null || echo 0)
if (( file_bytes < 100000000 )); then
    echo ""
    echo "  Warning: downloaded file seems too small ($(du -sh "$dest" | cut -f1))."
    echo "  It may be an error page from HuggingFace (some models require login)."
    echo "  Check the file and try downloading manually if needed."
    exit 1
fi

echo ""
echo "  Done! Saved to: $dest"
echo ""
echo "  Build the server (from the www/ directory):"
echo "    mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
echo ""
echo "  Run the server:"
echo "    ./build/www --model $dest --port 8080 --threads \$(nproc)"
echo ""
